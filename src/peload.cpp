// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// A loader for 64-bit Windows programs inside an Xbox Dev Mode app (or a PC program, for testing): maps the .exe
// into our own memory, applies its relocations, connects its imports and runs its TLS callbacks and entry point on a
// thread of its own. Imports come from, in order:
//   1. our stand-ins (shims.cpp): calls an Xbox app does not have (windows, message boxes, process exit, ...);
//   2. the real system DLLs already loaded in our process (kernelbase / kernel32 / ucrtbase / ...), where an app may
//      call them;
//   3. otherwise a small stub per missing function that logs its name the first time and returns 0, so a game gets
//      as far as it can and the log lists what is still missing.

#include "peload.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <csetjmp>
#include <cstdarg>
#include <algorithm>

namespace arcade {

static void (*g_log)(const char*) = nullptr;
void SetLog(void (*fn)(const char*)) { g_log = fn; }
void Log(const char* fmt, ...) {
    // the program's last error (and Winsock's, the same slot) kept: a game reads it right after the call we logged
    // in (Caladrius: GetLastError after a non-blocking connect saw the log file's own result instead of 10035)
    struct KeepError {
        DWORD e = GetLastError();
        ~KeepError() { SetLastError(e); }
    } keep;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(buf);
}

#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
static void* AllocExec(size_t size) {
    return VirtualAllocFromApp(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static bool Protect(void* p, size_t size, ULONG prot) {
    ULONG old;
    return VirtualProtectFromApp(p, size, prot, &old) != 0;
}
static HMODULE SystemModule(const char* name) {
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
    return GetModuleHandleW(w);   // only DLLs already in the process: an app cannot load system DLLs by name
}
#else
static void* AllocExec(size_t size) { return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static bool Protect(void* p, size_t size, ULONG prot) {
    DWORD old;
    return VirtualProtect(p, size, prot, &old) != 0;
}
static HMODULE SystemModule(const char* name) {
    HMODULE m = GetModuleHandleA(name);
    return m ? m : LoadLibraryA(name);
}
#endif

// --- stubs for missing imports: a small piece of code per function that calls MissingImport(index) -----------
struct Missing {
    std::string name;
    bool told = false;
};
static std::vector<Missing> g_missing;
static std::mutex g_missingLock;
static uint64_t MissingImport(uint64_t index) {
    std::lock_guard<std::mutex> l(g_missingLock);
    if (index < g_missing.size() && !g_missing[index].told) {
        g_missing[index].told = true;
        Log("missing import called: %s (returns 0)", g_missing[index].name.c_str());
    }
    return 0;
}
static uint8_t* g_stubPage = nullptr;
static size_t g_stubUsed = 0, g_stubSize = 0;
static void* MakeStub(const std::string& name) {
    // mov rcx, index ; mov rax, MissingImport ; jmp rax   (shadow space and stack stay the caller's)
    const size_t kStub = 32;
    if (!g_stubPage || g_stubUsed + kStub > g_stubSize) {
        g_stubSize = 64 * 1024;
        g_stubPage = (uint8_t*)AllocExec(g_stubSize);
        g_stubUsed = 0;
        if (!g_stubPage) return nullptr;
    }
    uint64_t index;
    {
        std::lock_guard<std::mutex> l(g_missingLock);
        index = g_missing.size();
        g_missing.push_back({name});
    }
    uint8_t* p = g_stubPage + g_stubUsed;
    Protect(g_stubPage, g_stubSize, PAGE_READWRITE);
    uint8_t code[kStub] = {0x48, 0xB9};                       // mov rcx, imm64
    memcpy(code + 2, &index, 8);
    code[10] = 0x48; code[11] = 0xB8;                          // mov rax, imm64
    uint64_t fn = (uint64_t)&MissingImport;
    memcpy(code + 12, &fn, 8);
    code[20] = 0xFF; code[21] = 0xE0;                          // jmp rax
    memcpy(p, code, kStub);
    g_stubUsed += kStub;
    Protect(g_stubPage, g_stubSize, PAGE_EXECUTE_READ);
    FlushInstructionCache(GetCurrentProcess(), p, kStub);
    return p;
}


// --- tracing (testing): with SetTrace(true), every import goes through a small piece of code that logs its name the
// first 3 times it is called, then jumps to the real function (arguments, float ones too, kept as they were)
static bool g_trace = false;
extern bool g_trace32;
void SetTrace(bool on) { g_trace = on; g_trace32 = on; }
struct Traced {
    std::string name;
    int calls = 0;
};
static std::vector<Traced> g_traced;
static void TraceCall(uint64_t index) {
    std::lock_guard<std::mutex> l(g_missingLock);
    if (index < g_traced.size() && g_traced[index].calls < 3) {
        g_traced[index].calls++;
        Log("call: %s", g_traced[index].name.c_str());
    }
}
static void* MakeTrace(const std::string& name, void* target) {
    const size_t kStub = 128;
    if (!g_stubPage || g_stubUsed + kStub > g_stubSize) {
        g_stubSize = 256 * 1024;
        g_stubPage = (uint8_t*)AllocExec(g_stubSize);
        g_stubUsed = 0;
        if (!g_stubPage) return target;
    }
    uint64_t index;
    {
        std::lock_guard<std::mutex> l(g_missingLock);
        index = g_traced.size();
        g_traced.push_back({name});
    }
    uint8_t c[kStub];
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) c[n++] = x; };
    auto put64 = [&](uint64_t v) { memcpy(c + n, &v, 8); n += 8; };
    put({0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00});          // sub rsp, 0x88
    put({0x48, 0x89, 0x4C, 0x24, 0x20});                       // mov [rsp+20h], rcx
    put({0x48, 0x89, 0x54, 0x24, 0x28});                       // mov [rsp+28h], rdx
    put({0x4C, 0x89, 0x44, 0x24, 0x30});                       // mov [rsp+30h], r8
    put({0x4C, 0x89, 0x4C, 0x24, 0x38});                       // mov [rsp+38h], r9
    put({0x0F, 0x11, 0x44, 0x24, 0x40});                       // movups [rsp+40h], xmm0
    put({0x0F, 0x11, 0x4C, 0x24, 0x50});                       // movups [rsp+50h], xmm1
    put({0x0F, 0x11, 0x54, 0x24, 0x60});                       // movups [rsp+60h], xmm2
    put({0x0F, 0x11, 0x5C, 0x24, 0x70});                       // movups [rsp+70h], xmm3
    put({0x48, 0xB9}); put64(index);                           // mov rcx, index
    put({0x48, 0xB8}); put64((uint64_t)&TraceCall);            // mov rax, TraceCall
    put({0xFF, 0xD0});                                         // call rax
    put({0x48, 0x8B, 0x4C, 0x24, 0x20});                       // mov rcx, [rsp+20h]
    put({0x48, 0x8B, 0x54, 0x24, 0x28});                       // mov rdx, [rsp+28h]
    put({0x4C, 0x8B, 0x44, 0x24, 0x30});                       // mov r8, [rsp+30h]
    put({0x4C, 0x8B, 0x4C, 0x24, 0x38});                       // mov r9, [rsp+38h]
    put({0x0F, 0x10, 0x44, 0x24, 0x40});                       // movups xmm0, [rsp+40h]
    put({0x0F, 0x10, 0x4C, 0x24, 0x50});                       // movups xmm1, [rsp+50h]
    put({0x0F, 0x10, 0x54, 0x24, 0x60});                       // movups xmm2, [rsp+60h]
    put({0x0F, 0x10, 0x5C, 0x24, 0x70});                       // movups xmm3, [rsp+70h]
    put({0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00});          // add rsp, 0x88
    put({0x48, 0xB8}); put64((uint64_t)target);                // mov rax, target
    put({0xFF, 0xE0});                                         // jmp rax
    uint8_t* p = g_stubPage + g_stubUsed;
    Protect(g_stubPage, g_stubSize, PAGE_READWRITE);
    memcpy(p, c, n);
    g_stubUsed += kStub;
    Protect(g_stubPage, g_stubSize, PAGE_EXECUTE_READ);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return p;
}

// api-ms-win-crt-*.dll and the like are names for parts of ucrtbase / kernelbase
static const char* RealDllFor(const char* dll) {
    if (!_strnicmp(dll, "api-ms-win-crt-", 15)) return "ucrtbase.dll";
    if (!_strnicmp(dll, "api-ms-win-core-", 16)) return "kernelbase.dll";
    if (!_stricmp(dll, "msvcrt.dll")) return "ucrtbase.dll";
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    // an app has the "_app" builds of the Visual C++ runtime, with the same functions
    if (!_stricmp(dll, "vcruntime140.dll")) return "vcruntime140_app.dll";
    if (!_stricmp(dll, "vcruntime140_1.dll")) return "vcruntime140_1_app.dll";
    if (!_stricmp(dll, "msvcp140.dll")) return "msvcp140_app.dll";
    // XInput: an app has its own build of it, xinputuap.dll, with the same functions
    // COM: an app has it in combase.dll (CoInitializeEx, CoCreateInstance, CoTaskMemFree, ...)
    if (!_stricmp(dll, "ole32.dll")) return "combase.dll";
    if (!_strnicmp(dll, "xinput1_", 8) || !_stricmp(dll, "xinput9_1_0.dll")) return "xinputuap.dll";
#endif
    return dll;
}

// --- 64-bit DLLs mapped like the program: the program's own (beside it) and ours (DXVK's d3d9.dll in the
// package's x64 folder). Their imports go through the same steps as the program's; their exports answer the
// program's imports, LoadLibrary and GetProcAddress. -----------------------------------------------------------
struct Dll64 {
    std::string name;
    std::wstring path;
    LoadedImage img;
};
static std::vector<Dll64*> g_dll64;
static std::vector<std::wstring> g_dllDirs64;
static std::vector<std::string> g_notFound64;
static std::recursive_mutex g_dllLock64;
static void RunTlsCallbacks(const LoadedImage& img, DWORD reason);
static void* Resolve(const char* dll, const char* func, uint16_t ordinal, bool byOrdinal, int& fromShim);

void AddDllFolder64(const std::wstring& dir) {
    std::wstring d = dir;
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    std::lock_guard<std::recursive_mutex> l(g_dllLock64);
    for (auto& x : g_dllDirs64)
        if (!_wcsicmp(x.c_str(), d.c_str())) return;
    g_dllDirs64.push_back(d);
    g_notFound64.clear();
}
static std::vector<uint8_t> ReadAll64(const std::wstring& path) {
    std::vector<uint8_t> v;
    if (NetFsReadAll(path, v)) return v;   // (a game played from the NAS)
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
#else
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
#endif
    if (h == INVALID_HANDLE_VALUE) return v;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 31)) {
        v.resize((size_t)size.QuadPart);
        DWORD n = 0;
        if (!ReadFile(h, v.data(), (DWORD)v.size(), &n, nullptr) || n != v.size()) v.clear();
    }
    CloseHandle(h);
    return v;
}
static bool IsPe64(const std::vector<uint8_t>& f) {
    if (f.size() < 0x400) return false;
    auto dos = (const IMAGE_DOS_HEADER*)f.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > f.size()) return false;
    auto nt = (const IMAGE_NT_HEADERS64*)(f.data() + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
}
static std::string DllName(const char* p) {
    std::string s = p ? p : "";
    size_t k = s.find_last_of("\\/");
    if (k != std::string::npos) s = s.substr(k + 1);
    if (s.find('.') == std::string::npos) s += ".dll";
    return s;
}
// an export by name or ordinal; forwarders ("OTHER.Function") are resolved like an import
static void* Export64(const LoadedImage& img, const char* func, uint16_t ordinal, bool byOrd) {
    uint8_t* b = img.base;
    auto nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.Size) return nullptr;
    auto ed = (IMAGE_EXPORT_DIRECTORY*)(b + dd.VirtualAddress);
    auto funcs = (DWORD*)(b + ed->AddressOfFunctions);
    DWORD idx = 0xFFFFFFFF;
    if (byOrd) {
        idx = (DWORD)ordinal - ed->Base;
    } else {
        auto names = (DWORD*)(b + ed->AddressOfNames);
        auto ords = (WORD*)(b + ed->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ed->NumberOfNames; i++)
            if (!strcmp((const char*)(b + names[i]), func)) {
                idx = ords[i];
                break;
            }
    }
    if (idx >= ed->NumberOfFunctions || !funcs[idx]) return nullptr;
    DWORD rva = funcs[idx];
    if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) {
        std::string fw = (const char*)(b + rva);
        size_t dot = fw.find('.');
        if (dot == std::string::npos) return nullptr;
        std::string dll = fw.substr(0, dot) + ".dll", fn = fw.substr(dot + 1);
        int s;
        if (!fn.empty() && fn[0] == '#') return Resolve(dll.c_str(), "", (uint16_t)atoi(fn.c_str() + 1), true, s);
        return Resolve(dll.c_str(), fn.c_str(), 0, false, s);
    }
    return b + rva;
}
// the name of an export given by ordinal, if it has one
static const char* ExportName64(const LoadedImage& img, uint16_t ordinal) {
    uint8_t* b = img.base;
    auto nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.Size) return nullptr;
    auto ed = (IMAGE_EXPORT_DIRECTORY*)(b + dd.VirtualAddress);
    auto names = (DWORD*)(b + ed->AddressOfNames);
    auto ords = (WORD*)(b + ed->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ed->NumberOfNames; i++)
        if (ords[i] + ed->Base == ordinal) return (const char*)(b + names[i]);
    return nullptr;
}
static Dll64* FindDll64(HMODULE m) {
    std::lock_guard<std::recursive_mutex> l(g_dllLock64);
    for (Dll64* d : g_dll64)
        if ((uint8_t*)m == d->img.base) return d;
    return nullptr;
}
static Dll64* LoadedDll64(const std::string& n) {
    std::lock_guard<std::recursive_mutex> l(g_dllLock64);
    for (Dll64* d : g_dll64)
        if (!_stricmp(d->name.c_str(), n.c_str())) return d;
    return nullptr;
}
static Dll64* LoadDll64(const char* dll) {
    std::string n = DllName(dll);
    std::lock_guard<std::recursive_mutex> l(g_dllLock64);
    if (Dll64* d = LoadedDll64(n)) return d;
    for (auto& x : g_notFound64)
        if (!_stricmp(x.c_str(), n.c_str())) return nullptr;
    for (const std::wstring& dir : g_dllDirs64) {
        std::wstring path = dir + std::wstring(n.begin(), n.end());
        std::vector<uint8_t> bytes = ReadAll64(path);
        if (bytes.empty()) continue;
        if (!IsPe64(bytes)) {
            Log("%s beside the program is not a 64-bit DLL: skipped", n.c_str());
            continue;
        }
        Log("--- loading the DLL %s", n.c_str());
        Dll64* d = new Dll64{n, path, {}};
        if (!MapProgram(bytes, d->img)) {
            Log("%s could not be loaded", n.c_str());
            delete d;
            break;
        }
        g_dll64.push_back(d);
        auto nt = (IMAGE_NT_HEADERS64*)(d->img.base + ((IMAGE_DOS_HEADER*)d->img.base)->e_lfanew);
        if (d->img.tls) {
            RunTlsCallbacks(d->img, DLL_PROCESS_ATTACH);
        }
        if (nt->OptionalHeader.AddressOfEntryPoint) {
            auto dllMain = (BOOL(WINAPI*)(void*, DWORD, void*))d->img.entry;
            BOOL ok = dllMain(d->img.base, DLL_PROCESS_ATTACH, nullptr);
            Log("--- %s ready (DllMain %s)", n.c_str(), ok ? "ok" : "FAILED");
        }
        return d;
    }
    g_notFound64.push_back(n);
    return nullptr;
}
void* Dll64Load(const char* name) {
    Dll64* d = LoadDll64(name);
    return d ? d->img.base : nullptr;
}
void* Dll64Loaded(const char* name) {
    Dll64* d = LoadedDll64(DllName(name));
    return d ? d->img.base : nullptr;
}
bool Dll64Export(void* m, const char* func, void** out) {
    Dll64* d = FindDll64((HMODULE)m);
    if (!d) return false;
    bool byOrd = (uintptr_t)func <= 0xFFFF;
    *out = Export64(d->img, byOrd ? "" : func, (uint16_t)(uintptr_t)func, byOrd);
    return true;
}
bool Dll64Path(void* m, std::wstring& path) {
    Dll64* d = FindDll64((HMODULE)m);
    if (d) path = d->path;
    return d != nullptr;
}

static void* Resolve(const char* dll, const char* func, uint16_t ordinal, bool byOrdinal, int& fromShim) {
    fromShim = 0;
    if (!byOrdinal) {
        if (void* s = FindShim(dll, func)) {
            fromShim = 1;
            return s;
        }
    }
    if (Dll64* d = LoadDll64(dll)) {
        if (byOrdinal) {   // a stand-in by the export's name (hasp_login is imported as ordinal 13)
            if (const char* name = ExportName64(d->img, ordinal))
                if (void* s = FindShim(dll, name)) {
                    fromShim = 1;
                    return s;
                }
        }
        void* p = Export64(d->img, func, ordinal, byOrdinal);
        if (p) fromShim = 1;
        return p;
    }
    if (byOrdinal)
        if (const char* name = KnownOrdinalName(dll, ordinal)) {
            if (void* s = FindShim(dll, name)) {
                fromShim = 1;
                return s;
            }
            // by name from here on: the app's own build of a DLL (xinputuap.dll) numbers its exports differently
            func = name;
            byOrdinal = false;
        }
    const char* real = RealDllFor(dll);
    HMODULE m = SystemModule(real);
    if (!m && !_stricmp(real, "kernel32.dll")) m = SystemModule("kernelbase.dll");
    if (m) {
        FARPROC p = GetProcAddress(m, byOrdinal ? (LPCSTR)(uintptr_t)ordinal : func);
        if (!p && !_stricmp(real, "kernel32.dll")) {
            if (HMODULE kb = SystemModule("kernelbase.dll")) p = GetProcAddress(kb, byOrdinal ? (LPCSTR)(uintptr_t)ordinal : func);
        }
        if (p) return (void*)p;
    }
    return nullptr;
}

// --- THREAD-LOCAL DATA (__declspec(thread) / thread_local) of a module we map. Windows finds a module's block at
// TEB.ThreadLocalStoragePointer[module's TLS index] (code: mov reg, gs:[0x58]; mov reg, [reg + index*8]); that
// array belongs to Windows' own loader and has no room for our modules. So each module of ours with thread data
// gets a TlsAlloc slot, its "gs:[0x58]" reads are pointed at the slot (TEB.TlsSlots[slot], gs:[0x1480 + slot*8]),
// and its index is 0: the slot holds a one-entry array with the thread's block. A thread that has no block yet reads
// address 0 there; ModuleTlsFault makes the block on the spot and the read is done again.
struct TlsModule {
    uint8_t *lo, *hi;
    DWORD slot;
    std::vector<uint8_t> tmpl;   // the initial data (copied before the module's code runs)
    size_t zeroFill;
};
static std::vector<TlsModule*> g_tlsModules;
static SRWLOCK g_tlsLock = SRWLOCK_INIT;
static void* ThreadTlsFor(const TlsModule& t) {
    size_t size = t.tmpl.size() + t.zeroFill;
    auto block = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 64);
    auto arr = (void**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
    if (!block || !arr) return nullptr;
    memcpy(block, t.tmpl.data(), t.tmpl.size());
    arr[0] = block;
    TlsSetValue(t.slot, arr);
    return arr;
}
static LONG CALLBACK ModuleTlsFault(PEXCEPTION_POINTERS e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* rip = (uint8_t*)e->ContextRecord->Rip;
    TlsModule* t = nullptr;
    AcquireSRWLockShared(&g_tlsLock);
    for (TlsModule* m : g_tlsModules)
        if (rip >= m->lo && rip < m->hi) t = m;
    ReleaseSRWLockShared(&g_tlsLock);
    if (!t || TlsGetValue(t->slot)) return EXCEPTION_CONTINUE_SEARCH;
    // the read: [REX] 8B modrm (not a register operand) [sib]; the register holding the empty slot is its base
    uint8_t* p = rip;
    uint8_t rex = 0;
    if ((*p & 0xF0) == 0x40) rex = *p++;
    if (*p++ != 0x8B) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t modrm = *p++;
    if ((modrm & 0xC0) == 0xC0) return EXCEPTION_CONTINUE_SEARCH;
    int base = (modrm & 7) == 4 ? (*p & 7) : (modrm & 7);
    base |= (rex & 1) << 3;
    DWORD64* regs = &e->ContextRecord->Rax;   // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15, in encoding order
    if (regs[base] != 0) return EXCEPTION_CONTINUE_SEARCH;
    void* arr = ThreadTlsFor(*t);
    if (!arr) return EXCEPTION_CONTINUE_SEARCH;
    regs[base] = (DWORD64)arr;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static bool IsModuleTlsFault(PEXCEPTION_POINTERS e) {   // (the crash log stays quiet about these)
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return false;
    uint8_t* rip = (uint8_t*)e->ContextRecord->Rip;
    bool in = false;
    AcquireSRWLockShared(&g_tlsLock);
    for (TlsModule* m : g_tlsModules)
        if (rip >= m->lo && rip < m->hi && !TlsGetValue(m->slot)) in = true;
    ReleaseSRWLockShared(&g_tlsLock);
    return in;
}
static void SetupModuleTls(uint8_t* base, IMAGE_NT_HEADERS64* nt) {
    const auto& td = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!td.Size) return;
    auto tls = (IMAGE_TLS_DIRECTORY64*)(base + td.VirtualAddress);
    if (tls->AddressOfIndex) *(DWORD*)tls->AddressOfIndex = 0;
    size_t raw = (size_t)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    if (!raw && !tls->SizeOfZeroFill) return;
    auto t = new TlsModule;
    t->lo = base;
    t->hi = base + nt->OptionalHeader.SizeOfImage;
    t->tmpl.assign((uint8_t*)tls->StartAddressOfRawData, (uint8_t*)tls->StartAddressOfRawData + raw);
    t->zeroFill = tls->SizeOfZeroFill;
    t->slot = TlsAlloc();
    if (t->slot == TLS_OUT_OF_INDEXES || t->slot >= 64) {
        Log("thread-local data: no TLS slot below 64 (got %lu)", t->slot);
        delete t;
        return;
    }
    // point every "mov reg, gs:[0x58]" in the code at the slot
    uint32_t disp = 0x1480 + t->slot * 8;
    int sites = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* c = base + sec[i].VirtualAddress;
        size_t n = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        for (size_t k = 0; k + 9 <= n; k++) {
            if (c[k] != 0x65 || (c[k + 1] & 0xF0) != 0x40 || c[k + 2] != 0x8B) continue;
            if ((c[k + 3] & 0xC7) != 0x04 || c[k + 4] != 0x25) continue;
            if (c[k + 5] != 0x58 || c[k + 6] || c[k + 7] || c[k + 8]) continue;
            memcpy(c + k + 5, &disp, 4);
            sites++;
        }
    }
    AcquireSRWLockExclusive(&g_tlsLock);
    g_tlsModules.push_back(t);
    ReleaseSRWLockExclusive(&g_tlsLock);
    static bool handler = false;
    if (!handler) {   // not declared for apps, but kernelbase has it
        typedef PVOID(WINAPI * add_t)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        auto add = (add_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "AddVectoredExceptionHandler");
        handler = add && add(1, ModuleTlsFault) != nullptr;
    }
    ThreadTlsFor(*t);   // this thread's block now (the module's DllMain / entry point runs here)
    Log("thread-local data: %zu bytes, slot %lu, %d code sites", raw + (size_t)tls->SizeOfZeroFill, t->slot, sites);
}

static std::mutex g_imagesLock;
static std::vector<std::pair<uint8_t*, size_t>> g_images;
void* MappedImageBase(const void* pc) {
    std::lock_guard<std::mutex> l(g_imagesLock);
    for (auto& im : g_images)
        if ((const uint8_t*)pc >= im.first && (const uint8_t*)pc < im.first + im.second) return im.first;
    return nullptr;
}

const char* MappedImageName(const void* pc) {
    void* b = MappedImageBase(pc);
    if (!b) return "other";
    std::lock_guard<std::recursive_mutex> l(g_dllLock64);
    for (Dll64* d : g_dll64)
        if (d->img.base == b) return d->name.c_str();
    return "program";
}

bool MapProgram(const std::vector<uint8_t>& file, LoadedImage& out) {
    if (file.size() < 0x400) return false;
    auto dos = (const IMAGE_DOS_HEADER*)file.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        Log("not a Windows program (no MZ/PE header)");
        return false;
    }
    auto nt = (const IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        Log("not a 64-bit Windows program (machine 0x%x): 32-bit games need the 32-bit bridge (not built yet)",
            nt->FileHeader.Machine);
        return false;
    }
    const auto& oh = nt->OptionalHeader;
    size_t size = oh.SizeOfImage;
    uint8_t* base = (uint8_t*)AllocExec(size);
    if (!base) {
        Log("no memory for the program (%zu bytes)", size);
        return false;
    }
    memcpy(base, file.data(), oh.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].SizeOfRawData && sec[i].PointerToRawData + sec[i].SizeOfRawData <= file.size())
            memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData,
                   std::min<size_t>(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    }
    // relocations (we never get the preferred base: our own memory)
    int64_t delta = (int64_t)(uintptr_t)base - (int64_t)oh.ImageBase;
    const auto& rd = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && rd.Size) {
        uint8_t* p = base + rd.VirtualAddress, *end = p + rd.Size;
        int count = 0;
        while (p < end) {
            auto blk = (IMAGE_BASE_RELOCATION*)p;
            if (!blk->SizeOfBlock) break;
            auto ent = (uint16_t*)(blk + 1);
            int n = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
            for (int k = 0; k < n; k++) {
                int type = ent[k] >> 12, off = ent[k] & 0xFFF;
                if (type == IMAGE_REL_BASED_DIR64) {
                    *(uint64_t*)(base + blk->VirtualAddress + off) += delta;
                    count++;
                } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                    *(uint32_t*)(base + blk->VirtualAddress + off) += (uint32_t)delta;
                    count++;
                }
            }
            p += blk->SizeOfBlock;
        }
        Log("relocations: %d", count);
    } else if (delta && (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)) {
        Log("the program has no relocations and must sit at 0x%llx: not supported yet", oh.ImageBase);
        return false;
    }
    // imports
    const auto& id = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int shims = 0, real = 0, missing = 0;
    if (id.Size) {
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + id.VirtualAddress); imp->Name; imp++) {
            const char* dll = (const char*)(base + imp->Name);
            auto thunk = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
            auto names = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            for (; names->u1.AddressOfData; names++, thunk++) {
                bool byOrd = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal);
                uint16_t ord = (uint16_t)IMAGE_ORDINAL64(names->u1.Ordinal);
                const char* fn = byOrd ? "" : (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
                int fromShim;
                void* p = Resolve(dll, fn, ord, byOrd, fromShim);
                std::string full = std::string(dll) + "!" + (byOrd ? "#" + std::to_string(ord) : std::string(fn));
                if (!p) {
                    p = MakeStub(full);
                    missing++;
                    out.missing.push_back(full);
                } else if (fromShim) {
                    shims++;
                } else {
                    real++;
                }
                if (g_trace && p) p = MakeTrace(full, p);
                thunk->u1.Function = (uint64_t)p;
            }
        }
    }
    Log("imports: %d from the system, %d stand-ins, %d missing", real, shims, missing);
    for (auto& m : out.missing) Log("  missing: %s", m.c_str());
    SetupModuleTls(base, (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew));
    // section protections
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD c = sec[i].Characteristics;
        bool x = c & IMAGE_SCN_MEM_EXECUTE, w = c & IMAGE_SCN_MEM_WRITE;
        ULONG prot = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (w ? PAGE_READWRITE : PAGE_READONLY);
        size_t vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (vs && !Protect(base + sec[i].VirtualAddress, vs, prot))
            Log("section %.8s: protection 0x%x refused (%lu)", (const char*)sec[i].Name, prot, GetLastError());
    }
    FlushInstructionCache(GetCurrentProcess(), base, size);
    // exception tables, so C++ exceptions and SEH in the program work
    const auto& ed = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (ed.Size) {
        if (!RtlAddFunctionTable((PRUNTIME_FUNCTION)(base + ed.VirtualAddress), ed.Size / sizeof(RUNTIME_FUNCTION),
                                 (DWORD64)base))
            Log("exception table not added");
    }
    out.base = base;
    out.size = size;
    {   // every mapped image, for "which module is this address in" (RtlPcToFileHeader)
        std::lock_guard<std::mutex> l(g_imagesLock);
        g_images.push_back({base, size});
    }
    out.entry = base + oh.AddressOfEntryPoint;
    out.tls = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size ? base + oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress : nullptr;
    Log("loaded at %p, %zu bytes, entry +0x%x", base, size, oh.AddressOfEntryPoint);
    return true;
}

// TLS: the program's thread-local block is not set up by us (needs the loader's TLS slot); only callbacks are run.
static void RunTlsCallbacks(const LoadedImage& img, DWORD reason) {
    if (!img.tls) return;
    auto tls = (IMAGE_TLS_DIRECTORY64*)img.tls;
    if (!tls->AddressOfCallBacks) return;
    for (auto cb = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks; *cb; cb++) (*cb)(img.base, reason, nullptr);
}

struct RunArgs {
    LoadedImage img;
};
static thread_local jmp_buf* t_exit = nullptr;
static thread_local int t_exitCode = 0;
void ExitProgram(int code) {
    if (t_exit) {
        t_exitCode = code;
        longjmp(*t_exit, 1);
    }
    ExitThread(code);
}

// first sight of any crash, before anything can end the app: code, where (as program+offset when it is in the
// program), and the instruction pointer's module otherwise
static const LoadedImage* g_running = nullptr;
static LONG CALLBACK WatchCrash(PEXCEPTION_POINTERS e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 || code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;
    if (IsModuleTlsFault(e)) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* at = (uint8_t*)e->ExceptionRecord->ExceptionAddress;
    if (g_running && at >= g_running->base && at < g_running->base + g_running->size)
        Log("exception 0x%08lx at program+0x%llx (read/write address %p)", code, (unsigned long long)(at - g_running->base),
            e->ExceptionRecord->NumberParameters > 1 ? (void*)e->ExceptionRecord->ExceptionInformation[1] : nullptr);
    else {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &m))
            GetModuleFileNameA(m, name, MAX_PATH);
        Log("exception 0x%08lx at %p in %s+0x%llx, return address on stack %p", code, at, name,
            m ? (unsigned long long)(at - (uint8_t*)m) : 0ull, *(void**)e->ContextRecord->Rsp);
        // where it came from: return addresses found on the stack (module+offset), and the access address
        Log("  access %s %p, rcx=%p rdx=%p", e->ExceptionRecord->NumberParameters > 0 && e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            e->ExceptionRecord->NumberParameters > 1 ? (void*)e->ExceptionRecord->ExceptionInformation[1] : nullptr,
            (void*)e->ContextRecord->Rcx, (void*)e->ContextRecord->Rdx);
        {   // real unwind (the system modules carry unwind data)
            typedef PRUNTIME_FUNCTION(NTAPI * lookup_t)(DWORD64, PDWORD64, PVOID);
            typedef PVOID(NTAPI * unwind_t)(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PVOID*, PDWORD64, PVOID);
            HMODULE nt = GetModuleHandleW(L"ntdll.dll");
            auto lookup = (lookup_t)GetProcAddress(nt, "RtlLookupFunctionEntry");
            auto unwind = (unwind_t)GetProcAddress(nt, "RtlVirtualUnwind");
            CONTEXT c = *e->ContextRecord;
            for (int f = 0; lookup && unwind && f < 24 && c.Rip; f++) {
                HMODULE fm = nullptr;
                char fn[MAX_PATH] = "?";
                uintptr_t base = 0;
                if (uint8_t* b = (uint8_t*)MappedImageBase((void*)c.Rip)) {
                    strncpy(fn, MappedImageName((void*)c.Rip), MAX_PATH - 1);
                    base = (uintptr_t)b;
                } else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)c.Rip, &fm)) {
                    GetModuleFileNameA(fm, fn, MAX_PATH);
                    base = (uintptr_t)fm;
                }
                const char* sl = strrchr(fn, '\\');
                Log("  frame %d: %s+0x%llx", f, sl ? sl + 1 : fn, (unsigned long long)(c.Rip - base));
                DWORD64 ib = 0;
                PRUNTIME_FUNCTION rf = lookup(c.Rip, &ib, nullptr);
                if (!rf) {   // leaf: return address at rsp
                    c.Rip = *(DWORD64*)c.Rsp;
                    c.Rsp += 8;
                    continue;
                }
                PVOID hd = nullptr;
                DWORD64 ef = 0;
                unwind(0, ib, c.Rip, rf, &c, &hd, &ef, nullptr);
            }
        }
        uintptr_t* sp = (uintptr_t*)e->ContextRecord->Rsp;
        int shown = 0;
        for (int i = 0; i < 1024 && shown < 16; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (((uintptr_t)(sp + i) & 0xFFF) == 0 && (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)) break;
            uintptr_t v = sp[i];
            if (v < 0x10000) continue;
            HMODULE mm = nullptr;
            char nm[MAX_PATH] = "";
            if (uint8_t* b = (uint8_t*)MappedImageBase((void*)v)) {
                Log("  stack+%x: %s+0x%llx", i * 8, MappedImageName((void*)v), (unsigned long long)(v - (uintptr_t)b));
                shown++;
                continue;
            }
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &mm) &&
                GetModuleFileNameA(mm, nm, MAX_PATH)) {
                const char* s = strrchr(nm, '\\');
                Log("  stack+%x: %s+0x%llx", i * 8, s ? s + 1 : nm, (unsigned long long)(v - (uintptr_t)mm));
                shown++;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void RunEntry(RunArgs* a);
static DWORD WINAPI RunThread(void* p) {
    auto a = (RunArgs*)p;
    static bool watching = false;
    if (!watching) {   // not declared for apps, but kernelbase has it
        typedef PVOID(WINAPI * add_t)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        auto add = (add_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "AddVectoredExceptionHandler");
        watching = add && add(1, WatchCrash) != nullptr;
    }
    g_running = &a->img;
    __try {
        RunEntry(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("the program crashed: exception 0x%08lx (the loader goes on)", GetExceptionCode());
    }
    delete a;
    return 0;
}
static void RunEntry(RunArgs* a) {
    jmp_buf jb;
    t_exit = &jb;
    if (setjmp(jb) == 0) {
        RunTlsCallbacks(a->img, DLL_PROCESS_ATTACH);
        Log("entry point starts");
        auto entry = (int (*)())a->img.entry;
        int r = entry();
        Log("entry point returned %d", r);
    } else {
        Log("the program ended (exit code %d)", t_exitCode);
    }
    t_exit = nullptr;
}

bool Run(const LoadedImage& img, bool wait) {
    auto a = new RunArgs{img};
    HANDLE h = CreateThread(nullptr, 16 << 20, RunThread, a, 0, nullptr);
    if (!h) return false;
    if (wait) WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return true;
}

}  // namespace arcade
