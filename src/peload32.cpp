// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// THE 32-BIT BRIDGE (most PC arcade games are 32-bit; an Xbox runs 64-bit apps only). A 64-bit process can run
// 32-bit x86 code directly: with the 32-bit code segment (0x23) the CPU is in compatibility mode, and 0x33 brings it
// back. The 32-bit program is mapped below 4 GB; it runs on its own 32-bit stack, with FS pointing at a 32-bit thread
// block of ours (WRFSBASE before the switch; 32-bit code uses FS:[0] for its exception frames). Each function it
// imports becomes a few bytes of 32-bit code that note which one it is and jump to 64-bit code; that code moves to the
// 64-bit stack and calls Dispatch, which reads the arguments from the 32-bit stack, calls a stand-in or the real
// 64-bit function (each argument widened to 64 bits) and goes back to the 32-bit caller with the result in EAX,
// removing the arguments as stdcall does.
// One thread for now; callbacks into 32-bit code (window procedures, threads) come later.

#include "peload.h"
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <cstdio>
#include <algorithm>
#include <initializer_list>

namespace arcade {

#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
static void* LowAlloc(void* at, size_t size) { return VirtualAllocFromApp(at, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static bool LowProtect(void* p, size_t size, ULONG prot) {
    ULONG old;
    return VirtualProtectFromApp(p, size, prot, &old) != 0;
}
#else
static void* LowAlloc(void* at, size_t size) { return VirtualAlloc(at, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static bool LowProtect(void* p, size_t size, ULONG prot) {
    DWORD old;
    return VirtualProtect(p, size, prot, &old) != 0;
}
#endif

// where threads' 32-bit thread blocks will go: the low 32 bits of their 64-bit TEBs, which Windows places close to
// each other; our own memory below 2 GB keeps 128 MB clear of that spot (else a new thread finds its place taken)
static bool NearTebs(uintptr_t at, size_t size) {
    static const uintptr_t teb = (uint32_t)(uintptr_t)NtCurrentTeb();
    const uintptr_t lo = teb > (128u << 20) ? teb - (128u << 20) : 0, hi = teb + (128u << 20);
    return at < hi && at + size > lo;
}
// memory below 2 GB (see Dispatch: arguments are sign-extended): the first free place from 'from' up
// the bridge's own memory (switch code, thread stacks) from 0x60000000 first: a program image can be huge and must
// sit at its own address (Caladrius: 269 MB from 0x400000, no relocations); then from <from>
static uint8_t* AllocBelow4G(size_t size, uintptr_t from = 0x10000000) {
    for (uintptr_t at = 0x60000000; at + size < 0x70000000; at += 0x00100000)
        if (!NearTebs(at, size))
            if (void* p = LowAlloc((void*)at, size)) return (uint8_t*)p;
    for (uintptr_t at = from; at + size < 0x80000000; at += 0x00100000)
        if (!NearTebs(at, size))
            if (void* p = LowAlloc((void*)at, size)) return (uint8_t*)p;
    return nullptr;
}

// --- the functions a 32-bit program may call ------------------------------------------------------------------
struct Bridged {
    std::string name;   // dll!function, for the log
    void* target;       // 64-bit function (stand-in or real); nullptr: missing
    int args;           // 32-bit stack arguments
    bool cdecl_;        // caller removes the arguments
    int calls = 0;
};
static std::deque<Bridged> g_fn;   // a deque: entries stay put while it grows
static const LoadedImage32* g_img32 = nullptr;

// stand-ins for 32-bit callers (their arguments are 64-bit here, already widened)
static uint64_t B_GetModuleHandleA(uint64_t name) {
    if (!name) return g_img32->base;
    return 0;   // other modules: not mapped for 32-bit code yet
}
static uint64_t B_OutputDebugStringA(uint64_t s) {
    Log("32-bit program says: %s", (const char*)(uintptr_t)s);
    return 0;
}
static uint64_t B_MessageBoxA(uint64_t, uint64_t text, uint64_t title, uint64_t) {
    Log("32-bit message box \"%s\": %s", title ? (const char*)(uintptr_t)title : "", text ? (const char*)(uintptr_t)text : "");
    return IDOK;
}
static uint64_t B_ExitProcess(uint64_t code);   // ends the 32-bit program (below)
static uint64_t B_ExitThread(uint64_t code);

// --- memory a 32-bit program gets must be below 4 GB: a heap there (ntdll's RtlCreateHeap on our own reserved
// region) for HeapAlloc / LocalAlloc / GlobalAlloc / GetProcessHeap, and VirtualAlloc without an address below 4 GB
static HANDLE g_lowHeap = nullptr;
static bool LowCommit(void* at, size_t size) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    return VirtualAllocFromApp(at, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return VirtualAlloc(at, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#endif
}
static HANDLE LowHeap() {
    if (g_lowHeap) return g_lowHeap;
    typedef PVOID(NTAPI * create_t)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);
    auto create = (create_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCreateHeap");
    // the 64-bit process has some of its own memory at random places below 2 GB: the biggest free area that fits
    size_t reserve = 0;
    uint8_t* region = nullptr;
    for (size_t want : {768u << 20, 512u << 20, 256u << 20, 128u << 20})
        for (uintptr_t at = 0x20000000; at + want < 0x80000000 && !region; at += 0x00400000)
            if (!NearTebs(at, want) && (region = (uint8_t*)VirtualAlloc((void*)at, want, MEM_RESERVE, PAGE_READWRITE))) reserve = want;
    // not HEAP_GROWABLE: a growable heap gives big blocks (over ~512 KB) memory of their own anywhere, above 4 GB too;
    // this one keeps every block inside the reserved area below 2 GB
    // with a commit routine of ours, as Windows needs for a heap on memory the caller reserved
    struct HeapParams {
        ULONG Length;
        SIZE_T SegmentReserve, SegmentCommit, DeCommitFreeBlockThreshold, DeCommitTotalFreeThreshold,
            MaximumAllocationSize, VirtualMemoryThreshold, InitialCommit, InitialReserve;
        LONG(NTAPI* CommitRoutine)(PVOID base, PVOID* address, PSIZE_T size);
        SIZE_T Reserved[2];
    } params = {};
    params.Length = sizeof params;
    params.InitialCommit = 1 << 20;   // (required with a commit routine: what is committed and reserved already)
    params.InitialReserve = reserve;
    params.CommitRoutine = [](PVOID, PVOID* address, PSIZE_T size) -> LONG {
        return LowCommit(*address, *size) ? 0 : (LONG)0xC0000017;   // STATUS_NO_MEMORY
    };
    if (create && region && LowCommit(region, 1 << 20)) g_lowHeap = create(0, region, reserve, 1 << 20, nullptr, &params);
    Log("32-bit heap below 2 GB: %p (%zu MB)", g_lowHeap, reserve >> 20);
    return g_lowHeap;
}
static uint64_t B_GetProcessHeap() { return (uint64_t)LowHeap(); }
// BIG BLOCKS. Windows' heap gives a block over ~508 KB memory of its own, outside the heap's area (above 4 GB too,
// or not at all for a heap that may not grow), so those are ours: whole pages below 2 GB (B_VirtualAlloc)
static const size_t kBigBlock = 0x7F000;
static std::mutex g_bigLock;
static std::map<uint32_t, size_t> g_big;   // address -> size asked for
static uint64_t B_VirtualAlloc(uint64_t addr, uint64_t size, uint64_t type, uint64_t prot);
static uint64_t BigAlloc(size_t size) {
    uint64_t p = B_VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // (zeroed)
    if (p) {
        std::lock_guard<std::mutex> l(g_bigLock);
        g_big[(uint32_t)p] = size;
    }
    return p;
}
static size_t BigSize(uint64_t p) {
    std::lock_guard<std::mutex> l(g_bigLock);
    auto it = g_big.find((uint32_t)p);
    return it == g_big.end() ? 0 : it->second;
}
static void BigFree(uint64_t p) {
    {
        std::lock_guard<std::mutex> l(g_bigLock);
        g_big.erase((uint32_t)p);
    }
    VirtualFree((void*)(uintptr_t)(uint32_t)p, 0, MEM_RELEASE);
}
static uint64_t B_HeapAlloc(uint64_t heap, uint64_t flags, uint64_t size) {
    if (size >= kBigBlock) return BigAlloc((size_t)size);
    if (!(uint32_t)heap) {   // no heap (a runtime not set up yet): ours
        static int told = 0;
        if (told++ < 3) Log("32-bit HeapAlloc with no heap: the low heap instead");
        heap = (uint64_t)LowHeap();
    }
    return (uint64_t)HeapAlloc((HANDLE)heap, (DWORD)flags, (SIZE_T)size);
}
static uint64_t B_HeapFree(uint64_t heap, uint64_t flags, uint64_t p) {
    if (!(uint32_t)p) return TRUE;
    if (BigSize(p)) {
        BigFree(p);
        return TRUE;
    }
    return HeapFree((HANDLE)heap, (DWORD)flags, (void*)(uintptr_t)(uint32_t)p);
}
static uint64_t B_HeapSize(uint64_t heap, uint64_t flags, uint64_t p) {
    if (size_t b = BigSize(p)) return b;
    return HeapSize((HANDLE)heap, (DWORD)flags, (void*)(uintptr_t)(uint32_t)p);
}
static uint64_t B_HeapReAlloc(uint64_t heap, uint64_t flags, uint64_t p, uint64_t size) {
    size_t old = BigSize(p);
    if (!old && size < kBigBlock)
        return (uint64_t)HeapReAlloc((HANDLE)heap, (DWORD)flags, (void*)(uintptr_t)(uint32_t)p, (SIZE_T)size);
    if (flags & 0x10) return 0;   // HEAP_REALLOC_IN_PLACE_ONLY
    if (!old) old = HeapSize((HANDLE)heap, 0, (void*)(uintptr_t)(uint32_t)p);
    uint64_t n = B_HeapAlloc(heap, flags & 0x8 /*HEAP_ZERO_MEMORY*/, size);
    if (!n) return 0;
    memcpy((void*)(uintptr_t)n, (void*)(uintptr_t)(uint32_t)p, old < size ? old : (size_t)size);
    if ((flags & 0x8) && size > old) memset((uint8_t*)(uintptr_t)n + old, 0, (size_t)(size - old));
    B_HeapFree(heap, 0, p);
    return n;
}
static uint64_t B_HeapCreate(uint64_t, uint64_t, uint64_t) { return (uint64_t)LowHeap(); }
static uint64_t B_HeapDestroy(uint64_t) { return TRUE; }
static uint64_t B_LocalAlloc(uint64_t flags, uint64_t size) {
    return (uint64_t)HeapAlloc(LowHeap(), (flags & 0x40) ? HEAP_ZERO_MEMORY : 0, (SIZE_T)(size ? size : 1));
}
static uint64_t B_LocalFree(uint64_t p) {
    if (p) HeapFree(LowHeap(), 0, (void*)(uintptr_t)p);
    return 0;
}
static uint64_t B_VirtualAlloc(uint64_t addr, uint64_t size, uint64_t type, uint64_t prot) {
    if (addr) return (uint64_t)VirtualAlloc((void*)(uintptr_t)addr, (SIZE_T)size, (DWORD)type, (DWORD)prot);
    size = (size + 0xFFFF) & ~0xFFFFull;
    for (uintptr_t at = 0x50000000; at + size < 0x80000000; at += 0x10000)
        if (!NearTebs(at, (size_t)size))
            if (void* p = VirtualAlloc((void*)at, (SIZE_T)size, (DWORD)type | MEM_RESERVE, (DWORD)prot)) return (uint64_t)p;
    return 0;
}
// strings the 64-bit side keeps (maybe above 4 GB): a copy below 4 GB
// the program's own command line: the process's, without the loader's own first word (copied below 4 GB)
const std::wstring& ProgramCommandLineW();   // shims.cpp
static const wchar_t* ProgramCommandLine() {
    if (!ProgramCommandLineW().empty()) return ProgramCommandLineW().c_str();   // the game's: its path, then its arguments
    const wchar_t* c = GetCommandLineW();
    bool q = false;
    while (*c && (q || (*c != L' ' && *c != L'\t'))) {
        if (*c == L'"') q = !q;
        c++;
    }
    while (*c == L' ' || *c == L'\t') c++;
    return c;
}
static uint64_t B_GetCommandLineW() {
    static wchar_t* low = nullptr;
    if (!low) {
        const wchar_t* c = ProgramCommandLine();
        low = (wchar_t*)HeapAlloc(LowHeap(), 0, (wcslen(c) + 1) * 2);
        wcscpy(low, c);
    }
    return (uint64_t)low;
}
static uint64_t B_GetCommandLineA() {
    static char* low = nullptr;
    if (!low) {
        const wchar_t* c = ProgramCommandLine();
        int n = WideCharToMultiByte(CP_ACP, 0, c, -1, nullptr, 0, nullptr, nullptr);
        low = (char*)HeapAlloc(LowHeap(), 0, n);
        WideCharToMultiByte(CP_ACP, 0, c, -1, low, n, nullptr, nullptr);
    }
    return (uint64_t)low;
}


#include "sync32.inc"

static uint64_t B_GetModuleHandleA2(uint64_t name);
static uint64_t B_PathFileExistsA(uint64_t p);
static uint64_t B_AllocateAndInitializeSid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_FreeSid(uint64_t);
static uint64_t B_InitializeSecurityDescriptor(uint64_t, uint64_t);
static uint64_t B_SetSecurityDescriptorDacl(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_CreateMutexA32(uint64_t, uint64_t, uint64_t);
static uint64_t B_CreateMutexW32(uint64_t, uint64_t, uint64_t);
static uint64_t B_CreateEventA32(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_CreateEventW32(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_CreateSemaphoreA32(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_CreateSemaphoreW32(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_SetupDiGetClassDevsA(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_SetupDiGetClassDevsExA(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_SetupDiEnumDeviceInterfaces(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_SetupDiGetDeviceInterfaceDetailA(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_SetupDiEnumDeviceInfo(uint64_t, uint64_t, uint64_t);
static uint64_t B_SetupDiDestroyDeviceInfoList(uint64_t);
static uint64_t B_SetupDiFalse7(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_SetupDiFalse2(uint64_t, uint64_t);
static uint64_t B_SetupDiOpenDevRegKey(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_CM_Get_Device_ID_ExA(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
// a 32-bit program's vectored exception handler: not given to Windows (it would call the 32-bit function as 64-bit
// code: dxwrapper's crashed at every exception); a handle back, nothing called
static uint64_t B_AddVectoredHandler32(uint64_t, uint64_t fn) {
    static int told = 0;
    if (told++ < 3) Log("32-bit program adds a vectored exception handler %08llx (kept out)", fn);
    static uint32_t next = 0x0E000010;
    return next += 16;
}
static uint64_t B_RemoveVectoredHandler32(uint64_t) { return 1; }
// (testing) the text a game writes with GDI: each different line once (status and error screens drawn as text)
static uint64_t B_TextOutA32(uint64_t, uint64_t x, uint64_t y, uint64_t str, uint64_t n) {
    static std::mutex lock;
    static std::set<std::string> seen;
    if ((uint32_t)str && (int)n > 0 && (int)n < 400) {
        std::string t((const char*)(uintptr_t)(uint32_t)str, (size_t)(int)n);
        std::lock_guard<std::mutex> l(lock);
        if (seen.size() < 60 && seen.insert(t).second) Log("32-bit program writes text at %d,%d: \"%s\"", (int)x, (int)y, t.c_str());
    }
    return TRUE;
}
// folder searches: the console's search handles are 64-bit addresses, so the 32-bit program gets a small number of
// ours (0x0F000004..) for each; the real functions (through the loader's own file shims) do the work
static void* ResolveFor32(const std::string& dll, const std::string& fn, int& args, bool& cdecl_);
bool RedirectCabinetPath(const char* in, std::string& out);   // arcade_io.cpp
static std::mutex g_findLock;
static std::vector<HANDLE> g_finds;
static void* FindFn32(const char* name) {   // the loader's file shim (NAS, app folders), else Windows'
    if (void* f = FindShim("kernel32.dll", name)) return f;
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    void* f = k ? (void*)GetProcAddress(k, name) : nullptr;
    if (!f && (k = GetModuleHandleW(L"kernel32.dll"))) f = (void*)GetProcAddress(k, name);
    return f;
}
static uint64_t FindKeep32(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || !h) return (uint64_t)(int64_t)-1;
    std::lock_guard<std::mutex> l(g_findLock);
    for (size_t i = 0; i < g_finds.size(); i++)
        if (!g_finds[i]) {
            g_finds[i] = h;
            return 0x0F000004u + (uint32_t)i * 4u;
        }
    g_finds.push_back(h);
    return 0x0F000004u + (uint32_t)(g_finds.size() - 1) * 4u;
}
static HANDLE FindReal32(uint64_t h) {
    uint32_t i = ((uint32_t)h - 0x0F000004u) / 4u;
    std::lock_guard<std::mutex> l(g_findLock);
    return (uint32_t)h >= 0x0F000004u && i < g_finds.size() ? g_finds[i] : nullptr;
}
static uint64_t B_FindFirstFileExA32(uint64_t name, uint64_t lvl, uint64_t data, uint64_t op, uint64_t filt, uint64_t fl) {
    typedef HANDLE(WINAPI * f_t)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    static auto f = (f_t)FindFn32("FindFirstFileExA");
    const char* n = (const char*)(uintptr_t)(uint32_t)name;
    std::string cab;
    if (RedirectCabinetPath(n, cab)) n = cab.c_str();
    return f ? FindKeep32(f(n, (FINDEX_INFO_LEVELS)lvl, (void*)(uintptr_t)(uint32_t)data, (FINDEX_SEARCH_OPS)op, nullptr, (DWORD)fl))
             : (uint64_t)(int64_t)-1;
}
static uint64_t B_FindFirstFileExW32(uint64_t name, uint64_t lvl, uint64_t data, uint64_t op, uint64_t filt, uint64_t fl) {
    typedef HANDLE(WINAPI * f_t)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    static auto f = (f_t)FindFn32("FindFirstFileExW");
    return f ? FindKeep32(f((LPCWSTR)(uintptr_t)(uint32_t)name, (FINDEX_INFO_LEVELS)lvl, (void*)(uintptr_t)(uint32_t)data,
                            (FINDEX_SEARCH_OPS)op, nullptr, (DWORD)fl))
             : (uint64_t)(int64_t)-1;
}
static uint64_t B_FindFirstFileA32(uint64_t name, uint64_t data) { return B_FindFirstFileExA32(name, FindExInfoStandard, data, 0, 0, 0); }
static uint64_t B_FindFirstFileW32(uint64_t name, uint64_t data) { return B_FindFirstFileExW32(name, FindExInfoStandard, data, 0, 0, 0); }
static uint64_t B_FindNextFileA32(uint64_t h, uint64_t data) {
    typedef BOOL(WINAPI * f_t)(HANDLE, LPWIN32_FIND_DATAA);
    static auto f = (f_t)FindFn32("FindNextFileA");
    HANDLE r = FindReal32(h);
    if (!r || !f) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return f(r, (LPWIN32_FIND_DATAA)(uintptr_t)(uint32_t)data);
}
static uint64_t B_FindNextFileW32(uint64_t h, uint64_t data) {
    typedef BOOL(WINAPI * f_t)(HANDLE, LPWIN32_FIND_DATAW);
    static auto f = (f_t)FindFn32("FindNextFileW");
    HANDLE r = FindReal32(h);
    if (!r || !f) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return f(r, (LPWIN32_FIND_DATAW)(uintptr_t)(uint32_t)data);
}
static uint64_t B_FindClose32(uint64_t h) {
    typedef BOOL(WINAPI * f_t)(HANDLE);
    static auto f = (f_t)FindFn32("FindClose");
    HANDLE r = FindReal32(h);
    if (!r) return FALSE;
    {
        std::lock_guard<std::mutex> l(g_findLock);
        g_finds[((uint32_t)h - 0x0F000004u) / 4u] = nullptr;
    }
    return f ? f(r) : TRUE;
}
// DirectDraw 7 for games that only ask it how much video memory there is (Battle Fantasia: DirectDrawCreateEx,
// GetAvailableVidMem, Release): a small object of ours; its other methods say "not implemented" (a game drawing with
// DirectDraw brings a wrapper, dxwrapper, which is a 32-bit DLL of its own and comes first)
uint32_t BridgeThunk32(const char* name, void* fn, int args, bool cdecl_);
static uint64_t DD_QueryInterface(uint64_t, uint64_t, uint64_t out) {
    if ((uint32_t)out) *(uint32_t*)(uintptr_t)(uint32_t)out = 0;
    return 0x80004002;   // E_NOINTERFACE
}
static uint64_t DD_AddRef(uint64_t) { return 1; }
static uint64_t DD_Release(uint64_t) { return 0; }
static uint64_t DD_GetAvailableVidMem(uint64_t, uint64_t, uint64_t total, uint64_t free_) {
    if ((uint32_t)total) *(uint32_t*)(uintptr_t)(uint32_t)total = 512u << 20;
    if ((uint32_t)free_) *(uint32_t*)(uintptr_t)(uint32_t)free_ = 448u << 20;
    return 0;
}
static uint64_t DD_NotImpl(uint64_t) { return 0x80004001; }   // E_NOTIMPL
static uint64_t B_DirectDrawCreateEx32(uint64_t, uint64_t out, uint64_t, uint64_t) {
    static uint32_t obj = 0;
    if (!obj) {
        const int kSlots = 30;
        uint32_t* vt = (uint32_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 4 * (kSlots + 1));
        uint32_t notImpl = BridgeThunk32("ddraw 7.(not implemented)", (void*)DD_NotImpl, 1, false);
        for (int i = 0; i < kSlots; i++) vt[i] = notImpl;
        vt[0] = BridgeThunk32("ddraw 7.QueryInterface", (void*)DD_QueryInterface, 3, false);
        vt[1] = BridgeThunk32("ddraw 7.AddRef", (void*)DD_AddRef, 1, false);
        vt[2] = BridgeThunk32("ddraw 7.Release", (void*)DD_Release, 1, false);
        vt[23] = BridgeThunk32("ddraw 7.GetAvailableVidMem", (void*)DD_GetAvailableVidMem, 4, false);
        uint32_t* o = (uint32_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 16);
        o[0] = (uint32_t)(uintptr_t)vt;
        obj = (uint32_t)(uintptr_t)o;
        Log("32-bit DirectDraw 7: an object of ours (video memory questions only)");
    }
    if ((uint32_t)out) *(uint32_t*)(uintptr_t)(uint32_t)out = obj;
    return 0;
}
// SetupAPI: Sega's amLib (RingEdge) finds its drivers by device interface class, then opens the path it is given.
// Each list of ours holds one device of the class asked for; its path is the driver ringedge.cpp emulates (by the
// class's GUID; unknown classes are logged and get \.\mxsram, the SRAM amBackup needs). Lists: 0x5E700010 + 16*n.
struct DevList32 { GUID g; };
static std::vector<DevList32> g_devLists;
static const char* DevPathFor(const GUID& g) {
    static const GUID kSmbus = {0x5C49E1FE, 0x3FEC, 0x4B8D, {0xA4, 0xB5, 0x76, 0xBE, 0x70, 0x25, 0xD8, 0x42}};
    if (!memcmp(&g, &kSmbus, sizeof g)) return "\\\\.\\mxsmbus";
    return "\\\\.\\mxsram";
}
static uint64_t SetupDiOpen(uint64_t guid) {
    DevList32 d = {};
    if ((uint32_t)guid) memcpy(&d.g, (const void*)(uintptr_t)(uint32_t)guid, sizeof(GUID));
    g_devLists.push_back(d);
    const GUID& g = d.g;
    Log("32-bit SetupAPI: device class {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} -> %s", g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], DevPathFor(g));
    return 0x5E700010u + (uint32_t)(g_devLists.size() - 1) * 16u;
}
static DevList32* DevListOf(uint64_t h) {
    uint32_t i = ((uint32_t)h - 0x5E700010u) / 16u;
    return (uint32_t)h >= 0x5E700010u && i < g_devLists.size() ? &g_devLists[i] : nullptr;
}
static uint64_t B_SetupDiGetClassDevsA(uint64_t guid, uint64_t, uint64_t, uint64_t) { return SetupDiOpen(guid); }
static uint64_t B_SetupDiGetClassDevsExA(uint64_t guid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return SetupDiOpen(guid);
}
static uint64_t B_SetupDiEnumDeviceInterfaces(uint64_t h, uint64_t, uint64_t guid, uint64_t index, uint64_t data) {
    DevList32* d = DevListOf(h);
    if (!d || (uint32_t)index != 0 || !(uint32_t)data) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    uint8_t* p = (uint8_t*)(uintptr_t)(uint32_t)data;   // SP_DEVICE_INTERFACE_DATA (32-bit): cbSize, GUID, Flags, Reserved
    memcpy(p + 4, (uint32_t)guid ? (const void*)(uintptr_t)(uint32_t)guid : (const void*)&d->g, sizeof(GUID));
    *(uint32_t*)(p + 20) = 1;   // SPINT_ACTIVE
    *(uint32_t*)(p + 24) = (uint32_t)h;
    return TRUE;
}
static uint64_t B_SetupDiGetDeviceInterfaceDetailA(uint64_t h, uint64_t data, uint64_t detail, uint64_t size, uint64_t need,
                                                   uint64_t info) {
    DevList32* d = DevListOf(h);
    if (!d) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    GUID g = d->g;
    if ((uint32_t)data) memcpy(&g, (const uint8_t*)(uintptr_t)(uint32_t)data + 4, sizeof g);
    const char* path = DevPathFor(g);
    uint32_t want = 4 + (uint32_t)strlen(path) + 1;   // SP_DEVICE_INTERFACE_DETAIL_DATA_A: cbSize, DevicePath[]
    if ((uint32_t)need) *(uint32_t*)(uintptr_t)(uint32_t)need = want;
    if ((uint32_t)info) memset((void*)(uintptr_t)(uint32_t)info, 0, 28);   // SP_DEVINFO_DATA (cbSize kept by the caller)
    if (!(uint32_t)detail || (uint32_t)size < want) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    strcpy((char*)(uintptr_t)(uint32_t)detail + 4, path);
    return TRUE;
}
static uint64_t B_SetupDiEnumDeviceInfo(uint64_t h, uint64_t index, uint64_t info) {
    if (!DevListOf(h) || (uint32_t)index != 0 || !(uint32_t)info) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    uint8_t* p = (uint8_t*)(uintptr_t)(uint32_t)info;   // SP_DEVINFO_DATA: cbSize, ClassGuid, DevInst, Reserved
    memcpy(p + 4, &DevListOf(h)->g, sizeof(GUID));
    *(uint32_t*)(p + 20) = 1;
    return TRUE;
}
static uint64_t B_SetupDiDestroyDeviceInfoList(uint64_t) { return TRUE; }
static uint64_t B_SetupDiFalse7(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
}
static uint64_t B_SetupDiFalse2(uint64_t, uint64_t) {
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
}
static uint64_t B_SetupDiOpenDevRegKey(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) { return (uint64_t)(int64_t)-1; }
static uint64_t B_CM_Get_Device_ID_ExA(uint64_t, uint64_t buf, uint64_t len, uint64_t, uint64_t) {
    if ((uint32_t)buf && (uint32_t)len > 24) strcpy((char*)(uintptr_t)(uint32_t)buf, "ROOT\SEGA\0000");
    return 0;   // CR_SUCCESS
}
// Windows security for named objects (Sega amLib makes its SRAM/EEPROM/DIP switch mutexes "for everyone"): the console
// has none of it; the descriptor is ours and empty, and the object functions below leave security out (a 32-bit
// SECURITY_ATTRIBUTES is not the 64-bit layout either). Names lose "Global\" (an app has no global namespace).
static uint64_t B_AllocateAndInitializeSid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t, uint64_t out) {
    if ((uint32_t)out) *(uint32_t*)(uintptr_t)(uint32_t)out = (uint32_t)(uintptr_t)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 16);
    return TRUE;
}
static uint64_t B_FreeSid(uint64_t) { return 0; }
static uint64_t B_InitializeSecurityDescriptor(uint64_t sd, uint64_t) {
    if ((uint32_t)sd) memset((void*)(uintptr_t)(uint32_t)sd, 0, 20);
    return TRUE;
}
static uint64_t B_SetSecurityDescriptorDacl(uint64_t, uint64_t, uint64_t, uint64_t) { return TRUE; }
static std::wstring ObjName32(uint64_t p, bool wide) {
    if (!(uint32_t)p) return L"";
    std::wstring n;
    if (wide) n = (const wchar_t*)(uintptr_t)(uint32_t)p;
    else for (const char* c = (const char*)(uintptr_t)(uint32_t)p; *c; c++) n += (wchar_t)(unsigned char)*c;
    if (!_wcsnicmp(n.c_str(), L"Global\\", 7)) n = n.substr(7);
    return n;
}
static uint64_t Handle32(HANDLE h) { return (uint64_t)(uintptr_t)h; }
static uint64_t MakeMutex32(uint64_t owner, uint64_t name, bool wide) {
    std::wstring n = ObjName32(name, wide);
    return Handle32(CreateMutexExW(nullptr, n.empty() ? nullptr : n.c_str(), owner ? CREATE_MUTEX_INITIAL_OWNER : 0, MUTEX_ALL_ACCESS));
}
static uint64_t B_CreateMutexA32(uint64_t, uint64_t owner, uint64_t name) { return MakeMutex32(owner, name, false); }
static uint64_t B_CreateMutexW32(uint64_t, uint64_t owner, uint64_t name) { return MakeMutex32(owner, name, true); }
static uint64_t MakeEvent32(uint64_t manual, uint64_t initial, uint64_t name, bool wide) {
    std::wstring n = ObjName32(name, wide);
    DWORD f = ((uint32_t)manual ? CREATE_EVENT_MANUAL_RESET : 0) | ((uint32_t)initial ? CREATE_EVENT_INITIAL_SET : 0);
    return Handle32(CreateEventExW(nullptr, n.empty() ? nullptr : n.c_str(), f, EVENT_ALL_ACCESS));
}
static uint64_t B_CreateEventA32(uint64_t, uint64_t m, uint64_t i, uint64_t name) { return MakeEvent32(m, i, name, false); }
static uint64_t B_CreateEventW32(uint64_t, uint64_t m, uint64_t i, uint64_t name) { return MakeEvent32(m, i, name, true); }
static uint64_t MakeSemaphore32(uint64_t initial, uint64_t max, uint64_t name, bool wide) {
    std::wstring n = ObjName32(name, wide);
    return Handle32(CreateSemaphoreExW(nullptr, (LONG)(int32_t)initial, (LONG)(int32_t)max, n.empty() ? nullptr : n.c_str(), 0,
                                       SEMAPHORE_ALL_ACCESS));
}
static uint64_t B_CreateSemaphoreA32(uint64_t, uint64_t i, uint64_t m, uint64_t name) { return MakeSemaphore32(i, m, name, false); }
static uint64_t B_CreateSemaphoreW32(uint64_t, uint64_t i, uint64_t m, uint64_t name) { return MakeSemaphore32(i, m, name, true); }
// the one screen, to the program's 32-bit callback (Windows would call it as 64-bit code); the same monitor handle
// and size the window shims give (MonitorFromWindow, GetMonitorInfo)
uint32_t Call32(uint32_t func, const uint32_t* args, int n);
static uint64_t B_EnumDisplayMonitors32(uint64_t, uint64_t, uint64_t fn, uint64_t data) {
    static RECT* r = (RECT*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, sizeof(RECT));
    struct {
        DWORD cbSize;
        RECT rcMonitor, rcWork;
        DWORD dwFlags;
    } mi = {40};   // (MONITORINFO: not in the console's headers)
    typedef BOOL(WINAPI * gmi_t)(HMONITOR, void*);
    static auto gmi = (gmi_t)FindShim("user32.dll", "GetMonitorInfoA");
    if (gmi) gmi((HMONITOR)(uintptr_t)0x0A5C0FE0, &mi);
    if (mi.rcMonitor.right) *r = mi.rcMonitor;
    else r->left = r->top = 0, r->right = 1920, r->bottom = 1080;
    if ((uint32_t)fn) {
        uint32_t a[4] = {0x0A5C0FE0, 0, (uint32_t)(uintptr_t)r, (uint32_t)data};
        Call32((uint32_t)fn, a, 4);
    }
    return TRUE;
}
static uint64_t B_GetWindowThreadProcessId32(uint64_t, uint64_t pid) {
    if ((uint32_t)pid) *(uint32_t*)(uintptr_t)(uint32_t)pid = GetCurrentProcessId();
    return GetCurrentThreadId();
}
static uint64_t B_GetClassNameA32(uint64_t, uint64_t buf, uint64_t n) {   // (the game's own window class: one name)
    if (!(uint32_t)buf || !(int)n) return 0;
    strncpy_s((char*)(uintptr_t)(uint32_t)buf, (size_t)(int)n, "GameWindow", _TRUNCATE);
    return strlen((char*)(uintptr_t)(uint32_t)buf);
}
static uint32_t g_unhandled32 = 0;   // (kept, never given to Windows for the same reason)
static uint64_t B_SetUnhandledFilter32(uint64_t fn) {
    uint32_t was = g_unhandled32;
    g_unhandled32 = (uint32_t)fn;
    return was;
}
static uint64_t B_VirtualProtect(uint64_t at, uint64_t size, uint64_t prot, uint64_t old);
static uint64_t B_PathFileExistsW(uint64_t p);
static uint64_t B_PathCombineA(uint64_t out, uint64_t dir, uint64_t file);
static uint64_t B_PathAppendA(uint64_t path, uint64_t more);
static uint64_t B_PathRemoveFileSpecA(uint64_t path);
static uint64_t B_RaiseException(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_RtlUnwind(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_CreateThread(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint32_t Call32(uint32_t func, const uint32_t* args, int n);
static uint8_t* MakeTeb32(uint32_t stackTop, uint32_t stackBottom);
static uint64_t B_LoadLibraryA(uint64_t name);
static uint64_t B_LoadLibraryW(uint64_t name);
static uint64_t B_LoadLibraryExA(uint64_t name, uint64_t, uint64_t);
static uint64_t B_LoadLibraryExW(uint64_t name, uint64_t, uint64_t);
static uint64_t B_FreeLibrary(uint64_t);
static uint64_t B_GetModuleHandleW(uint64_t name);
static uint64_t B_GetModuleFileNameA(uint64_t, uint64_t, uint64_t);
static uint64_t B_GetModuleInformation(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_GetModuleFileNameW(uint64_t, uint64_t, uint64_t);
static uint64_t B_GetProcAddress(uint64_t mod, uint64_t name);

struct Known {
    const char* dll;
    const char* name;
    int args;
    bool cdecl_;
    void* shim;   // nullptr: the real 64-bit function of that name
};
// COM for 32-bit programs: no 32-bit COM classes exist here. CoCreateInstance answers "class not registered" (a
// game then goes without: DirectMusic, DirectShow movies, ...); the class asked for is logged
static uint64_t B_CoInitialize32(uint64_t) { return 0; }
static uint64_t B_CoInitializeEx32(uint64_t, uint64_t) { return 0; }
static uint64_t B_CoUninitialize32() { return 0; }
static uint64_t B_CoCreateInstance32(uint64_t clsid, uint64_t, uint64_t, uint64_t, uint64_t out) {
    if ((uint32_t)out) *(uint32_t*)(uintptr_t)(uint32_t)out = 0;
    const GUID* g = (const GUID*)(uintptr_t)(uint32_t)clsid;
    if (g)
        Log("32-bit program asks for COM class {%08lX-%04X-%04X-%02X%02X-...}: not there", g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1]);
    return 0x80040154;   // REGDB_E_CLASSNOTREG
}
// waits on handle ARRAYS: a 32-bit program's array holds 4-byte handles, 64-bit Windows reads 8-byte ones
static void WidenHandles(uint64_t arr, uint64_t n, HANDLE* out) {
    const int32_t* in = (const int32_t*)(uintptr_t)(uint32_t)arr;
    for (uint32_t i = 0; i < (uint32_t)n && i < MAXIMUM_WAIT_OBJECTS; i++) out[i] = (HANDLE)(intptr_t)in[i];
}
// structures with pointer-size fields: a 32-bit program's are smaller than the system's (it wrote past them)
// CreateFile: a 32-bit SECURITY_ATTRIBUTES (12 bytes) is not the system's (24): left out (handles are not inherited
// here anyway); a failed open is logged with its reason (the first ones)
static void FileFailed(const char* what, DWORD err) {
    static int told = 0;
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
        if (told++ < 20) Log("32-bit program opens %s: failed (%lu)", what, err);
    } else if (told++ < 20) {
        Log("32-bit program opens %s: not found", what);
    }
}
typedef HANDLE(WINAPI* cfa_t)(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* cfw_t)(LPCWSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
static cfa_t RealCreateFileA() {
    static cfa_t f = (cfa_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileA");
    return f;
}
static cfw_t RealCreateFileW() {
    static cfw_t f = (cfw_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW");
    return f;
}
bool RedirectCabinetPath(const char* in, std::string& out);   // arcade_io.cpp
// Konami's save folder: the game asks for "sv/..." (its AVS mount), the TeknoParrot pack has it as "save/..."
static const char* KonamiSave(const char* n, std::string& buf) {
    if (!n || !((n[0] == 's' || n[0] == 'S') && (n[1] == 'v' || n[1] == 'V') && (n[2] == '/' || n[2] == '\\'))) return n;
    std::string alt = std::string("save") + (n + 2);
    std::wstring w(alt.begin(), alt.end());
    std::wstring dir = w.substr(0, w.find_last_of(L"\\/"));
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExFromAppW(w.c_str(), GetFileExInfoStandard, &fa) &&
        !GetFileAttributesExFromAppW(dir.c_str(), GetFileExInfoStandard, &fa))
        return n;
    static int told = 0;
    if (told++ < 3) Log("save folder: %s -> %s", n, alt.c_str());
    buf = alt;
    return buf.c_str();
}
static uint64_t B_CreateFileA(uint64_t name, uint64_t acc, uint64_t share, uint64_t, uint64_t disp, uint64_t flags, uint64_t) {
    const char* n = (const char*)(uintptr_t)(uint32_t)name;
    std::string cab;
    if (RedirectCabinetPath(n, cab)) n = cab.c_str();
    std::string ks;
    n = KonamiSave(n, ks);
    if (NetFsOn() && n)
        if (void* nf = FindNetFsShim("kernel32.dll", "CreateFileA")) {   // a game played from the NAS
            HANDLE h = ((HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE))nf)(n, (DWORD)acc, (DWORD)share, nullptr,
                                                                                               (DWORD)disp, (DWORD)flags, nullptr);
            return h == INVALID_HANDLE_VALUE ? 0xFFFFFFFFull : (uint64_t)(uint32_t)(uintptr_t)h;
        }
    HANDLE h;
    // the game's I/O board (COM2, \\.\COM1 ...) is the emulated one; any other file goes on to Windows
    if (void* io = n ? FindIoShim("kernel32.dll", "CreateFileA") : nullptr)
        h = ((HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE))io)(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);
    else
        h = RealCreateFileA()(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        FileFailed(n ? n : "(null)", e);
        SetLastError(e);
        return 0xFFFFFFFFull;
    }
    return (uint64_t)(uint32_t)(uintptr_t)h;
}
static uint64_t B_CreateFileW(uint64_t name, uint64_t acc, uint64_t share, uint64_t, uint64_t disp, uint64_t flags, uint64_t) {
    const wchar_t* n = (const wchar_t*)(uintptr_t)(uint32_t)name;
    std::wstring cabW;
    if (n) {
        std::string a, cab;
        for (const wchar_t* c = n; *c; c++) a += (char)*c;
        if (RedirectCabinetPath(a.c_str(), cab)) cabW.assign(cab.begin(), cab.end()), n = cabW.c_str();
    }
    if (NetFsOn() && n)
        if (void* nf = FindNetFsShim("kernel32.dll", "CreateFileW")) {   // a game played from the NAS
            HANDLE h = ((HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE))nf)(n, (DWORD)acc, (DWORD)share, nullptr,
                                                                                                (DWORD)disp, (DWORD)flags, nullptr);
            return h == INVALID_HANDLE_VALUE ? 0xFFFFFFFFull : (uint64_t)(uint32_t)(uintptr_t)h;
        }
    HANDLE h;
    // the game's I/O board (COM2, \\.\COM1 ...) is the emulated one; any other file goes on to Windows
    if (void* io = n ? FindIoShim("kernel32.dll", "CreateFileW") : nullptr)
        h = ((HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE))io)(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);
    else
        h = RealCreateFileW()(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        char a[MAX_PATH] = "(null)";
        if (n) WideCharToMultiByte(CP_UTF8, 0, n, -1, a, MAX_PATH, nullptr, nullptr);
        FileFailed(a, e);
        SetLastError(e);
        return 0xFFFFFFFFull;
    }
    return (uint64_t)(uint32_t)(uintptr_t)h;
}
// --- the registry for 32-bit programs: an arcade game's settings (Dragon Dance: Taito's keys) kept in memory. A key
// the game made answers; one it never made is "not found" (a missing function answered 0 = success, and the game
// used an empty key). Keys are by their full name, any case; HKEYs below 4 GB.
struct RegKey {
    std::string path;   // "HKLM\Software\Taito\..." (lower case)
};
static std::map<uint32_t, RegKey> g_regOpen;
static std::map<std::string, std::map<std::string, std::pair<uint32_t, std::vector<uint8_t>>>> g_reg;   // key -> value -> (type, data)
static std::set<std::string> g_regKeys;
static uint32_t g_regNext = 0x7C000000;
static std::mutex g_regLock;
static std::string RegLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    while (!s.empty() && s.back() == '\\') s.pop_back();
    return s;
}
// Taito's settings, as TeknoParrot (OpenParrot RegHooks) answers them: free play, HD, horizontal screen, no events
static void RegSeed() {
    static bool done = false;
    if (done) return;
    done = true;
    static const struct { const char* name; uint32_t v; } kNesica[] = {
        {"coincredit", 0}, {"resolution", 1}, {"screenvertical", 0}, {"eventmodeenable", 0}, {"userselectenable", 0},
        {"gameresult", 0}, {"ioerrorcoin", 0}, {"ioerrorcredit", 0}, {"systemtype", 0}, {"conditiontime", 300},
        {"eventnexttime", 900}, {"gamekind", 1234}, {"loglevel", 0}, {"trafficcount", 2}, {"updatestep", 0},
        {"country", 1}, {"appver", 1}};
    for (const char* key : {"hklm\\software\\taito\\nesicaxlive", "hklm\\software\\taito\\typex"}) {
        std::string k = key;
        for (size_t at = k.find('\\'); ; at = k.find('\\', at + 1)) {
            g_regKeys.insert(at == std::string::npos ? k : k.substr(0, at));
            if (at == std::string::npos) break;
        }
        for (auto& v : kNesica) {
            std::vector<uint8_t> d(4);
            memcpy(d.data(), &v.v, 4);
            g_reg[k][v.name] = {REG_DWORD, d};
        }
    }
}
static std::string RegRoot(uint64_t h) {
    { std::lock_guard<std::mutex> l(g_regLock); RegSeed(); }
    switch ((uint32_t)h) {
    case 0x80000000: return "hkcr";
    case 0x80000001: return "hkcu";
    case 0x80000002: return "hklm";
    case 0x80000003: return "hku";
    }
    std::lock_guard<std::mutex> l(g_regLock);
    auto it = g_regOpen.find((uint32_t)h);
    return it == g_regOpen.end() ? "" : it->second.path;
}
static std::string RegA(uint64_t p) { return p ? std::string((const char*)(uintptr_t)(uint32_t)p) : std::string(); }
static std::string RegW(uint64_t p) {
    if (!p) return {};
    const wchar_t* w = (const wchar_t*)(uintptr_t)(uint32_t)p;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
static uint64_t RegOpen(uint64_t root, const std::string& sub, uint64_t out, bool create) {
    std::string base = RegRoot(root);
    if (base.empty()) return ERROR_INVALID_HANDLE;
    std::string path = RegLower(sub.empty() ? base : base + "\\" + sub);
    std::lock_guard<std::mutex> l(g_regLock);
    if (!create && !g_regKeys.count(path) && path != base) return ERROR_FILE_NOT_FOUND;
    if (create)
        for (size_t at = path.find('\\'); ; at = path.find('\\', at + 1)) {
            g_regKeys.insert(at == std::string::npos ? path : path.substr(0, at));
            if (at == std::string::npos) break;
        }
    g_regNext += 4;
    g_regOpen[g_regNext] = RegKey{path};
    if (out) *(uint32_t*)(uintptr_t)(uint32_t)out = g_regNext;
    static int told = 0;
    if (told++ < 20) Log("32-bit registry: %s %s", create ? "made/opened" : "opened", path.c_str());
    return ERROR_SUCCESS;
}
static uint64_t B_RegOpenKeyExA(uint64_t k, uint64_t sub, uint64_t, uint64_t, uint64_t out) { return RegOpen(k, RegA(sub), out, false); }
static uint64_t B_RegOpenKeyExW(uint64_t k, uint64_t sub, uint64_t, uint64_t, uint64_t out) { return RegOpen(k, RegW(sub), out, false); }
static uint64_t B_RegOpenKeyA(uint64_t k, uint64_t sub, uint64_t out) { return RegOpen(k, RegA(sub), out, false); }
static uint64_t B_RegCreateKeyExA(uint64_t k, uint64_t sub, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t out, uint64_t disp) {
    if (disp) *(uint32_t*)(uintptr_t)(uint32_t)disp = 1;   // REG_CREATED_NEW_KEY
    return RegOpen(k, RegA(sub), out, true);
}
static uint64_t B_RegCreateKeyExW(uint64_t k, uint64_t sub, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t out, uint64_t disp) {
    if (disp) *(uint32_t*)(uintptr_t)(uint32_t)disp = 1;
    return RegOpen(k, RegW(sub), out, true);
}
static uint64_t B_RegCreateKeyA(uint64_t k, uint64_t sub, uint64_t out) { return RegOpen(k, RegA(sub), out, true); }
static uint64_t B_RegCloseKey(uint64_t k) {
    std::lock_guard<std::mutex> l(g_regLock);
    g_regOpen.erase((uint32_t)k);
    return ERROR_SUCCESS;
}
static uint64_t RegQuery(uint64_t k, const std::string& name, uint64_t type, uint64_t data, uint64_t size) {
    std::string key = RegRoot(k);
    std::lock_guard<std::mutex> l(g_regLock);
    auto kit = g_reg.find(key);
    if (kit == g_reg.end()) return ERROR_FILE_NOT_FOUND;
    auto vit = kit->second.find(RegLower(name));
    if (vit == kit->second.end()) return ERROR_FILE_NOT_FOUND;
    const auto& v = vit->second;
    if (type) *(uint32_t*)(uintptr_t)(uint32_t)type = v.first;
    uint32_t* sz = size ? (uint32_t*)(uintptr_t)(uint32_t)size : nullptr;
    if (data) {
        if (!sz || *sz < v.second.size()) {
            if (sz) *sz = (uint32_t)v.second.size();
            return ERROR_MORE_DATA;
        }
        memcpy((void*)(uintptr_t)(uint32_t)data, v.second.data(), v.second.size());
    }
    if (sz) *sz = (uint32_t)v.second.size();
    return ERROR_SUCCESS;
}
static uint64_t B_RegQueryValueExA(uint64_t k, uint64_t name, uint64_t, uint64_t type, uint64_t data, uint64_t size) {
    return RegQuery(k, RegA(name), type, data, size);
}
static uint64_t B_RegQueryValueExW(uint64_t k, uint64_t name, uint64_t, uint64_t type, uint64_t data, uint64_t size) {
    return RegQuery(k, RegW(name), type, data, size);
}
static uint64_t RegSet(uint64_t k, const std::string& name, uint64_t type, uint64_t data, uint64_t size) {
    std::string key = RegRoot(k);
    if (key.empty()) return ERROR_INVALID_HANDLE;
    std::lock_guard<std::mutex> l(g_regLock);
    const uint8_t* d = (const uint8_t*)(uintptr_t)(uint32_t)data;
    g_reg[key][RegLower(name)] = {(uint32_t)type, std::vector<uint8_t>(d, d + (d ? (uint32_t)size : 0))};
    return ERROR_SUCCESS;
}
static uint64_t B_RegSetValueExA(uint64_t k, uint64_t name, uint64_t, uint64_t type, uint64_t data, uint64_t size) {
    return RegSet(k, RegA(name), type, data, size);
}
static uint64_t B_RegSetValueExW(uint64_t k, uint64_t name, uint64_t, uint64_t type, uint64_t data, uint64_t size) {
    return RegSet(k, RegW(name), type, data, size);
}
static uint64_t B_RegDeleteValueA(uint64_t k, uint64_t name) {
    std::string key = RegRoot(k);
    std::lock_guard<std::mutex> l(g_regLock);
    auto it = g_reg.find(key);
    return it != g_reg.end() && it->second.erase(RegLower(RegA(name))) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}
static uint64_t B_RegFlushKey(uint64_t) { return ERROR_SUCCESS; }
// --- file mappings for 32-bit programs: views must be below 2 GB. Memory-only ones (no file: shared memory between
// the game's parts, NESiCA's "TaitoTypeX" block) are blocks of the low heap, one per name; a file's view is a low copy.
struct Map32 { uint8_t* mem = nullptr; uint64_t size = 0; HANDLE file = nullptr; std::string name; };
static std::map<uint32_t, Map32> g_maps32;
static std::map<std::string, uint32_t> g_mapNames32;
static uint32_t g_mapNext = 0x7D000000;
static std::mutex g_mapLock;
void* Alloc32(size_t size);
static uint64_t MapMake(uint64_t file, uint64_t hi, uint64_t lo, const std::string& nameIn) {
    std::string name = RegLower(nameIn);
    std::lock_guard<std::mutex> l(g_mapLock);
    if (!name.empty()) {
        auto it = g_mapNames32.find(name);
        if (it != g_mapNames32.end()) { SetLastError(ERROR_ALREADY_EXISTS); return it->second; }
    }
    Map32 m;
    m.name = name;
    m.size = (hi << 32) | (uint32_t)lo;
    HANDLE f = (HANDLE)(intptr_t)(int64_t)file;
    if (f != INVALID_HANDLE_VALUE && f) {
        m.file = f;
        if (!m.size && NetFsHandle(f)) m.size = NetFsSize(f);
        if (!m.size) {
            LARGE_INTEGER sz = {};
            GetFileSizeEx(f, &sz);
            m.size = (uint64_t)sz.QuadPart;
        }
    }
    if (!m.size || m.size > 0x40000000) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    g_mapNext += 4;
    g_maps32[g_mapNext] = m;
    if (!name.empty()) g_mapNames32[name] = g_mapNext;
    Log("32-bit file mapping %s%s, %llu KB", name.empty() ? "(no name)" : name.c_str(), m.file ? " of a file" : "",
        (unsigned long long)(m.size >> 10));
    SetLastError(0);
    return g_mapNext;
}
static uint64_t B_CreateFileMappingA(uint64_t f, uint64_t, uint64_t, uint64_t hi, uint64_t lo, uint64_t name) { return MapMake(f, hi, lo, RegA(name)); }
static uint64_t B_CreateFileMappingW(uint64_t f, uint64_t, uint64_t, uint64_t hi, uint64_t lo, uint64_t name) { return MapMake(f, hi, lo, RegW(name)); }
static uint64_t MapOpen(const std::string& name) {
    std::lock_guard<std::mutex> l(g_mapLock);
    auto it = g_mapNames32.find(RegLower(name));
    if (it == g_mapNames32.end()) { SetLastError(ERROR_FILE_NOT_FOUND); return 0; }
    return it->second;
}
static uint64_t B_OpenFileMappingA(uint64_t, uint64_t, uint64_t name) { return MapOpen(RegA(name)); }
static uint64_t B_OpenFileMappingW(uint64_t, uint64_t, uint64_t name) { return MapOpen(RegW(name)); }
static uint64_t B_MapViewOfFile(uint64_t h, uint64_t, uint64_t hi, uint64_t lo, uint64_t n) {
    std::lock_guard<std::mutex> l(g_mapLock);
    auto it = g_maps32.find((uint32_t)h);
    if (it == g_maps32.end()) {   // not ours: the real one, if its view lands low enough
        void* v = MapViewOfFile((HANDLE)(intptr_t)(int64_t)h, FILE_MAP_ALL_ACCESS, (DWORD)hi, (DWORD)lo, (SIZE_T)n);
        return (uintptr_t)v < 0x80000000ull ? (uintptr_t)v : 0;
    }
    Map32& m = it->second;
    if (!m.mem) {
        m.mem = (uint8_t*)Alloc32((size_t)m.size);
        if (m.mem && m.file && NetFsHandle(m.file)) {
            NetFsPread(m.file, 0, m.mem, (size_t)m.size);
        } else if (m.mem && m.file) {   // a file's contents, read once (what the game writes there stays in memory)
            LARGE_INTEGER z = {};
            SetFilePointerEx(m.file, z, nullptr, FILE_BEGIN);
            DWORD got = 0;
            for (uint64_t at = 0; at < m.size;) {
                DWORD k = (DWORD)std::min<uint64_t>(m.size - at, 1u << 24);
                if (!ReadFile(m.file, m.mem + at, k, &got, nullptr) || !got) break;
                at += got;
            }
        }
    }
    uint64_t at = (hi << 32) | (uint32_t)lo;
    return m.mem && at < m.size ? (uintptr_t)(m.mem + at) : 0;
}
static uint64_t B_MapViewOfFileEx(uint64_t h, uint64_t a, uint64_t hi, uint64_t lo, uint64_t n, uint64_t) { return B_MapViewOfFile(h, a, hi, lo, n); }
static uint64_t B_UnmapViewOfFile(uint64_t) { return 1; }   // (the block stays: another part may still use it)
static uint64_t B_FlushViewOfFile(uint64_t, uint64_t) { return 1; }
// --- CryptoAPI for 32-bit programs (the Xbox has none): MD5 and SHA-1 hashes, RC4 keys derived from a hash, as the
// Microsoft providers make them (Enhanced / Strong: 128-bit RC4 by default; Base: 40-bit, salted with 11 zero bytes
// unless CRYPT_NO_SALT). Handles are small numbers below 2 GB.
struct Md5 {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t len = 0;
    uint8_t buf[64];
    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8,
            0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
            0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87,
            0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
            0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
            0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
            0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
            0xeb86d391};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9,
                                  14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | uint32_t(p[i * 4 + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            uint32_t t = d;
            d = c, c = b;
            b = b + rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    void add(const uint8_t* p, size_t n) {
        while (n) {
            size_t at = len % 64, k = std::min<size_t>(64 - at, n);
            memcpy(buf + at, p, k);
            len += k, p += k, n -= k;
            if ((len % 64) == 0) block(buf);
        }
    }
    void done(uint8_t out[16]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80, z = 0;
        add(&pad, 1);
        while (len % 64 != 56) add(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (8 * i));
        add(l, 8);
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h[i] >> (8 * k));
    }
};
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t len = 0;
    uint8_t buf[64];
    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void add(const uint8_t* p, size_t n) {
        while (n) {
            size_t at = len % 64, k = std::min<size_t>(64 - at, n);
            memcpy(buf + at, p, k);
            len += k, p += k, n -= k;
            if ((len % 64) == 0) block(buf);
        }
    }
    void done(uint8_t out[20]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80, z = 0;
        add(&pad, 1);
        while (len % 64 != 56) add(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        add(l, 8);
        for (int i = 0; i < 5; i++)
            for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h[i] >> (24 - 8 * k));
    }
};
struct CryptObj {
    int kind = 0;   // 1 provider, 2 hash, 3 key
    bool enhanced = true;
    uint32_t alg = 0, prov = 0;
    Md5 md5;
    Sha1 sha1;
    bool finished = false;
    uint8_t value[20] = {};
    std::vector<uint8_t> key;   // RC4 key
    uint8_t S[256];
    int i = 0, j = 0;
    void rc4Init() {
        for (int k = 0; k < 256; k++) S[k] = uint8_t(k);
        for (int k = 0, m = 0; k < 256; k++) {
            m = (m + S[k] + key[k % key.size()]) & 255;
            std::swap(S[k], S[m]);
        }
        i = j = 0;
    }
    void rc4(uint8_t* p, size_t n) {
        for (size_t k = 0; k < n; k++) {
            i = (i + 1) & 255;
            j = (j + S[i]) & 255;
            std::swap(S[i], S[j]);
            p[k] ^= S[(S[i] + S[j]) & 255];
        }
    }
    size_t hashSize() const { return alg == 0x8004 ? 20 : 16; }
    void finish() {
        if (finished) return;
        finished = true;
        if (alg == 0x8004) sha1.done(value);
        else md5.done(value);
    }
};
static std::map<uint32_t, CryptObj> g_crypt;
static uint32_t g_cryptNext = 0x7E000000;
static std::mutex g_cryptLock;
static CryptObj* CryptGet(uint64_t h, int kind) {
    auto it = g_crypt.find((uint32_t)h);
    return it != g_crypt.end() && it->second.kind == kind ? &it->second : nullptr;
}
static uint32_t CryptNew(CryptObj o) {
    g_cryptNext += 4;
    g_crypt[g_cryptNext] = std::move(o);
    return g_cryptNext;
}
#define C32(p) ((uint32_t*)(uintptr_t)(uint32_t)(p))
static uint64_t CryptAcquire(uint64_t out, const std::string& provider) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj o;
    o.kind = 1;
    o.enhanced = provider.empty() || provider.find("Base") == std::string::npos;
    if (out) *C32(out) = CryptNew(o);
    static int told = 0;
    if (!told++) Log("32-bit crypto: provider \"%s\" (MD5, SHA-1, RC4 here)", provider.c_str());
    return 1;
}
static uint64_t B_CryptAcquireContextA(uint64_t out, uint64_t, uint64_t prov, uint64_t, uint64_t) { return CryptAcquire(out, RegA(prov)); }
static uint64_t B_CryptAcquireContextW(uint64_t out, uint64_t, uint64_t prov, uint64_t, uint64_t) { return CryptAcquire(out, RegW(prov)); }
static uint64_t B_CryptReleaseContext(uint64_t h, uint64_t) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    return g_crypt.erase((uint32_t)h) ? 1 : 1;
}
static uint64_t B_CryptCreateHash(uint64_t prov, uint64_t alg, uint64_t, uint64_t, uint64_t out) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* p = CryptGet(prov, 1);
    if (!p || (alg != 0x8003 && alg != 0x8004)) {
        Log("32-bit crypto: hash 0x%x not made here", (unsigned)alg);
        SetLastError(NTE_BAD_ALGID);
        return 0;
    }
    CryptObj o;
    o.kind = 2, o.alg = (uint32_t)alg, o.prov = (uint32_t)prov, o.enhanced = p->enhanced;
    *C32(out) = CryptNew(o);
    return 1;
}
static uint64_t B_CryptHashData(uint64_t h, uint64_t data, uint64_t n, uint64_t) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* o = CryptGet(h, 2);
    if (!o || o->finished) return 0;
    if (o->alg == 0x8004) o->sha1.add((const uint8_t*)(uintptr_t)(uint32_t)data, (uint32_t)n);
    else o->md5.add((const uint8_t*)(uintptr_t)(uint32_t)data, (uint32_t)n);
    return 1;
}
static uint64_t B_CryptGetHashParam(uint64_t h, uint64_t what, uint64_t data, uint64_t size, uint64_t) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* o = CryptGet(h, 2);
    if (!o || !size) return 0;
    uint32_t have = *C32(size);
    if (what == 4) {   // HP_HASHSIZE
        *C32(size) = 4;
        if (data && have >= 4) *C32(data) = (uint32_t)o->hashSize();
        return 1;
    }
    if (what == 1) {   // HP_ALGID
        *C32(size) = 4;
        if (data && have >= 4) *C32(data) = o->alg;
        return 1;
    }
    if (what != 2) return 0;   // HP_HASHVAL
    o->finish();
    *C32(size) = (uint32_t)o->hashSize();
    if (!data) return 1;
    if (have < o->hashSize()) { SetLastError(ERROR_MORE_DATA); return 0; }
    memcpy((void*)(uintptr_t)(uint32_t)data, o->value, o->hashSize());
    return 1;
}
static uint64_t B_CryptDeriveKey(uint64_t prov, uint64_t alg, uint64_t hash, uint64_t flags, uint64_t out) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* hp = CryptGet(hash, 2);
    if (!hp || alg != 0x6801) {   // CALG_RC4 only
        Log("32-bit crypto: key 0x%x not made here", (unsigned)alg);
        SetLastError(NTE_BAD_ALGID);
        return 0;
    }
    hp->finish();
    uint32_t bits = uint32_t(flags) >> 16;
    if (!bits) bits = hp->enhanced ? 128 : 40;
    size_t n = std::min<size_t>(bits / 8, hp->hashSize());
    CryptObj o;
    o.kind = 3, o.alg = 0x6801;
    o.key.assign(hp->value, hp->value + n);
    if (!(flags & 0x10) && n < 16) o.key.resize(16, 0);   // the old providers' salt: zero bytes up to 128 bits
    o.rc4Init();
    *C32(out) = CryptNew(o);
    return 1;
}
static uint64_t CryptRun(uint64_t k, uint64_t final_, uint64_t data, uint64_t len) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* o = CryptGet(k, 3);
    if (!o) return 0;
    if (data && len) o->rc4((uint8_t*)(uintptr_t)(uint32_t)data, *C32(len));
    if (final_) o->rc4Init();   // a final block: the key starts over for the next message
    return 1;
}
static uint64_t B_CryptEncrypt(uint64_t k, uint64_t, uint64_t fin, uint64_t, uint64_t data, uint64_t len, uint64_t) {
    return CryptRun(k, fin, data, len);
}
static uint64_t B_CryptDecrypt(uint64_t k, uint64_t, uint64_t fin, uint64_t, uint64_t data, uint64_t len) {
    return CryptRun(k, fin, data, len);
}
static uint64_t B_CryptDestroyHash(uint64_t h) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    g_crypt.erase((uint32_t)h);
    return 1;
}
static uint64_t B_CryptDestroyKey(uint64_t h) { return B_CryptDestroyHash(h); }
// OpenProcess: arcade games check that their launcher (the parent process) still runs, and quit when it cannot be
// opened (Dragon Dance). In the console's sandbox no other process opens: the game gets its own process instead,
// which is "still running" (GetExitCodeProcess -> STILL_ACTIVE)
static uint64_t B_OpenProcess(uint64_t access, uint64_t inherit, uint64_t pid) {
    HANDLE h = OpenProcess((DWORD)access, (BOOL)inherit, (DWORD)pid);
    if (h) return (uint64_t)(uint32_t)(uintptr_t)h;
    static int told = 0;
    if (told++ < 3) Log("32-bit OpenProcess(%u) not allowed here: the game gets its own process", (unsigned)pid);
    return 0xFFFFFFFFull;   // (sign-extended to the current process pseudo handle when passed back)
}
// iphlpapi GetNetworkParams for 32-bit programs: FIXED_INFO as 32-bit Windows lays it out (584 bytes): host name,
// no domain, no DNS server. Battle Gear 4 read garbage from an unfilled buffer (missing function: 0) and its
// buffer check stopped it.
static uint64_t B_GetNetworkParams(uint64_t out, uint64_t sizeP) {
    uint32_t* size = sizeP ? (uint32_t*)(uintptr_t)(uint32_t)sizeP : nullptr;
    const uint32_t need = 584;
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!out || *size < need) { *size = need; return ERROR_BUFFER_OVERFLOW; }
    uint8_t* f = (uint8_t*)(uintptr_t)(uint32_t)out;
    memset(f, 0, need);
    strcpy((char*)f, "ARCADE");                       // HostName[132]
    *(uint32_t*)(f + 264) = (uint32_t)(uintptr_t)(f + 268);   // CurrentDnsServer -> DnsServerList
    strcpy((char*)f + 272, "0.0.0.0");                // DnsServerList.IpAddress
    strcpy((char*)f + 288, "0.0.0.0");                // DnsServerList.IpMask
    *(uint32_t*)(f + 308) = 1;                        // NodeType: broadcast
    return ERROR_SUCCESS;
}
static uint64_t B_timeSetEvent(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_timeKillEvent(uint64_t);
static uint64_t B_mmioOpenA(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioClose(uint64_t, uint64_t);
static uint64_t B_mmioRead(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioWrite(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioSeek(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioDescend(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioAscend(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioGetInfo(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioSetInfo(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioAdvance(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioCreateChunk(uint64_t, uint64_t, uint64_t);
static uint64_t B_joyGetNumDevs() { return 0; }
// iphlpapi for 32-bit programs: one network adapter (index 1, 127.0.0.1 style), as 32-bit Windows lays them out
static uint64_t B_GetInterfaceInfo(uint64_t out, uint64_t sizeP) {   // IP_INTERFACE_INFO: 4 + 260 bytes
    uint32_t* size = sizeP ? (uint32_t*)(uintptr_t)(uint32_t)sizeP : nullptr;
    const uint32_t need = 4 + 4 + 256;
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!out || *size < need) { *size = need; return ERROR_INSUFFICIENT_BUFFER; }
    uint8_t* f = (uint8_t*)(uintptr_t)(uint32_t)out;
    memset(f, 0, need);
    *(int32_t*)f = 1;                 // NumAdapters
    *(uint32_t*)(f + 4) = 1;          // Adapter[0].Index
    const wchar_t* name = L"\\DEVICE\\TCPIP_{00000000-0000-0000-0000-000000000001}";
    memcpy(f + 8, name, (wcslen(name) + 1) * 2);
    return ERROR_SUCCESS;
}
static uint64_t B_GetAdaptersInfo(uint64_t out, uint64_t sizeP) {   // IP_ADAPTER_INFO, 648 bytes in 32-bit
    uint32_t* size = sizeP ? (uint32_t*)(uintptr_t)(uint32_t)sizeP : nullptr;
    const uint32_t need = 648;
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!out || *size < need) { *size = need; return ERROR_BUFFER_OVERFLOW; }
    uint8_t* f = (uint8_t*)(uintptr_t)(uint32_t)out;
    memset(f, 0, need);
    *(uint32_t*)(f + 4) = 1;                                                   // ComboIndex
    strcpy((char*)f + 8, "{00000000-0000-0000-0000-000000000001}");           // AdapterName[260]
    strcpy((char*)f + 268, "Game Vault network");                             // Description[132]
    *(uint32_t*)(f + 400) = 6;                                                 // AddressLength
    static const uint8_t mac[6] = {0x00, 0x15, 0x5D, 0x47, 0x56, 0x01};
    memcpy(f + 404, mac, 6);                                                   // Address[8]
    *(uint32_t*)(f + 412) = 1;                                                 // Index
    *(uint32_t*)(f + 416) = 6;                                                 // Type: Ethernet
    strcpy((char*)f + 432, "192.168.0.2");                                     // IpAddressList.IpAddress
    strcpy((char*)f + 448, "255.255.255.0");                                   // IpAddressList.IpMask
    strcpy((char*)f + 472, "192.168.0.1");                                     // GatewayList.IpAddress
    strcpy((char*)f + 488, "255.255.255.255");
    return ERROR_SUCCESS;
}
// small user32 helpers for 32-bit programs (rectangles, upper case; menus and accelerators: none on a console)
static uint64_t B_SetRect(uint64_t r, uint64_t l, uint64_t t, uint64_t rt, uint64_t b) {
    if (!r) return 0;
    int32_t* p = (int32_t*)(uintptr_t)(uint32_t)r;
    p[0] = (int32_t)l, p[1] = (int32_t)t, p[2] = (int32_t)rt, p[3] = (int32_t)b;
    return 1;
}
static uint64_t B_SetRectEmpty(uint64_t r) { return B_SetRect(r, 0, 0, 0, 0); }
static uint64_t B_CharUpperA(uint64_t s) {
    if ((uint32_t)s < 0x10000) return (uint64_t)(uint32_t)toupper((int)(uint32_t)s);   // one character
    for (char* c = (char*)(uintptr_t)(uint32_t)s; *c; c++) *c = (char)toupper((unsigned char)*c);
    return s;
}
static uint64_t B_CharLowerA(uint64_t s) {
    if ((uint32_t)s < 0x10000) return (uint64_t)(uint32_t)tolower((int)(uint32_t)s);
    for (char* c = (char*)(uintptr_t)(uint32_t)s; *c; c++) *c = (char)tolower((unsigned char)*c);
    return s;
}
static uint64_t B_Null2(uint64_t, uint64_t) { return 0; }
// SuspendThread / ResumeThread for 32-bit programs: pausing a thread that is inside the loader (holding a lock of
// the bridge, the log, the graphics) stops every other thread for good (Battle Gear 4 suspended one right after its
// I/O board check). A pause is counted, not done; a resume undoes counted pauses first, and only then (a thread
// made with CREATE_SUSPENDED) really resumes.
static std::mutex g_suspendLock;
static std::map<uint64_t, uint32_t> g_suspended;
static uint64_t B_SuspendThread(uint64_t h) {
    HANDLE th = (HANDLE)(intptr_t)(int32_t)(uint32_t)h;
    // a thread pausing itself (until another wakes it: Battle Gear 4's I/O thread) holds no lock here: for real
    if ((uint32_t)h == 0xFFFFFFFEu || GetThreadId(th) == GetCurrentThreadId()) {
        static int toldSelf = 0;
        if (toldSelf++ < 3) Log("32-bit SuspendThread: the thread pauses itself");
        return SuspendThread(GetCurrentThread());
    }
    std::lock_guard<std::mutex> l(g_suspendLock);
    static int told = 0;
    if (told++ < 3) Log("32-bit SuspendThread: another thread, counted only (a real pause can freeze the loader)");
    return g_suspended[(uint64_t)(uint32_t)h]++;
}
static uint64_t B_ResumeThread(uint64_t h) {
    {
        std::lock_guard<std::mutex> l(g_suspendLock);
        auto it = g_suspended.find((uint64_t)(uint32_t)h);
        if (it != g_suspended.end() && it->second) return it->second--;
    }
    return (uint64_t)ResumeThread((HANDLE)(uintptr_t)(uint32_t)h);
}
// the cabinet drive (D:\ = the game's folder, profiles that say so) for file checks; misses logged (what a game waits for)
static uint64_t B_GetFileAttributesA(uint64_t name) {
    const char* n = (const char*)(uintptr_t)(uint32_t)name;
    std::string cab;
    if (RedirectCabinetPath(n, cab)) n = cab.c_str();
    std::string ks;
    n = KonamiSave(n, ks);
    DWORD a = INVALID_FILE_ATTRIBUTES;
    if (void* f = FindShim("kernel32.dll", "GetFileAttributesA"))   // (NAS games)
        a = ((DWORD(WINAPI*)(LPCSTR))f)(n);
    else {
        WIN32_FILE_ATTRIBUTE_DATA d;
        std::wstring w;
        for (const char* c = n; c && *c; c++) w += (wchar_t)(unsigned char)*c;
        if (GetFileAttributesExFromAppW(w.c_str(), GetFileExInfoStandard, &d)) a = d.dwFileAttributes;
    }
    if (a == INVALID_FILE_ATTRIBUTES) {
        static int told = 0;
        if (told++ < 15) Log("32-bit program looks for %s: not there", n ? n : "(null)");
        SetLastError(ERROR_FILE_NOT_FOUND);
    }
    return a;
}
static bool CabinetRoot(const char* n) {   // "D:\" or "D:" while the profile has the cabinet drive
    std::string cab;
    return n && RedirectCabinetPath((std::string(n).size() == 2 ? std::string(n) + "\\" : std::string(n)).c_str(), cab) &&
           strlen(n) <= 3;
}
static uint64_t B_GetDriveTypeA(uint64_t root) {
    const char* n = (const char*)(uintptr_t)(uint32_t)root;
    if (CabinetRoot(n)) return DRIVE_FIXED;
    return GetDriveTypeA(n);
}
static uint64_t B_GetDiskFreeSpaceA(uint64_t root, uint64_t spc, uint64_t bps, uint64_t freeC, uint64_t totalC) {
    const char* n = (const char*)(uintptr_t)(uint32_t)root;
    // (any drive: 4 KB clusters, 8 GB free of 16 GB; a cabinet's drive is small)
    if (spc) *(uint32_t*)(uintptr_t)(uint32_t)spc = 8;
    if (bps) *(uint32_t*)(uintptr_t)(uint32_t)bps = 512;
    if (freeC) *(uint32_t*)(uintptr_t)(uint32_t)freeC = 2u * 1024 * 1024;
    if (totalC) *(uint32_t*)(uintptr_t)(uint32_t)totalC = 4u * 1024 * 1024;
    (void)n;
    return 1;
}
// DuplicateHandle for 32-bit programs: the new handle is written as 4 bytes (Windows' 64-bit one writes 8 and wiped
// the next field: Konami's libavs lost the thread ID it had just stored beside it)
static uint64_t B_DuplicateHandle(uint64_t sp, uint64_t sh, uint64_t tp, uint64_t outP, uint64_t access, uint64_t inherit, uint64_t opts) {
    HANDLE h = nullptr;
    BOOL ok = DuplicateHandle((HANDLE)(intptr_t)(int32_t)(uint32_t)sp, (HANDLE)(intptr_t)(int32_t)(uint32_t)sh,
                              (HANDLE)(intptr_t)(int32_t)(uint32_t)tp, outP ? &h : nullptr, (DWORD)access, (BOOL)inherit, (DWORD)opts);
    if (ok && outP) *(uint32_t*)(uintptr_t)(uint32_t)outP = (uint32_t)(uintptr_t)h;
    return ok;
}
// --- resources of 32-bit images: the loader mapped them itself, so Windows does not know them. A resource is found in
// the image's resource tree (type, then name, then the first language); the handle is its data entry's address.
uint32_t MainImageBase32();
static const uint8_t* ResImage(uint64_t mod) {
    uint32_t base = (uint32_t)mod ? (uint32_t)mod : MainImageBase32();
    const uint8_t* b = (const uint8_t*)(uintptr_t)base;
    if (!base || b[0] != 'M' || b[1] != 'Z') return nullptr;
    return b;
}
static const IMAGE_RESOURCE_DIRECTORY* ResRoot(const uint8_t* b, uint32_t& size) {
    auto nt = (const IMAGE_NT_HEADERS32*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    size = d.Size;
    return d.VirtualAddress ? (const IMAGE_RESOURCE_DIRECTORY*)(b + d.VirtualAddress) : nullptr;
}
// one level: the entry named by an ID (key < 0x10000) or by a name (upper case compare); nullptr = the first entry
static const IMAGE_RESOURCE_DIRECTORY_ENTRY* ResFind(const uint8_t* root, const IMAGE_RESOURCE_DIRECTORY* dir, const std::wstring* name,
                                                     uint32_t id, bool any) {
    auto e = (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(dir + 1);
    int n = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    for (int i = 0; i < n; i++, e++) {
        if (any) return e;
        if (name && e->NameIsString) {
            auto s = (const IMAGE_RESOURCE_DIR_STRING_U*)(root + e->NameOffset);
            std::wstring en(s->NameString, s->Length);
            if (!_wcsicmp(en.c_str(), name->c_str())) return e;
        } else if (!name && !e->NameIsString && e->Id == id) {
            return e;
        }
    }
    return nullptr;
}
static bool ResKey(uint64_t v, bool wide, std::wstring& name, uint32_t& id) {   // "#123" and small numbers are IDs
    uint32_t p = (uint32_t)v;
    if (p < 0x10000) { id = p; return false; }
    name.clear();
    if (wide) for (const wchar_t* c = (const wchar_t*)(uintptr_t)p; *c; c++) name += *c;
    else for (const char* c = (const char*)(uintptr_t)p; *c; c++) name += (wchar_t)(unsigned char)*c;
    if (!name.empty() && name[0] == L'#') { id = (uint32_t)_wtoi(name.c_str() + 1); return false; }
    return true;
}
static uint64_t FindRes32(uint64_t mod, uint64_t nameP, uint64_t typeP, bool wide, uint32_t lang) {
    const uint8_t* b = ResImage(mod);
    uint32_t size = 0;
    auto root = b ? ResRoot(b, size) : nullptr;
    if (!root) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return 0; }
    const uint8_t* r = (const uint8_t*)root;
    std::wstring tn, nn;
    uint32_t tid = 0, nid = 0;
    bool tByName = ResKey(typeP, wide, tn, tid), nByName = ResKey(nameP, wide, nn, nid);
    auto t = ResFind(r, root, tByName ? &tn : nullptr, tid, false);
    if (!t || !t->DataIsDirectory) { SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND); return 0; }
    auto nd = (const IMAGE_RESOURCE_DIRECTORY*)(r + t->OffsetToDirectory);
    auto ne = ResFind(r, nd, nByName ? &nn : nullptr, nid, false);
    if (!ne || !ne->DataIsDirectory) { SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND); return 0; }
    auto ld = (const IMAGE_RESOURCE_DIRECTORY*)(r + ne->OffsetToDirectory);
    auto le = lang ? ResFind(r, ld, nullptr, lang, false) : nullptr;
    if (!le) le = ResFind(r, ld, nullptr, 0, true);
    if (!le || le->DataIsDirectory) { SetLastError(ERROR_RESOURCE_LANG_NOT_FOUND); return 0; }
    return (uint32_t)(uintptr_t)(r + le->OffsetToData);   // the IMAGE_RESOURCE_DATA_ENTRY
}
static uint64_t B_FindResourceA(uint64_t m, uint64_t n, uint64_t t) { return FindRes32(m, n, t, false, 0); }
static uint64_t B_FindResourceW(uint64_t m, uint64_t n, uint64_t t) { return FindRes32(m, n, t, true, 0); }
static uint64_t B_FindResourceExA(uint64_t m, uint64_t t, uint64_t n, uint64_t l) { return FindRes32(m, n, t, false, (uint32_t)l & 0xFFFF); }
static uint64_t B_FindResourceExW(uint64_t m, uint64_t t, uint64_t n, uint64_t l) { return FindRes32(m, n, t, true, (uint32_t)l & 0xFFFF); }
static uint64_t B_LoadResource(uint64_t m, uint64_t h) {
    const uint8_t* b = ResImage(m);
    if (!b || !(uint32_t)h) return 0;
    auto e = (const IMAGE_RESOURCE_DATA_ENTRY*)(uintptr_t)(uint32_t)h;
    return (uint32_t)(uintptr_t)(b + e->OffsetToData);
}
static uint64_t B_LockResource(uint64_t h) { return (uint32_t)h; }
static uint64_t B_SizeofResource(uint64_t, uint64_t h) {
    return (uint32_t)h ? ((const IMAGE_RESOURCE_DATA_ENTRY*)(uintptr_t)(uint32_t)h)->Size : 0;
}
static uint64_t B_FreeResource(uint64_t) { return 0; }
// device arrival notices (a controller plugged in): a handle that never sends anything (Contra quits without one)
static uint64_t B_RegisterDeviceNotification(uint64_t, uint64_t, uint64_t) { return 0x7E7E0001; }
static uint64_t B_UnregisterDeviceNotification(uint64_t) { return 1; }
// timeGetSystemTime: MMTIME { wType = TIME_MS, ms } (Contra's clock: a missing one froze its fade-in on black)
static uint64_t B_timeGetSystemTime(uint64_t mmt, uint64_t size) {
    if (!mmt || (uint32_t)size < 8) return 97;   // MMSYSERR_INVALPARAM
    uint32_t* t = (uint32_t*)(uintptr_t)(uint32_t)mmt;
    t[0] = 1;   // TIME_MS
    t[1] = (uint32_t)GetTickCount64();
    return 0;
}
// GetFullPathName for 32-bit programs: the "file part" pointer is 4 bytes in the program (Windows'
// 64-bit one writes 8: D3DCompiler_42 broke on the slot after it). NAS games go through the NAS layer first.
static uint64_t B_GetFullPathNameA(uint64_t name, uint64_t size, uint64_t buf, uint64_t part) {
    char* fp = nullptr;
    DWORD n;
    if (void* f = NetFsOn() ? FindShim("kernel32.dll", "GetFullPathNameA") : nullptr)
        n = ((DWORD(WINAPI*)(LPCSTR, DWORD, LPSTR, LPSTR*))f)((LPCSTR)(uintptr_t)(uint32_t)name, (DWORD)size, (LPSTR)(uintptr_t)(uint32_t)buf, &fp);
    else
        n = GetFullPathNameA((LPCSTR)(uintptr_t)(uint32_t)name, (DWORD)size, (LPSTR)(uintptr_t)(uint32_t)buf, &fp);
    if ((uint32_t)part) *(uint32_t*)(uintptr_t)(uint32_t)part = (uint32_t)(uintptr_t)fp;
    return n;
}
static uint64_t B_GetFullPathNameW(uint64_t name, uint64_t size, uint64_t buf, uint64_t part) {
    wchar_t* fp = nullptr;
    DWORD n;
    if (void* f = NetFsOn() ? FindShim("kernel32.dll", "GetFullPathNameW") : nullptr)
        n = ((DWORD(WINAPI*)(LPCWSTR, DWORD, LPWSTR, LPWSTR*))f)((LPCWSTR)(uintptr_t)(uint32_t)name, (DWORD)size, (LPWSTR)(uintptr_t)(uint32_t)buf, &fp);
    else
        n = GetFullPathNameW((LPCWSTR)(uintptr_t)(uint32_t)name, (DWORD)size, (LPWSTR)(uintptr_t)(uint32_t)buf, &fp);
    if ((uint32_t)part) *(uint32_t*)(uintptr_t)(uint32_t)part = (uint32_t)(uintptr_t)fp;
    return n;
}
static uint64_t B_GlobalMemoryStatus(uint64_t out) {   // MEMORYSTATUS, 32-bit: 8 DWORDs, sizes capped as on 32-bit Windows
    uint32_t* m = (uint32_t*)(uintptr_t)(uint32_t)out;
    MEMORYSTATUSEX x = {sizeof x};
    GlobalMemoryStatusEx(&x);
    auto cap = [](DWORDLONG v) { return (uint32_t)(v > 0x7FFFFFFFull ? 0x7FFFFFFFu : v); };
    m[0] = 32;
    m[1] = x.dwMemoryLoad;
    m[2] = cap(x.ullTotalPhys);
    m[3] = cap(x.ullAvailPhys);
    m[4] = cap(x.ullTotalPageFile);
    m[5] = cap(x.ullAvailPageFile);
    m[6] = 0x7FFE0000;   // a 32-bit program's 2 GB
    m[7] = 0x60000000;
    return 0;
}
static uint64_t B_GetSystemInfo(uint64_t out) {   // SYSTEM_INFO, 32-bit: 36 bytes
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* o = (uint8_t*)(uintptr_t)(uint32_t)out;
    *(uint16_t*)(o + 0) = 0;            // PROCESSOR_ARCHITECTURE_INTEL
    *(uint16_t*)(o + 2) = 0;
    *(uint32_t*)(o + 4) = si.dwPageSize;
    *(uint32_t*)(o + 8) = 0x10000;
    *(uint32_t*)(o + 12) = 0x7FFEFFFF;
    *(uint32_t*)(o + 16) = (uint32_t)si.dwActiveProcessorMask;
    *(uint32_t*)(o + 20) = si.dwNumberOfProcessors;
    *(uint32_t*)(o + 24) = 586;         // PROCESSOR_INTEL_PENTIUM
    *(uint32_t*)(o + 28) = si.dwAllocationGranularity;
    *(uint16_t*)(o + 32) = 6;
    *(uint16_t*)(o + 34) = si.wProcessorRevision;
    return 0;
}
// version.dll: a game checking its DirectX files' versions (Ford Racing: "DirectX 9.0 must be installed") gets a
// version resource for any file that is there or is a system DirectX file: 9.29.952.3111 (DirectX 9.0c's d3d9.dll
// numbering is 5.x-10.x by Windows; this is above all the checks games make)
static const char* DllName32(uint64_t p) { return (const char*)(uintptr_t)(uint32_t)p; }
static uint32_t VersionBlobSize() { return 0x5C + 0x34; }
static uint64_t B_GetFileVersionInfoSizeA(uint64_t name, uint64_t handle) {
    if (handle) *(uint32_t*)(uintptr_t)(uint32_t)handle = 0;
    static int told = 0;
    if (told++ < 8) Log("32-bit program asks the version of %s", DllName32(name));
    return VersionBlobSize();
}
static uint64_t B_GetFileVersionInfoA(uint64_t name, uint64_t, uint64_t len, uint64_t data) {
    uint8_t* d = (uint8_t*)(uintptr_t)(uint32_t)data;
    if ((uint32_t)len < VersionBlobSize()) return 0;
    memset(d, 0, VersionBlobSize());
    // VS_VERSIONINFO header (wLength, wValueLength, wType, "VS_VERSION_INFO", padding) then VS_FIXEDFILEINFO
    *(uint16_t*)d = (uint16_t)VersionBlobSize();
    *(uint16_t*)(d + 2) = 0x34;
    const wchar_t* key = L"VS_VERSION_INFO";
    memcpy(d + 6, key, 32);
    uint32_t* f = (uint32_t*)(d + 0x28);
    f[0] = 0xFEEF04BD;            // signature
    f[1] = 0x00010000;            // struct version
    f[2] = (9 << 16) | 29;        // file version 9.29.952.3111
    f[3] = (952 << 16) | 3111;
    f[4] = f[2];
    f[5] = f[3];
    f[6] = 0x3F;
    f[8] = 0x40004;               // VOS_NT_WINDOWS32
    f[9] = 2;                     // VFT_DLL
    return 1;
}
static uint64_t B_VerQueryValueA(uint64_t block, uint64_t sub, uint64_t outPtr, uint64_t outLen) {
    const char* q = DllName32(sub);
    if (!q || strcmp(q, "\\")) return 0;   // only the fixed info
    uint32_t base = (uint32_t)block;
    *(uint32_t*)(uintptr_t)(uint32_t)outPtr = base + 0x28;
    if (outLen) *(uint32_t*)(uintptr_t)(uint32_t)outLen = 0x34;
    return 1;
}
static uint64_t B_WaitForMultipleObjects(uint64_t n, uint64_t arr, uint64_t all, uint64_t ms) {
    HANDLE h[MAXIMUM_WAIT_OBJECTS];
    WidenHandles(arr, n, h);
    return WaitForMultipleObjects((DWORD)n, h, (BOOL)all, (DWORD)ms);
}
static uint64_t B_WaitForMultipleObjectsEx(uint64_t n, uint64_t arr, uint64_t all, uint64_t ms, uint64_t alertable) {
    HANDLE h[MAXIMUM_WAIT_OBJECTS];
    WidenHandles(arr, n, h);
    return WaitForMultipleObjectsEx((DWORD)n, h, (BOOL)all, (DWORD)ms, (BOOL)alertable);
}
// (no window messages here: the message wait is a plain wait)
static uint64_t B_MsgWaitForMultipleObjects(uint64_t n, uint64_t arr, uint64_t all, uint64_t ms, uint64_t) {
    if (!(uint32_t)n) {
        Sleep((DWORD)ms == INFINITE ? 10 : (DWORD)ms);
        return WAIT_TIMEOUT;
    }
    return B_WaitForMultipleObjects(n, arr, all, ms);
}
static uint64_t B_MsgWaitForMultipleObjectsEx(uint64_t n, uint64_t arr, uint64_t ms, uint64_t, uint64_t flags) {
    if (!(uint32_t)n) {
        Sleep((DWORD)ms == INFINITE ? 10 : (DWORD)ms);
        return WAIT_TIMEOUT;
    }
    return B_WaitForMultipleObjectsEx(n, arr, (flags & 1) /*MWMO_WAITALL*/, ms, (flags & 2) /*MWMO_ALERTABLE*/);
}
// Interlocked*: 32-bit kernel32 exports them; 64-bit Windows has them only as compiler intrinsics
static uint64_t B_InterlockedExchange(uint64_t t, uint64_t v) { return (uint32_t)InterlockedExchange((volatile LONG*)(uintptr_t)(uint32_t)t, (LONG)v); }
static uint64_t B_InterlockedCompareExchange(uint64_t t, uint64_t v, uint64_t c) {
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)(uintptr_t)(uint32_t)t, (LONG)v, (LONG)c);
}
static uint64_t B_InterlockedIncrement(uint64_t t) { return (uint32_t)InterlockedIncrement((volatile LONG*)(uintptr_t)(uint32_t)t); }
static uint64_t B_InterlockedDecrement(uint64_t t) { return (uint32_t)InterlockedDecrement((volatile LONG*)(uintptr_t)(uint32_t)t); }
static uint64_t B_InterlockedExchangeAdd(uint64_t t, uint64_t v) { return (uint32_t)InterlockedExchangeAdd((volatile LONG*)(uintptr_t)(uint32_t)t, (LONG)v); }
static const Known kKnown[] = {
    {"kernel32.dll", "GetTickCount", 0, false, nullptr},
    {"kernel32.dll", "GetCurrentProcessId", 0, false, nullptr},
    {"kernel32.dll", "GetCurrentThreadId", 0, false, nullptr},
    {"kernel32.dll", "Sleep", 1, false, nullptr},
    {"kernel32.dll", "GetLastError", 0, false, nullptr},
    {"kernel32.dll", "SetLastError", 1, false, nullptr},
    {"kernel32.dll", "lstrlenA", 1, false, nullptr},
    {"kernel32.dll", "GetModuleHandleA", 1, false, (void*)B_GetModuleHandleA2},
    {"kernel32.dll", "OutputDebugStringA", 1, false, (void*)B_OutputDebugStringA},
    {"kernel32.dll", "ExitProcess", 1, false, (void*)B_ExitProcess},
    {"kernel32.dll", "ExitThread", 1, false, (void*)B_ExitThread},
    {"kernel32.dll", "CreateThread", 6, false, (void*)B_CreateThread},
    {"kernel32.dll", "RaiseException", 4, false, (void*)B_RaiseException},
    {"kernel32.dll", "RtlUnwind", 4, false, (void*)B_RtlUnwind},
    {"ntdll.dll", "RtlUnwind", 4, false, (void*)B_RtlUnwind},
    {"user32.dll", "MessageBoxA", 4, false, (void*)B_MessageBoxA},
    {"kernel32.dll", "GetProcessHeap", 0, false, (void*)B_GetProcessHeap},
    {"kernel32.dll", "HeapCreate", 3, false, (void*)B_HeapCreate},
    {"kernel32.dll", "HeapAlloc", 3, false, (void*)B_HeapAlloc},
    {"kernel32.dll", "HeapFree", 3, false, (void*)B_HeapFree},
    {"kernel32.dll", "HeapSize", 3, false, (void*)B_HeapSize},
    {"kernel32.dll", "HeapReAlloc", 4, false, (void*)B_HeapReAlloc},
    {"kernel32.dll", "HeapDestroy", 1, false, (void*)B_HeapDestroy},
    {"kernel32.dll", "LocalAlloc", 2, false, (void*)B_LocalAlloc},
    {"kernel32.dll", "GlobalAlloc", 2, false, (void*)B_LocalAlloc},
    {"kernel32.dll", "LocalFree", 1, false, (void*)B_LocalFree},
    {"kernel32.dll", "GlobalFree", 1, false, (void*)B_LocalFree},
    {"kernel32.dll", "VirtualAlloc", 4, false, (void*)B_VirtualAlloc},
    {"kernel32.dll", "GetCommandLineA", 0, false, (void*)B_GetCommandLineA},
    {"kernel32.dll", "GetCommandLineW", 0, false, (void*)B_GetCommandLineW},
    {"kernel32.dll", "LoadLibraryA", 1, false, (void*)B_LoadLibraryA},
    {"kernel32.dll", "LoadLibraryW", 1, false, (void*)B_LoadLibraryW},
    {"kernel32.dll", "LoadLibraryExA", 3, false, (void*)B_LoadLibraryExA},
    {"kernel32.dll", "LoadLibraryExW", 3, false, (void*)B_LoadLibraryExW},
    {"kernel32.dll", "FreeLibrary", 1, false, (void*)B_FreeLibrary},
    {"kernel32.dll", "GetModuleHandleW", 1, false, (void*)B_GetModuleHandleW},
    {"kernel32.dll", "GetProcAddress", 2, false, (void*)B_GetProcAddress},
    {"kernel32.dll", "InitializeCriticalSection", 1, false, (void*)B_InitializeCriticalSection},
    {"kernel32.dll", "InitializeCriticalSectionAndSpinCount", 2, false, (void*)B_InitializeCriticalSectionAndSpinCount},
    {"kernel32.dll", "InitializeCriticalSectionEx", 3, false, (void*)B_InitializeCriticalSectionEx},
    {"kernel32.dll", "EnterCriticalSection", 1, false, (void*)B_EnterCriticalSection},
    {"kernel32.dll", "LeaveCriticalSection", 1, false, (void*)B_LeaveCriticalSection},
    {"kernel32.dll", "TryEnterCriticalSection", 1, false, (void*)B_TryEnterCriticalSection},
    {"kernel32.dll", "DeleteCriticalSection", 1, false, (void*)B_DeleteCriticalSection},
    {"kernel32.dll", "InitializeSRWLock", 1, false, (void*)B_InitializeSRWLock},
    {"kernel32.dll", "AcquireSRWLockExclusive", 1, false, (void*)B_AcquireSRWLockExclusive},
    {"kernel32.dll", "AcquireSRWLockShared", 1, false, (void*)B_AcquireSRWLockShared},
    {"kernel32.dll", "ReleaseSRWLockExclusive", 1, false, (void*)B_ReleaseSRWLockExclusive},
    {"kernel32.dll", "ReleaseSRWLockShared", 1, false, (void*)B_ReleaseSRWLockShared},
    {"kernel32.dll", "TryAcquireSRWLockExclusive", 1, false, (void*)B_TryAcquireSRWLockExclusive},
    {"kernel32.dll", "TryAcquireSRWLockShared", 1, false, (void*)B_TryAcquireSRWLockShared},
    {"kernel32.dll", "InitializeConditionVariable", 1, false, (void*)B_InitializeConditionVariable},
    {"kernel32.dll", "SleepConditionVariableCS", 3, false, (void*)B_SleepConditionVariableCS},
    {"kernel32.dll", "SleepConditionVariableSRW", 4, false, (void*)B_SleepConditionVariableSRW},
    {"kernel32.dll", "WakeConditionVariable", 1, false, (void*)B_WakeConditionVariable},
    {"kernel32.dll", "WakeAllConditionVariable", 1, false, (void*)B_WakeAllConditionVariable},
    {"kernel32.dll", "InitializeSListHead", 1, false, (void*)B_InitializeSListHead},
    {"kernel32.dll", "GlobalMemoryStatus", 1, false, (void*)B_GlobalMemoryStatus},
    {"kernel32.dll", "AddVectoredExceptionHandler", 2, false, (void*)B_AddVectoredHandler32},
    {"kernel32.dll", "AddVectoredContinueHandler", 2, false, (void*)B_AddVectoredHandler32},
    {"kernel32.dll", "RemoveVectoredExceptionHandler", 1, false, (void*)B_RemoveVectoredHandler32},
    {"kernel32.dll", "SetUnhandledExceptionFilter", 1, false, (void*)B_SetUnhandledFilter32},
    {"user32.dll", "EnumDisplayMonitors", 4, false, (void*)B_EnumDisplayMonitors32},
    {"gdi32.dll", "TextOutA", 5, false, (void*)B_TextOutA32},
    {"advapi32.dll", "AllocateAndInitializeSid", 11, false, (void*)B_AllocateAndInitializeSid},
    {"advapi32.dll", "FreeSid", 1, false, (void*)B_FreeSid},
    {"advapi32.dll", "InitializeSecurityDescriptor", 2, false, (void*)B_InitializeSecurityDescriptor},
    {"advapi32.dll", "SetSecurityDescriptorDacl", 4, false, (void*)B_SetSecurityDescriptorDacl},
    {"kernel32.dll", "CreateMutexA", 3, false, (void*)B_CreateMutexA32},
    {"kernel32.dll", "CreateMutexW", 3, false, (void*)B_CreateMutexW32},
    {"kernel32.dll", "CreateEventA", 4, false, (void*)B_CreateEventA32},
    {"kernel32.dll", "CreateEventW", 4, false, (void*)B_CreateEventW32},
    {"kernel32.dll", "CreateSemaphoreA", 4, false, (void*)B_CreateSemaphoreA32},
    {"kernel32.dll", "CreateSemaphoreW", 4, false, (void*)B_CreateSemaphoreW32},
    {"setupapi.dll", "SetupDiGetClassDevsA", 4, false, (void*)B_SetupDiGetClassDevsA},
    {"setupapi.dll", "SetupDiGetClassDevsExA", 7, false, (void*)B_SetupDiGetClassDevsExA},
    {"setupapi.dll", "SetupDiEnumDeviceInterfaces", 5, false, (void*)B_SetupDiEnumDeviceInterfaces},
    {"setupapi.dll", "SetupDiGetDeviceInterfaceDetailA", 6, false, (void*)B_SetupDiGetDeviceInterfaceDetailA},
    {"setupapi.dll", "SetupDiEnumDeviceInfo", 3, false, (void*)B_SetupDiEnumDeviceInfo},
    {"setupapi.dll", "SetupDiDestroyDeviceInfoList", 1, false, (void*)B_SetupDiDestroyDeviceInfoList},
    {"setupapi.dll", "SetupDiGetDeviceRegistryPropertyA", 7, false, (void*)B_SetupDiFalse7},
    {"setupapi.dll", "SetupDiGetDeviceInfoListDetailA", 2, false, (void*)B_SetupDiFalse2},
    {"setupapi.dll", "SetupDiOpenDevRegKey", 6, false, (void*)B_SetupDiOpenDevRegKey},
    {"setupapi.dll", "CM_Get_Device_ID_ExA", 5, false, (void*)B_CM_Get_Device_ID_ExA},
    {"ddraw.dll", "DirectDrawCreateEx", 4, false, (void*)B_DirectDrawCreateEx32},
    {"kernel32.dll", "FindFirstFileA", 2, false, (void*)B_FindFirstFileA32},
    {"kernel32.dll", "FindFirstFileW", 2, false, (void*)B_FindFirstFileW32},
    {"kernel32.dll", "FindFirstFileExA", 6, false, (void*)B_FindFirstFileExA32},
    {"kernel32.dll", "FindFirstFileExW", 6, false, (void*)B_FindFirstFileExW32},
    {"kernel32.dll", "FindNextFileA", 2, false, (void*)B_FindNextFileA32},
    {"kernel32.dll", "FindNextFileW", 2, false, (void*)B_FindNextFileW32},
    {"kernel32.dll", "FindClose", 1, false, (void*)B_FindClose32},
    {"user32.dll", "GetWindowThreadProcessId", 2, false, (void*)B_GetWindowThreadProcessId32},
    {"user32.dll", "GetClassNameA", 3, false, (void*)B_GetClassNameA32},
    {"kernel32.dll", "RemoveVectoredContinueHandler", 1, false, (void*)B_RemoveVectoredHandler32},
    {"kernel32.dll", "VirtualProtect", 4, false, (void*)B_VirtualProtect},
    {"shlwapi.dll", "PathFileExistsA", 1, false, (void*)B_PathFileExistsA},
    {"shlwapi.dll", "PathFileExistsW", 1, false, (void*)B_PathFileExistsW},
    {"shlwapi.dll", "PathCombineA", 3, false, (void*)B_PathCombineA},
    {"shlwapi.dll", "PathAppendA", 2, false, (void*)B_PathAppendA},
    {"shlwapi.dll", "PathRemoveFileSpecA", 1, false, (void*)B_PathRemoveFileSpecA},
    {"kernel32.dll", "GetFullPathNameA", 4, false, (void*)B_GetFullPathNameA},
    {"kernel32.dll", "GetFullPathNameW", 4, false, (void*)B_GetFullPathNameW},

    {"psapi.dll", "GetModuleInformation", 4, false, (void*)B_GetModuleInformation},
    {"kernel32.dll", "K32GetModuleInformation", 4, false, (void*)B_GetModuleInformation},
    {"kernel32.dll", "GetModuleFileNameA", 3, false, (void*)B_GetModuleFileNameA},
    {"kernel32.dll", "GetModuleFileNameW", 3, false, (void*)B_GetModuleFileNameW},
    {"winmm.dll", "timeGetSystemTime", 2, false, (void*)B_timeGetSystemTime},
    {"user32.dll", "RegisterDeviceNotificationA", 3, false, (void*)B_RegisterDeviceNotification},
    {"user32.dll", "RegisterDeviceNotificationW", 3, false, (void*)B_RegisterDeviceNotification},
    {"user32.dll", "UnregisterDeviceNotification", 1, false, (void*)B_UnregisterDeviceNotification},
    {"kernel32.dll", "FindResourceA", 3, false, (void*)B_FindResourceA},
    {"kernel32.dll", "FindResourceW", 3, false, (void*)B_FindResourceW},
    {"kernel32.dll", "FindResourceExA", 4, false, (void*)B_FindResourceExA},
    {"kernel32.dll", "FindResourceExW", 4, false, (void*)B_FindResourceExW},
    {"kernel32.dll", "LoadResource", 2, false, (void*)B_LoadResource},
    {"kernel32.dll", "LockResource", 1, false, (void*)B_LockResource},
    {"kernel32.dll", "SizeofResource", 2, false, (void*)B_SizeofResource},
    {"kernel32.dll", "FreeResource", 1, false, (void*)B_FreeResource},
    {"kernel32.dll", "DuplicateHandle", 7, false, (void*)B_DuplicateHandle},
    {"kernel32.dll", "GetFileAttributesA", 1, false, (void*)B_GetFileAttributesA},
    {"kernel32.dll", "GetDriveTypeA", 1, false, (void*)B_GetDriveTypeA},
    {"kernel32.dll", "GetDiskFreeSpaceA", 5, false, (void*)B_GetDiskFreeSpaceA},
    {"kernel32.dll", "SuspendThread", 1, false, (void*)B_SuspendThread},
    {"kernel32.dll", "ResumeThread", 1, false, (void*)B_ResumeThread},
    {"user32.dll", "SetRect", 5, false, (void*)B_SetRect},
    {"user32.dll", "SetRectEmpty", 1, false, (void*)B_SetRectEmpty},
    {"user32.dll", "CharUpperA", 1, false, (void*)B_CharUpperA},
    {"user32.dll", "CharLowerA", 1, false, (void*)B_CharLowerA},
    {"user32.dll", "LoadMenuA", 2, false, (void*)B_Null2},
    {"user32.dll", "LoadAcceleratorsA", 2, false, (void*)B_Null2},
    {"iphlpapi.dll", "GetInterfaceInfo", 2, false, (void*)B_GetInterfaceInfo},
    {"iphlpapi.dll", "GetAdaptersInfo", 2, false, (void*)B_GetAdaptersInfo},
    {"winmm.dll", "timeSetEvent", 5, false, (void*)B_timeSetEvent},
    {"winmm.dll", "timeKillEvent", 1, false, (void*)B_timeKillEvent},
    {"winmm.dll", "joyGetNumDevs", 0, false, (void*)B_joyGetNumDevs},
    {"winmm.dll", "mmioOpenA", 3, false, (void*)B_mmioOpenA},
    {"winmm.dll", "mmioClose", 2, false, (void*)B_mmioClose},
    {"winmm.dll", "mmioRead", 3, false, (void*)B_mmioRead},
    {"winmm.dll", "mmioWrite", 3, false, (void*)B_mmioWrite},
    {"winmm.dll", "mmioSeek", 3, false, (void*)B_mmioSeek},
    {"winmm.dll", "mmioDescend", 4, false, (void*)B_mmioDescend},
    {"winmm.dll", "mmioAscend", 3, false, (void*)B_mmioAscend},
    {"winmm.dll", "mmioGetInfo", 3, false, (void*)B_mmioGetInfo},
    {"winmm.dll", "mmioSetInfo", 3, false, (void*)B_mmioSetInfo},
    {"winmm.dll", "mmioAdvance", 3, false, (void*)B_mmioAdvance},
    {"winmm.dll", "mmioCreateChunk", 3, false, (void*)B_mmioCreateChunk},
    {"iphlpapi.dll", "GetNetworkParams", 2, false, (void*)B_GetNetworkParams},
    {"kernel32.dll", "OpenProcess", 3, false, (void*)B_OpenProcess},
    {"advapi32.dll", "CryptAcquireContextA", 5, false, (void*)B_CryptAcquireContextA},
    {"advapi32.dll", "CryptAcquireContextW", 5, false, (void*)B_CryptAcquireContextW},
    {"advapi32.dll", "CryptReleaseContext", 2, false, (void*)B_CryptReleaseContext},
    {"advapi32.dll", "CryptCreateHash", 5, false, (void*)B_CryptCreateHash},
    {"advapi32.dll", "CryptHashData", 4, false, (void*)B_CryptHashData},
    {"advapi32.dll", "CryptGetHashParam", 5, false, (void*)B_CryptGetHashParam},
    {"advapi32.dll", "CryptDeriveKey", 5, false, (void*)B_CryptDeriveKey},
    {"advapi32.dll", "CryptEncrypt", 7, false, (void*)B_CryptEncrypt},
    {"advapi32.dll", "CryptDecrypt", 6, false, (void*)B_CryptDecrypt},
    {"advapi32.dll", "CryptDestroyHash", 1, false, (void*)B_CryptDestroyHash},
    {"advapi32.dll", "CryptDestroyKey", 1, false, (void*)B_CryptDestroyKey},
    {"kernel32.dll", "CreateFileMappingA", 6, false, (void*)B_CreateFileMappingA},
    {"kernel32.dll", "CreateFileMappingW", 6, false, (void*)B_CreateFileMappingW},
    {"kernel32.dll", "OpenFileMappingA", 3, false, (void*)B_OpenFileMappingA},
    {"kernel32.dll", "OpenFileMappingW", 3, false, (void*)B_OpenFileMappingW},
    {"kernel32.dll", "MapViewOfFile", 5, false, (void*)B_MapViewOfFile},
    {"kernel32.dll", "MapViewOfFileEx", 6, false, (void*)B_MapViewOfFileEx},
    {"kernel32.dll", "UnmapViewOfFile", 1, false, (void*)B_UnmapViewOfFile},
    {"kernel32.dll", "FlushViewOfFile", 2, false, (void*)B_FlushViewOfFile},
    {"advapi32.dll", "RegOpenKeyExA", 5, false, (void*)B_RegOpenKeyExA},
    {"advapi32.dll", "RegOpenKeyExW", 5, false, (void*)B_RegOpenKeyExW},
    {"advapi32.dll", "RegOpenKeyA", 3, false, (void*)B_RegOpenKeyA},
    {"advapi32.dll", "RegCreateKeyExA", 9, false, (void*)B_RegCreateKeyExA},
    {"advapi32.dll", "RegCreateKeyExW", 9, false, (void*)B_RegCreateKeyExW},
    {"advapi32.dll", "RegCreateKeyA", 3, false, (void*)B_RegCreateKeyA},
    {"advapi32.dll", "RegCloseKey", 1, false, (void*)B_RegCloseKey},
    {"advapi32.dll", "RegQueryValueExA", 6, false, (void*)B_RegQueryValueExA},
    {"advapi32.dll", "RegQueryValueExW", 6, false, (void*)B_RegQueryValueExW},
    {"advapi32.dll", "RegSetValueExA", 6, false, (void*)B_RegSetValueExA},
    {"advapi32.dll", "RegSetValueExW", 6, false, (void*)B_RegSetValueExW},
    {"advapi32.dll", "RegDeleteValueA", 2, false, (void*)B_RegDeleteValueA},
    {"advapi32.dll", "RegFlushKey", 1, false, (void*)B_RegFlushKey},
    {"kernel32.dll", "CreateFileA", 7, false, (void*)B_CreateFileA},
    {"kernel32.dll", "CreateFileW", 7, false, (void*)B_CreateFileW},
    {"version.dll", "GetFileVersionInfoSizeA", 2, false, (void*)B_GetFileVersionInfoSizeA},
    {"version.dll", "GetFileVersionInfoA", 4, false, (void*)B_GetFileVersionInfoA},
    {"version.dll", "VerQueryValueA", 4, false, (void*)B_VerQueryValueA},
    {"kernel32.dll", "GetSystemInfo", 1, false, (void*)B_GetSystemInfo},
    {"kernel32.dll", "GetNativeSystemInfo", 1, false, (void*)B_GetSystemInfo},
    {"kernel32.dll", "WaitForMultipleObjects", 4, false, (void*)B_WaitForMultipleObjects},
    {"kernel32.dll", "WaitForMultipleObjectsEx", 5, false, (void*)B_WaitForMultipleObjectsEx},
    {"user32.dll", "MsgWaitForMultipleObjects", 5, false, (void*)B_MsgWaitForMultipleObjects},
    {"user32.dll", "MsgWaitForMultipleObjectsEx", 5, false, (void*)B_MsgWaitForMultipleObjectsEx},
    {"ole32.dll", "CoInitialize", 1, false, (void*)B_CoInitialize32},
    {"ole32.dll", "CoInitializeEx", 2, false, (void*)B_CoInitializeEx32},
    {"ole32.dll", "CoUninitialize", 0, false, (void*)B_CoUninitialize32},
    {"ole32.dll", "CoCreateInstance", 5, false, (void*)B_CoCreateInstance32},
    {"kernel32.dll", "InterlockedExchange", 2, false, (void*)B_InterlockedExchange},
    {"kernel32.dll", "InterlockedCompareExchange", 3, false, (void*)B_InterlockedCompareExchange},
    {"kernel32.dll", "InterlockedIncrement", 1, false, (void*)B_InterlockedIncrement},
    {"kernel32.dll", "InterlockedDecrement", 1, false, (void*)B_InterlockedDecrement},
    {"kernel32.dll", "InterlockedExchangeAdd", 2, false, (void*)B_InterlockedExchangeAdd},
    {"kernel32.dll", "InterlockedPushEntrySList", 2, false, (void*)B_InterlockedPushEntrySList},
    {"kernel32.dll", "InterlockedPopEntrySList", 1, false, (void*)B_InterlockedPopEntrySList},
    {"kernel32.dll", "InterlockedFlushSList", 1, false, (void*)B_InterlockedFlushSList},
    {"kernel32.dll", "QueryDepthSList", 1, false, (void*)B_QueryDepthSList},
    {"kernel32.dll", "FlsAlloc", 1, false, (void*)B_FlsAlloc},
    {"kernel32.dll", "GetStartupInfoW", 1, false, (void*)B_GetStartupInfo},
    {"kernel32.dll", "GetStartupInfoA", 1, false, (void*)B_GetStartupInfo},
    {"kernel32.dll", "EncodePointer", 1, false, (void*)B_EncodePointer},
    {"kernel32.dll", "DecodePointer", 1, false, (void*)B_DecodePointer},
    {"kernel32.dll", "GetEnvironmentStringsW", 0, false, (void*)B_GetEnvironmentStringsW},
    {"kernel32.dll", "GetEnvironmentStrings", 0, false, (void*)B_GetEnvironmentStringsA},
    {"kernel32.dll", "GetEnvironmentStringsA", 0, false, (void*)B_GetEnvironmentStringsA},
    {"kernel32.dll", "FreeEnvironmentStringsW", 1, false, (void*)B_FreeEnvironmentStrings},
    {"kernel32.dll", "FreeEnvironmentStringsA", 1, false, (void*)B_FreeEnvironmentStrings},
};

// every other function: argument bytes from the SDK's 32-bit import libraries (tools/make_argtable.py)
struct ArgRow {
    const char* dll;
    const char* name;
    int bytes;
    int cdecl_;
};
static const ArgRow kArgs[] = {
#include "argtable32.inc"
};
static const ArgRow* FindArgs(const std::string& dll, const std::string& fn) {
    for (const ArgRow& r : kArgs)
        if (fn == r.name && dll == r.dll) return &r;
    return nullptr;
}
// "api-ms-win-core-synch-l1-2-0.dll" and the like are kernel32 to a 32-bit program
static std::string RealDll32(const std::string& dll) {
    if (!dll.compare(0, 16, "api-ms-win-core-") || dll == "kernelbase.dll") return "kernel32.dll";
    return dll;
}
void* FindD3D9Bridge32(const char* dll, const char* func, int& args);   // d3d9_bridge32.cpp
void* FindWinsock32(const char* dll, const char* func, int& args);     // winsock32.cpp
void* FindAcio32(const char* dll, const char* func, int& args);
void* FindGdi32(const char* dll, const char* func, int& args);         // gdi32.cpp        // acio32.cpp
void* FindGvAudio32(const char* dll, const char* func, int& args);
void* FindGvIo32(const char* dll, const char* func, int& args);        // arcade_io.cpp     // gvaudio.cpp: our DirectSound's sound out
static void* ResolveFor32(const std::string& dllIn, const std::string& fn, int& args, bool& cdecl_) {
    std::string dll = RealDll32(dllIn);
    if (void* d = FindD3D9Bridge32(dll.c_str(), fn.c_str(), args)) {   // Direct3D 9 through the bridge (DXVK)
        cdecl_ = false;
        return d;
    }
    if (void* g = FindGvAudio32(dll.c_str(), fn.c_str(), args)) {
        cdecl_ = false;
        return g;
    }
    if (void* g = FindGvIo32(dll.c_str(), fn.c_str(), args)) {   // our iDmacDrv32.dll's Fast I/O board (arcade_io.cpp)
        cdecl_ = false;
        return g;
    }
    if (void* g = FindGdi32(dll.c_str(), fn.c_str(), args)) {   // GDI text through DirectWrite (gdi32.cpp)
        cdecl_ = false;
        return g;
    }
    if (void* a = FindAcio32(dll.c_str(), fn.c_str(), args)) {   // Konami's I/O library (acio32.cpp), __cdecl
        cdecl_ = true;
        return a;
    }
    if (void* w = FindWinsock32(dll.c_str(), fn.c_str(), args)) {   // Winsock, structures converted
        cdecl_ = false;
        return w;
    }
    for (const Known& k : kKnown) {
        if (dll != k.dll || fn != k.name) continue;
        args = k.args, cdecl_ = k.cdecl_;
        if (k.shim) return k.shim;
        HMODULE m = GetModuleHandleA("kernelbase.dll");
        if (FARPROC p = m ? GetProcAddress(m, k.name) : nullptr) return (void*)p;
        if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) return (void*)GetProcAddress(k32, k.name);
        return nullptr;
    }
    const ArgRow* r = FindArgs(dll, fn);
    if (!r) return nullptr;
    args = r->bytes / 4, cdecl_ = r->cdecl_ != 0;
    if (void* t = FindShim(dll.c_str(), fn.c_str())) return t;
    HMODULE m = GetModuleHandleA(dll.c_str());
    if (!m && dll == "kernel32.dll") m = GetModuleHandleA("kernelbase.dll");
    void* t = m ? (void*)GetProcAddress(m, fn.c_str()) : nullptr;
    if (!t && dll == "kernel32.dll")
        if (HMODULE kb = GetModuleHandleA("kernelbase.dll")) t = (void*)GetProcAddress(kb, fn.c_str());
    return t;
}

// --- run-time lookups by 32-bit code: modules get handles below 4 GB (one per DLL name, nothing mapped there), and
// GetProcAddress makes a bridge entry and thunk for the function, as for an import
static std::vector<std::string> g_mods32;
static uint32_t MakeThunk32(uint32_t index);
static void* ResolveFor32(const std::string& dll, const std::string& fn, int& args, bool& cdecl_);
static std::string Lower(std::string s);
static std::string Base32(const char* p) {
    std::string s = p ? p : "";
    size_t k = s.find_last_of("\\/");
    if (k != std::string::npos) s = s.substr(k + 1);
    if (s.find('.') == std::string::npos) s += ".dll";
    return Lower(s);
}
// a stand-in module handle points at a small real DLL header of its own (MZ, PE32, an empty export table): code that
// reads a module's headers itself (dxwrapper's DDrawCompat looks exports up that way) finds no export instead of
// faulting
static void FakeHeader32(uint32_t at, const std::string& dll) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    uint8_t* h = (uint8_t*)VirtualAllocFromApp((void*)(uintptr_t)at, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    uint8_t* h = (uint8_t*)VirtualAlloc((void*)(uintptr_t)at, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
    if (h != (uint8_t*)(uintptr_t)at) return;
    *(uint16_t*)h = 0x5A4D;                // MZ
    *(uint32_t*)(h + 0x3C) = 0x40;         // e_lfanew
    uint8_t* nt = h + 0x40;
    *(uint32_t*)nt = 0x00004550;           // PE
    auto fh = (IMAGE_FILE_HEADER*)(nt + 4);
    fh->Machine = IMAGE_FILE_MACHINE_I386;
    fh->SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    fh->Characteristics = IMAGE_FILE_DLL | IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    auto oh = (IMAGE_OPTIONAL_HEADER32*)(fh + 1);
    oh->Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    oh->ImageBase = at;
    oh->SectionAlignment = oh->FileAlignment = 0x1000;
    oh->SizeOfImage = oh->SizeOfHeaders = 0x1000;
    oh->NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = 0x200;
    oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = sizeof(IMAGE_EXPORT_DIRECTORY);
    auto ed = (IMAGE_EXPORT_DIRECTORY*)(h + 0x200);
    ed->Name = 0x300;
    ed->Base = 1;
    ed->AddressOfFunctions = ed->AddressOfNames = ed->AddressOfNameOrdinals = 0x280;
    strncpy_s((char*)h + 0x300, 0x100, dll.c_str(), _TRUNCATE);
    ULONG old;
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    VirtualProtectFromApp(h, 0x1000, PAGE_READONLY, &old);
#else
    VirtualProtect(h, 0x1000, PAGE_READONLY, (DWORD*)&old);
#endif
}
static uint64_t ModHandle32(const std::string& dll) {
    for (size_t i = 0; i < g_mods32.size(); i++)
        if (g_mods32[i] == dll) return 0x7E000000u + i * 0x10000u;
    g_mods32.push_back(dll);
    uint32_t at = 0x7E000000u + (uint32_t)(g_mods32.size() - 1) * 0x10000u;
    FakeHeader32(at, dll);
    return at;
}
struct Mod32;
static Mod32* LoadDll32(const std::string& dll);
static void AddDllTls32(uint32_t dir);
static uint32_t g_realCreateEffect = 0;
uint32_t Call32(uint32_t func, const uint32_t* args, int n);
// D3DXCreateEffectFromFileA(device, file, defines, include, flags, pool, effect**, errors**): the real one, its result
// and its error text logged
static uint64_t Spy_CreateEffect(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint32_t args[8] = {(uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4, (uint32_t)a5, (uint32_t)a6, (uint32_t)a7};
    uint32_t hr = Call32(g_realCreateEffect, args, 8);
    std::string err;
    if ((uint32_t)a7 && *(uint32_t*)(uintptr_t)(uint32_t)a7) {   // ID3DXBuffer: GetBufferPointer is vtable[3]
        uint32_t buf = *(uint32_t*)(uintptr_t)(uint32_t)a7;
        uint32_t fn = ((uint32_t*)(uintptr_t)*(uint32_t*)(uintptr_t)buf)[3];
        uint32_t ptr = Call32(fn, &buf, 1);
        if (ptr) err.assign((const char*)(uintptr_t)ptr, strnlen((const char*)(uintptr_t)ptr, 600));
    }
    Log("D3DXCreateEffectFromFileA(%s, flags 0x%x) = 0x%08x %s", (uint32_t)a1 ? (const char*)(uintptr_t)(uint32_t)a1 : "?", (uint32_t)a4, hr, err.c_str());
    return hr;
}
static std::map<std::string, std::vector<std::string>> g_deps32;   // a 32-bit image's own DLLs it imports from
static void StartDlls32();
static Mod32* FindDll32(uint64_t base);
static Mod32* FindDllByName32(const std::string& dll);
static uint32_t ExportOf32(Mod32* m, const char* name);
static uint32_t BaseOf32(Mod32* m);
static const char* NameOf32(Mod32* m);
static uint64_t B_LoadLibraryA(uint64_t name) {
    std::string dll = Base32((const char*)(uintptr_t)name);
    // a 32-bit DLL of ours or beside the game (DirectInput/DirectSound stand-ins, d3d8to9): mapped and started, as
    // for imports (a bridged 64-bit one would hand the game objects it cannot use)
    if (Mod32* m = LoadDll32(dll)) {
        StartDlls32();
        Log("32-bit program loads %s: 32-bit DLL at 0x%08x", dll.c_str(), BaseOf32(m));
        return BaseOf32(m);
    }
    Log("32-bit program loads %s", dll.c_str());
    return ModHandle32(dll);
}
static uint64_t B_LoadLibraryW(uint64_t name) {
    char n[MAX_PATH] = "";
    if (name) WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)(uintptr_t)name, -1, n, MAX_PATH, nullptr, nullptr);
    return B_LoadLibraryA((uint64_t)(uintptr_t)n);
}
static uint64_t B_LoadLibraryExA(uint64_t name, uint64_t, uint64_t) { return B_LoadLibraryA(name); }
static uint64_t B_LoadLibraryExW(uint64_t name, uint64_t, uint64_t) { return B_LoadLibraryW(name); }
static uint64_t B_FreeLibrary(uint64_t) { return TRUE; }
// code a program puts in memory itself (dxwrapper's MemoryModule loads its own D3DX9 from a resource): when the memory
// is made runnable, its FS thread block accesses move to GS as at load (FsToGs); not for our own mapped images
static int FsToGs(uint8_t* code, size_t size);
static bool InMappedImage32(uint64_t at);
static BOOL Protect(void* at, SIZE_T size, DWORD prot, DWORD* was) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    ULONG w = 0;
    BOOL r = VirtualProtectFromApp(at, size, prot, &w);
    *was = w;
    return r;
#else
    return VirtualProtect(at, size, prot, was);
#endif
}
static uint64_t B_VirtualProtect(uint64_t at, uint64_t size, uint64_t prot, uint64_t old) {
    DWORD p = (DWORD)prot, was = 0;
    bool exec = (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    if (exec && at && size && at < 0x100000000ull && !InMappedImage32(at)) {
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery((void*)(uintptr_t)at, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE &&
            !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            DWORD t;
            if (Protect((void*)(uintptr_t)at, (SIZE_T)size, PAGE_READWRITE, &t)) {
                int n = FsToGs((uint8_t*)(uintptr_t)at, (size_t)size);
                static int told = 0;
                if (n && told++ < 20) Log("32-bit: code made runnable at %08llx (%llu bytes): %d thread block accesses moved from FS to GS", at, size, n);
            }
        }
    }
    BOOL r = Protect((void*)(uintptr_t)at, (SIZE_T)size, p, &was);
    if (old) *(uint32_t*)(uintptr_t)old = was;
    if (r && exec) FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)at, (SIZE_T)size);
    return r;
}
// shlwapi's path helpers (dxwrapper): through the program's own file functions (its paths may need redirecting)
static uint64_t B_GetFileAttributesA(uint64_t name);
static uint64_t B_PathFileExistsA(uint64_t p) {
    return p && (DWORD)B_GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
static uint64_t B_PathFileExistsW(uint64_t p) {
    char n[MAX_PATH] = "";
    if (p) WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)(uintptr_t)p, -1, n, MAX_PATH, nullptr, nullptr);
    return B_PathFileExistsA((uint64_t)(uintptr_t)n);
}
static uint64_t B_PathAppendA(uint64_t path, uint64_t more) {
    char* d = (char*)(uintptr_t)path;
    const char* m = more ? (const char*)(uintptr_t)more : "";
    if (!d) return FALSE;
    while (*m == '\\') m++;
    size_t n = strlen(d);
    if (n && d[n - 1] != '\\' && d[n - 1] != '/' && *m) strcat_s(d, MAX_PATH, "\\");
    return strcat_s(d, MAX_PATH, m) == 0;
}
static uint64_t B_PathCombineA(uint64_t out, uint64_t dir, uint64_t file) {
    char* o = (char*)(uintptr_t)out;
    if (!o) return 0;
    const char* f = file ? (const char*)(uintptr_t)file : "";
    char t[MAX_PATH] = "";
    if (f[0] && (f[1] == ':' || (f[0] == '\\' && f[1] == '\\'))) strcpy_s(t, f);
    else {
        if (dir) strcpy_s(t, (const char*)(uintptr_t)dir);
        B_PathAppendA((uint64_t)(uintptr_t)t, (uint64_t)(uintptr_t)f);
    }
    strcpy_s(o, MAX_PATH, t);
    return out;
}
static uint64_t B_PathRemoveFileSpecA(uint64_t path) {
    char* d = (char*)(uintptr_t)path;
    if (!d) return FALSE;
    char* k = strrchr(d, '\\');
    if (!k) k = strrchr(d, '/');
    if (!k) return FALSE;
    if (k > d && k[-1] == ':') k[1] = 0;
    else *k = 0;
    return TRUE;
}
// a module that is loaded: a mapped 32-bit DLL (its base), a system DLL the program already uses or Windows has
// (a stand-in handle); none for others (dxwrapper checks that "dxwrapper.dll" is not loaded, else it stays off)
static uint64_t LoadedModule32(const std::string& dll) {
    if (Mod32* m = FindDllByName32(dll)) return BaseOf32(m);
    for (size_t i = 0; i < g_mods32.size(); i++)
        if (g_mods32[i] == dll) return 0x7E000000u + i * 0x10000u;
    static const char* const kSystem[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ntdll.dll", "ole32.dll",
                                          "oleaut32.dll", "shell32.dll", "winmm.dll", "ws2_32.dll", "wsock32.dll", "version.dll",
                                          "shlwapi.dll", "imm32.dll", "kernelbase.dll", "comctl32.dll", "setupapi.dll"};
    for (const char* k : kSystem)
        if (dll == k) return ModHandle32(dll);
    if (!dll.compare(0, 7, "api-ms-") || !dll.compare(0, 7, "ext-ms-")) return ModHandle32(dll);
    return 0;
}
static uint64_t B_GetModuleHandleW(uint64_t name) {
    if (!name) return g_img32->base;
    char n[MAX_PATH] = "";
    WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)(uintptr_t)name, -1, n, MAX_PATH, nullptr, nullptr);
    return LoadedModule32(Base32(n));
}
static uint64_t B_GetModuleHandleA2(uint64_t name) {
    if (!name) return g_img32->base;
    return LoadedModule32(Base32((const char*)(uintptr_t)name));
}
static uint64_t B_GetProcAddress(uint64_t mod, uint64_t name) {
    if (Mod32* m = FindDll32(mod)) {   // a mapped 32-bit DLL: its own export
        uint32_t a = name > 0xFFFF ? ExportOf32(m, (const char*)(uintptr_t)name) : 0;
        if (!a) Log("32-bit program asks for %s!%s: none", NameOf32(m), name > 0xFFFF ? (const char*)(uintptr_t)name : "#");
        return a;
    }
    if (name <= 0xFFFF || mod == g_img32->base) return 0;   // by ordinal / the program's own exports: not kept
    uint32_t i = (uint32_t)((mod - 0x7E000000u) / 0x10000u);
    if (mod < 0x7E000000u || i >= g_mods32.size()) return 0;
    std::string dll = g_mods32[i], fn = (const char*)(uintptr_t)name;
    int args = 0;
    bool cdecl_ = false;
    void* t = ResolveFor32(dll, fn, args, cdecl_);
    if (!t) {
        Log("32-bit program asks for %s!%s: none", dll.c_str(), fn.c_str());
        return 0;
    }
    g_fn.push_back({dll + "!" + fn, t, args, cdecl_});
    return MakeThunk32((uint32_t)(g_fn.size() - 1));
}

// --- per run state (one 32-bit thread) ---------------------------------------------------------------------
static uint8_t* g_arena = nullptr;   // below 4 GB: 64-bit switch code, thunks, the 32-bit stack, our state
struct State {
    uint64_t saved64;      // 64-bit stack pointer at the switch into 32-bit code
    uint32_t index;        // the function a thunk asked for
    uint32_t esp32;        // the 32-bit stack pointer at that call
    uint32_t resumeEip, resumeEsp, eax, edx;
    uint64_t teb32;        // +0x20: the 32-bit thread block
    uint32_t exiting;      // +0x28: ExitProcess / ExitThread asked: the next return goes to g_exit32
    int32_t exitCode;      // +0x2C
    uint32_t stackTop;     // +0x30: this thread's 32-bit stack top
    uint32_t redirectEip;  // set by a bridged function (RaiseException): go there instead of back to the caller
    uint32_t redirectEsp;
};
// each thread's own: inside its 32-bit thread block at +0x800 (the GDI/user fields there are unused by game code), which sits at the low 32 bits of its 64-bit TEB
// (MakeTeb32); the bridge code finds it the same way, through GS
// THE THREAD BLOCK AREA. In 32-bit mode a GS access lands at the low 32 bits of the thread's 64-bit TEB plus the
// address in the instruction (wrapping at 4 GB). Windows keeps memory of its own around there (thread stacks on the
// Xbox), so every thread block goes into one 64 MB area we reserve below 2 GB instead: the address in each rewritten
// instruction (FsToGs) and the bridge's lookup get g_tebShift added, chosen so the loader thread's block sits in the
// middle of the area. Threads' TEBs lie close together, so theirs land in the area too.
static uint8_t* g_tebArea = nullptr;
static uint32_t g_tebShift = 0;
static const size_t kTebArea = 64u << 20;
static bool TebAreaReady() {
    if (g_tebArea) return true;
    // high up first (0x70000000): clear of the program image (Caladrius: 269 MB from 0x400000), its DLLs and the
    // low heap; then anywhere from 128 MB
    for (uintptr_t at = 0x70000000; at + kTebArea < 0x80000000 && !g_tebArea; at += 0x01000000)
        g_tebArea = (uint8_t*)VirtualAlloc((void*)at, kTebArea, MEM_RESERVE, PAGE_READWRITE);
    for (uintptr_t at = 0x08000000; at + kTebArea < 0x80000000 && !g_tebArea; at += 0x01000000)
        g_tebArea = (uint8_t*)VirtualAlloc((void*)at, kTebArea, MEM_RESERVE, PAGE_READWRITE);
    if (!g_tebArea) return false;
    uint32_t low = (uint32_t)(uintptr_t)NtCurrentTeb();
    g_tebShift = (uint32_t)((uintptr_t)g_tebArea + kTebArea / 2) - low;
    g_tebShift &= ~0xFFFu;   // whole pages
    Log("32-bit thread block area %p (+%zu MB), shift 0x%08x", g_tebArea, kTebArea >> 20, g_tebShift);
    return true;
}
static uint8_t* Teb32Of() { return (uint8_t*)(uintptr_t)(uint32_t)((uint32_t)(uintptr_t)NtCurrentTeb() + g_tebShift); }

static State* St() { return (State*)(Teb32Of() + 0x800); }
// a 64-bit thread that never ran 32-bit code has no such page (the crash logger asks first)
bool InProgram32();
static bool HasState32() {
    MEMORY_BASIC_INFORMATION mi = {};
    return VirtualQuery(St(), &mi, sizeof mi) && mi.State == MEM_COMMIT && (mi.Protect & PAGE_READWRITE);
}
bool InProgram32() { return HasState32(); }   // this thread is running the 32-bit program
static uint8_t* g_exit32 = nullptr;     // 32-bit code the entry point returns to

// the program's ExitProcess: Dispatch sends the 32-bit side straight to g_exit32, which leaves 32-bit mode as a
// return from the entry point does (no unwinding through code without unwind data)
static uint64_t B_ExitProcess(uint64_t code) {
    St()->exiting = 1;
    St()->exitCode = (int)code;
    return code;
}
static uint64_t B_ExitThread(uint64_t code) { return B_ExitProcess(code); }   // this thread's 32-bit code ends

// called on the 64-bit stack by the bridge code
// testing: ARCADE_RING=<file> (PC build): every bridged call's name into a memory-mapped file, which keeps what
// was written even when the process dies, with no system call per line
static char* g_ring = nullptr;
static size_t g_ringPos = 0, g_ringSize = 0;
static void RingOpen() {
#if !(defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP)
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("ARCADE_RING", path, MAX_PATH)) return;
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    g_ringSize = 4 << 20;
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, (DWORD)g_ringSize, nullptr);
    g_ring = m ? (char*)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, g_ringSize) : nullptr;
#endif
}
static void RingPut(const std::string& s, uint32_t ret) {
    if (!g_ring) return;
    char line[160];
    int n = snprintf(line, sizeof line, "%s <- %08x\n", s.c_str(), ret);
    if (n <= 0) return;
    if (g_ringPos + n >= g_ringSize) g_ringPos = 0;
    memcpy(g_ring + g_ringPos, line, n);
    g_ringPos += n;
}

bool g_trace32 = false;   // testing: log each bridged function's first 3 calls (SetTrace)
static DWORD g_dllStartTid = 0;   // (testing, with trace.txt) a DLL's start: every call it makes
static int g_dllStartCalls = 0;
static void Dispatch() {
    uint32_t i = St()->index;
    if (g_trace32 && g_dllStartTid == GetCurrentThreadId() && i < g_fn.size() && g_dllStartCalls++ < 4000) {
        const uint32_t* a = (const uint32_t*)(uintptr_t)St()->esp32;
        Log("  DLL start call: %s (%08x %08x %08x) from %08x", g_fn[i].name.c_str(), a[1], a[2], a[3], a[0]);
    }
    static const bool all = GetEnvironmentVariableA("ARCADE_TRACE_ALL", nullptr, 0) > 0;
    if (g_trace32 && i < g_fn.size() && (all || g_fn[i].calls < 40)) Log("32-bit call [%lu]: %s", GetCurrentThreadId(), g_fn[i].name.c_str());
    const uint32_t* stack = (const uint32_t*)(uintptr_t)St()->esp32;   // [0] return address, [1..] arguments
    uint64_t r = 0;
    if (i < g_fn.size()) {
        Bridged& f = g_fn[i];
        uint64_t a[12] = {};
        // sign-extended, as WoW64 does: pseudo handles (-1 process, -2 thread, -3..-6 tokens), INVALID_HANDLE_VALUE and
        // negative numbers stay what they are; every address a 32-bit program gets is below 2 GB, so none turns negative
        for (int k = 0; k < f.args && k < 12; k++) a[k] = (uint64_t)(int64_t)(int32_t)stack[1 + k];
        if (!f.target) {
            if (!f.calls) Log("32-bit program calls missing %s (returns 0)", f.name.c_str());
        } else {
            typedef uint64_t (*f12)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
            r = ((f12)f.target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
        }
        f.calls++;
        St()->resumeEsp = St()->esp32 + 4 + (f.cdecl_ ? 0 : 4 * f.args);
    } else {
        St()->resumeEsp = St()->esp32 + 4;
    }
    St()->resumeEip = stack[0];
    if (i < g_fn.size()) RingPut(g_fn[i].name, stack[0]);
    if (St()->redirectEip) {
        St()->resumeEip = St()->redirectEip;
        St()->resumeEsp = St()->redirectEsp;
        St()->redirectEip = 0;
    }
    if (St()->exiting) St()->resumeEip = (uint32_t)(uintptr_t)g_exit32;
    St()->eax = (uint32_t)r;
    St()->edx = (uint32_t)(r >> 32);
}

static uint32_t g_dispatch32, g_unhandledThunk;
static uint32_t MakeThunk32(uint32_t index);
// for the D3D9 bridge: a 64-bit function 32-bit code can call (stdcall, 'args' 32-bit arguments; at most 12), and
// memory below 4 GB
uint32_t BridgeFunction32(void* target, int args, const std::string& name) {
    Bridged b;
    b.name = name;
    b.target = target;
    b.args = args;
    b.cdecl_ = false;
    g_fn.push_back(b);
    return MakeThunk32((uint32_t)g_fn.size() - 1);
}
void* Alloc32(size_t size) { return (void*)(uintptr_t)B_HeapAlloc((uint64_t)LowHeap(), HEAP_ZERO_MEMORY, size); }
void Free32(void* p) {
    if (p) B_HeapFree((uint64_t)LowHeap(), 0, (uint64_t)(uintptr_t)p);
}
static uint32_t BuildDispatch32(uint8_t* c);
static uint64_t B_Unhandled32(uint64_t rec);
static uint32_t MakeThunk32(uint32_t index);
static uint8_t* g_bridge64 = nullptr;   // where thunks jump (64-bit code)
static uint8_t* g_enter64 = nullptr;    // Run32's way in: (entry32, esp32) -> runs until the program returns
static uint8_t* g_thunks = nullptr;
static size_t g_thunkUsed = 0;

static void BuildSwitchCode() {
    if (!TebAreaReady()) return;
    g_arena = AllocBelow4G(0x200000);
    if (!g_arena) return;
    uint8_t* c = g_arena;
    size_t n = 0;
    auto e = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) c[n++] = x; };
    auto q = [&](uint64_t v) { memcpy(c + n, &v, 8); n += 8; };
    auto d = [&](uint32_t v) { memcpy(c + n, &v, 4); n += 4; };
    // this thread's State: low 32 bits of the 64-bit TEB (gs:[0x30]) + 0x800
    auto stateToRax = [&] {
        e({0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00});   // mov rax, gs:[0x30]
        e({0x05}); d(g_tebShift);                                    // add eax, shift (zero-extends rax)
        e({0x48, 0x05, 0x00, 0x08, 0x00, 0x00});                      // add rax, 0x800
    };
    auto stateToRcx = [&] {
        e({0x65, 0x48, 0x8B, 0x0C, 0x25, 0x30, 0x00, 0x00, 0x00});   // mov rcx, gs:[0x30]
        e({0x81, 0xC1}); d(g_tebShift);                              // add ecx, shift (zero-extends rcx)
        e({0x48, 0x81, 0xC1, 0x00, 0x08, 0x00, 0x00});                // add rcx, 0x800
    };
    // --- enter: rcx = entry32, rdx = esp32 (the return address, g_exit32, already on that stack)
    g_enter64 = c + n;
    e({0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});   // push rbx rbp rsi rdi r12-r15
    e({0x48, 0x83, 0xEC, 0x08});                                                    // sub rsp, 8 (16-byte aligned for calls)
    stateToRax();
    e({0x48, 0x89, 0x20});                                                          // mov [rax].saved64, rsp
    // 32-bit code needs real data segments: 64-bit Windows may come back from a system call or an exception with a
    // null SS (fine for 64-bit code, a fault on the first push in 32-bit code)
    e({0x66, 0xB8, 0x2B, 0x00});                                                    // mov ax, 0x2B
    e({0x8E, 0xD0});                                                                // mov ss, ax
    e({0x8E, 0xD8});                                                                // mov ds, ax
    e({0x8E, 0xC0});                                                                // mov es, ax
    e({0x48, 0x89, 0xD4});                                                          // mov rsp, rdx (the 32-bit stack)
    e({0x6A, 0x23});                                                                // push 0x23
    e({0x51});                                                                      // push rcx (entry32)
    e({0x48, 0xCB});                                                                // retfq -> 32-bit entry point
    // --- back from the program (64-bit mode, from g_exit32)
    uint8_t* leave64 = c + n;
    stateToRcx();
    e({0x48, 0x8B, 0x21});                                                          // mov rsp, [rcx].saved64
    e({0x48, 0x83, 0xC4, 0x08});                                                    // add rsp, 8
    e({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B});   // pop r15-r12 rdi rsi rbp rbx
    e({0xC3});                                                                      // ret (eax: the program's return value)
    // --- bridge: 64-bit mode, eax = function index, esp = the 32-bit stack at the call
    g_bridge64 = c + n;
    stateToRcx();
    e({0x89, 0x41, 0x08});                                                          // mov [rcx].index, eax
    e({0x89, 0x61, 0x0C});                                                          // mov [rcx].esp32, esp
    e({0x48, 0x8B, 0x21});                                                          // mov rsp, [rcx].saved64
    e({0x48, 0x83, 0xEC, 0x20});                                                    // sub rsp, 32 (shadow space)
    e({0x48, 0xB8}); q((uint64_t)&Dispatch);                                        // mov rax, Dispatch
    e({0xFF, 0xD0});                                                                // call rax
    stateToRcx();
    // 32-bit code needs real data segments: 64-bit Windows may come back from a system call or an exception with a
    // null SS (fine for 64-bit code, a fault on the first push in 32-bit code)
    e({0x66, 0xB8, 0x2B, 0x00});                                                    // mov ax, 0x2B
    e({0x8E, 0xD0});                                                                // mov ss, ax
    e({0x8E, 0xD8});                                                                // mov ds, ax
    e({0x8E, 0xC0});                                                                // mov es, ax
    e({0x8B, 0x61, 0x14});                                                          // mov esp, [rcx].resumeEsp (zero-extends rsp)
    e({0x6A, 0x23});                                                                // push 0x23
    e({0x8B, 0x41, 0x10});                                                          // mov eax, [rcx].resumeEip
    e({0x50});                                                                      // push rax
    e({0x8B, 0x51, 0x1C});                                                          // mov edx, [rcx].edx
    e({0x8B, 0x41, 0x18});                                                          // mov eax, [rcx].eax
    e({0x48, 0xCB});                                                                // retfq -> back to the 32-bit caller
    // --- 32-bit code the program's entry point returns to: jmp far 0x33:leave64
    g_exit32 = c + n;
    e({0xEA}); d((uint32_t)(uintptr_t)leave64); e({0x33, 0x00});
    g_thunks = g_arena + 0x2000;   // thunks: 0x2000..0x40000; the 32-bit stack: up to 0x200000
    g_thunkUsed = 0;
    // the 32-bit exception dispatcher (exc32.inc) at 0x1000, and its way out when no handler takes an exception
    g_fn.push_back({"loader!Unhandled32", (void*)B_Unhandled32, 1, false});
    g_unhandledThunk = MakeThunk32((uint32_t)(g_fn.size() - 1));
    BuildDispatch32(g_arena + 0x1000);
    g_dispatch32 = (uint32_t)(uintptr_t)(g_arena + 0x1000);
}

static uint32_t MakeThunk32(uint32_t index) {
    // mov eax, index ; jmp far 0x33:bridge64
    uint8_t* t = g_thunks + g_thunkUsed;
    t[0] = 0xB8;
    memcpy(t + 1, &index, 4);
    t[5] = 0xEA;
    uint32_t to = (uint32_t)(uintptr_t)g_bridge64;
    memcpy(t + 6, &to, 4);
    t[10] = 0x33;
    t[11] = 0x00;
    g_thunkUsed += 16;
    return (uint32_t)(uintptr_t)t;
}

static std::string Lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

// 32-bit code finds its thread block through FS. A 64-bit Windows thread keeps FS's base only until its next thread
// switch (then it is 0, and nothing can sit at address 0); GS's base is always the thread's 64-bit TEB, and in 32-bit
// mode a GS access lands at the low 32 bits of it. So the 32-bit thread block lives there (MakeTeb32), and each
// instruction that reads or writes the thread block through FS gets the GS prefix instead (0x64 -> 0x65, same
// length). Matched: FS-prefixed instructions with an absolute address below 0x1000 (the thread block's fields), the
// forms compilers emit (mov eax,fs:[x] / mov fs:[x],eax, and opcodes with a modrm of [disp32]).
static int FsToGs(uint8_t* code, size_t size) {
    static const uint8_t modrmOps[] = {0x8B, 0x89, 0x8A, 0x88, 0xFF, 0x8F, 0xC7, 0xC6, 0x83, 0x81, 0x80, 0x3B, 0x39,
                                       0x3A, 0x38, 0x03, 0x01, 0x2B, 0x29, 0x33, 0x31, 0x0B, 0x09, 0x23, 0x21, 0x85,
                                       0x84, 0x8D, 0x87, 0xF7};
    int n = 0;
    for (size_t i = 0; i + 7 <= size; i++) {
        if (code[i] != 0x64) continue;
        uint8_t op = code[i + 1];
        uint32_t disp;
        if (op >= 0xA0 && op <= 0xA3) {   // mov al/eax <-> moffs32
            memcpy(&disp, code + i + 2, 4);
        } else {
            bool known = false;
            for (uint8_t m : modrmOps) known |= op == m;
            if (!known || (code[i + 2] & 0xC7) != 0x05) continue;   // modrm: [disp32]
            memcpy(&disp, code + i + 3, 4);
        }
        if (disp >= 0x1000) continue;
        code[i] = 0x65;
        disp += g_tebShift;
        memcpy(code + i + (op >= 0xA0 && op <= 0xA3 ? 2 : 3), &disp, 4);
        n++;
    }
    return n;
}

static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll);
// --- the program's own 32-bit DLLs (TeknoParrot games bring theirs: libavs, libacio, d3dx9_42, ...) -----------------
struct Mod32 {
    std::string name;
    LoadedImage32 img;
    bool started = false;
};
static std::deque<Mod32> g_dlls;
static std::wstring g_folder32;   // the program's folder, ends with a backslash
void SetProgramFolder32(const std::wstring& folder) {
    g_folder32 = folder;
    if (!g_folder32.empty() && g_folder32.back() != L'\\') g_folder32 += L'\\';
}
static std::vector<uint8_t> ReadFile32(const std::wstring& path) {
    std::vector<uint8_t> v;
    if (NetFsReadAll(path, v)) return v;   // (a game played from the NAS)
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
#else
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
#endif
    if (h == INVALID_HANDLE_VALUE) return v;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    v.resize((size_t)sz.QuadPart);
    DWORD n = 0;
    ReadFile(h, v.data(), (DWORD)v.size(), &n, nullptr);
    CloseHandle(h);
    if (n != v.size()) v.clear();
    return v;
}
// our 32-bit DLLs (the package's x86 folder: d3d8to9, DirectInput/DirectSound stand-ins): looked at FIRST, so a
// hook DLL an old loader kit left in a game's folder (dinput8.dll) does not replace them
static std::wstring g_ours32;
void SetOurFolder32(const std::wstring& folder) {
    g_ours32 = folder;
    if (!g_ours32.empty() && g_ours32.back() != L'\\') g_ours32 += L'\\';
}
static Mod32* LoadDll32(const std::string& dll) {
    for (auto& m : g_dlls)
        if (m.name == dll) return &m;
    std::wstring wname(dll.begin(), dll.end());
    std::vector<uint8_t> bytes;
    if (!g_ours32.empty()) bytes = ReadFile32(g_ours32 + wname);
    if (bytes.empty() && !g_folder32.empty()) bytes = ReadFile32(g_folder32 + wname);
    if (bytes.empty()) return nullptr;   // in neither folder: a system DLL (bridged)
    g_dlls.push_back({dll});
    Mod32& m = g_dlls.back();
    if (!MapImage32(bytes, m.img, dll, true)) {
        g_dlls.pop_back();
        return nullptr;
    }
    return &m;
}
// a function's address in a mapped 32-bit DLL, by name or ordinal; forwarders ("other.Function") are followed
static uint32_t Export32(const Mod32& m, const std::string& fn, uint32_t ordinal) {
    if (!m.img.exportDir) return 0;
    auto ed = (const IMAGE_EXPORT_DIRECTORY*)(uintptr_t)m.img.exportDir;
    uint8_t* base = (uint8_t*)(uintptr_t)m.img.base;
    auto funcs = (const uint32_t*)(base + ed->AddressOfFunctions);
    auto names = (const uint32_t*)(base + ed->AddressOfNames);
    auto ords = (const uint16_t*)(base + ed->AddressOfNameOrdinals);
    uint32_t index = 0xFFFFFFFF;
    if (fn.empty()) {
        index = ordinal - ed->Base;
    } else {
        for (uint32_t i = 0; i < ed->NumberOfNames; i++)
            if (!strcmp((const char*)(base + names[i]), fn.c_str())) {
                index = ords[i];
                break;
            }
    }
    if (index >= ed->NumberOfFunctions || !funcs[index]) return 0;
    uint32_t rva = funcs[index];
    if (base + rva >= (uint8_t*)(uintptr_t)m.img.exportDir && base + rva < (uint8_t*)(uintptr_t)m.img.exportDir + m.img.exportSize) {
        std::string fwd = (const char*)(base + rva);   // "OTHER.Function"
        size_t dot = fwd.find('.');
        std::string other = Lower(fwd.substr(0, dot)) + ".dll", name = fwd.substr(dot + 1);
        if (Mod32* o = LoadDll32(other)) return Export32(*o, name, 0);
        int args = 0;
        bool cd = false;
        void* t = ResolveFor32(other, name, args, cd);
        if (!t) return 0;
        g_fn.push_back({other + "!" + name, t, args, cd});
        return MakeThunk32((uint32_t)(g_fn.size() - 1));
    }
    return m.img.base + rva;
}

static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll) {
    auto dos = (const IMAGE_DOS_HEADER*)file.data();
    if (file.size() < 0x400 || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (const IMAGE_NT_HEADERS32*)(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return false;
    if (!g_arena) BuildSwitchCode();
    if (!g_arena) {
        Log("32-bit: no memory below 4 GB for the bridge");
        return false;
    }
    const auto& oh = nt->OptionalHeader;
    uint8_t* base = (uint8_t*)LowAlloc((void*)(uintptr_t)oh.ImageBase, oh.SizeOfImage);
    bool moved = false;
    if (!base) {
        base = AllocBelow4G(oh.SizeOfImage, 0x20000000);
        moved = true;
    }
    if (!base) {
        Log("32-bit: no room below 2 GB for %s", self.c_str());
        return false;
    }
    memcpy(base, file.data(), oh.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].SizeOfRawData && sec[i].PointerToRawData + sec[i].SizeOfRawData <= file.size())
            memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData,
                   std::min<size_t>(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    int32_t delta = (int32_t)((uint32_t)(uintptr_t)base - oh.ImageBase);
    const auto& rd = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && rd.Size) {
        uint8_t* p = base + rd.VirtualAddress, *end = p + rd.Size;
        while (p < end) {
            auto blk = (IMAGE_BASE_RELOCATION*)p;
            if (!blk->SizeOfBlock) break;
            auto ent = (uint16_t*)(blk + 1);
            for (unsigned k = 0; k < (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2; k++)
                if ((ent[k] >> 12) == IMAGE_REL_BASED_HIGHLOW) *(uint32_t*)(base + blk->VirtualAddress + (ent[k] & 0xFFF)) += delta;
            p += blk->SizeOfBlock;
        }
    } else if (delta) {
        Log("32-bit: the program must sit at 0x%x and has no relocations", oh.ImageBase);
        return false;
    }
    int bridged = 0, missing = 0;
    const auto& id = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (id.Size) {
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + id.VirtualAddress); imp->Name; imp++) {
            std::string dll = Lower((const char*)(base + imp->Name));
            auto thunk = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
            auto names = (IMAGE_THUNK_DATA32*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            for (; names->u1.AddressOfData; names++, thunk++) {
                bool byOrd = IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal);
                std::string fn = byOrd ? "#" + std::to_string(IMAGE_ORDINAL32(names->u1.Ordinal))
                                       : (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
                // the program's own DLLs (its folder): mapped too, the function's own address
                if (Mod32* m = LoadDll32(dll)) {
                    if (dll == "d3dx9_42.dll" && fn == "D3DXCreateEffectFromFileA") {   // (testing) its result logged
                        g_realCreateEffect = Export32(*m, fn, 0);
                        g_fn.push_back({dll + "!" + fn + " (watched)", (void*)Spy_CreateEffect, 8, false});
                        thunk->u1.Function = MakeThunk32((uint32_t)(g_fn.size() - 1));
                        bridged++;
                        continue;
                    }
                    {   // started before the DLL that imports it (StartDlls32)
                        auto& d = g_deps32[self];
                        if (std::find(d.begin(), d.end(), dll) == d.end()) d.push_back(dll);
                    }
                    uint32_t at = Export32(*m, byOrd ? "" : fn, byOrd ? IMAGE_ORDINAL32(names->u1.Ordinal) : 0);
                    if (at) {
                        thunk->u1.Function = at;
                        bridged++;
                        continue;
                    }
                }
                Bridged b{dll + "!" + fn, nullptr, 0, true};
                b.target = ResolveFor32(dll, fn, b.args, b.cdecl_);   // ("#n" by ordinal: Winsock knows its numbers)
                (b.target ? bridged : missing)++;
                if (!b.target) out.missing.push_back(b.name);
                g_fn.push_back(b);
                thunk->u1.Function = MakeThunk32((uint32_t)(g_fn.size() - 1));
            }
        }
    }
    int fs = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            fs += FsToGs(base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
    if (fs) Log("32-bit %s: %d thread block accesses moved from FS to GS", self.c_str(), fs);
    LowProtect(g_arena, 0x40000, PAGE_EXECUTE_READWRITE);   // switch code + thunks; State is written at run time
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD ch = sec[i].Characteristics;
        bool x = ch & IMAGE_SCN_MEM_EXECUTE, w = ch & IMAGE_SCN_MEM_WRITE;
        size_t vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        LowProtect(base + sec[i].VirtualAddress, vs, x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (w ? PAGE_READWRITE : PAGE_READONLY));
    }
    FlushInstructionCache(GetCurrentProcess(), base, oh.SizeOfImage);
    out.base = (uint32_t)(uintptr_t)base;
    out.size = oh.SizeOfImage;
    out.entry = out.base + oh.AddressOfEntryPoint;
    if (oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        out.tlsDir = out.base + oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress;
        if (isDll) AddDllTls32(out.tlsDir);
    }
    out.exportDir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size ? out.base + oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress : 0;
    out.exportSize = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    Log("32-bit %s %s loaded at 0x%08x%s, %u bytes, entry +0x%x; imports: %d bridged, %d missing", isDll ? "DLL" : "program",
        self.c_str(), out.base, moved ? " (moved)" : "", out.size, oh.AddressOfEntryPoint, bridged, missing);
    for (auto& m : out.missing) Log("  missing: %s", m.c_str());
    return true;
}

bool MapProgram32(const std::vector<uint8_t>& file, LoadedImage32& out) { return MapImage32(file, out, "program", false); }

// a 32-bit thread block where this thread's FS points in 32-bit mode
// the 32-bit process block (PEB): 32-bit code reads it straight from FS:[0x30]
static uint8_t* g_peb32 = nullptr;
static uint8_t* MakePeb32() {
    if (g_peb32) return g_peb32;
    g_peb32 = (uint8_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 0x1000);
    if (!g_peb32) return nullptr;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    OSVERSIONINFOW ov = {sizeof ov};
    typedef LONG(NTAPI * ver_t)(OSVERSIONINFOW*);
    if (auto rv = (ver_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) rv(&ov);
    *(uint32_t*)(g_peb32 + 0x08) = g_img32->base;                          // ImageBaseAddress
    *(uint32_t*)(g_peb32 + 0x18) = (uint32_t)(uintptr_t)LowHeap();         // ProcessHeap
    *(uint32_t*)(g_peb32 + 0x64) = si.dwNumberOfProcessors;                // NumberOfProcessors
    *(uint32_t*)(g_peb32 + 0xA4) = ov.dwMajorVersion;                      // OSMajorVersion
    *(uint32_t*)(g_peb32 + 0xA8) = ov.dwMinorVersion;                      // OSMinorVersion
    *(uint16_t*)(g_peb32 + 0xAC) = (uint16_t)ov.dwBuildNumber;             // OSBuildNumber
    *(uint32_t*)(g_peb32 + 0xB0) = 2;                                      // OSPlatformId (NT)
    *(uint32_t*)(g_peb32 + 0x88) = 1;                                      // NumberOfHeaps
    // RTL_USER_PROCESS_PARAMETERS (32-bit layout): flags, the image path and the command line as UNICODE_STRINGs
    uint8_t* pp = (uint8_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 0x400);
    if (pp) {
        *(uint32_t*)(pp + 0x00) = 0x2A0;   // MaximumLength
        *(uint32_t*)(pp + 0x04) = 0x2A0;   // Length
        *(uint32_t*)(pp + 0x08) = 1;       // Flags: normalized
        auto ustr = [&](size_t at, const wchar_t* w) {
            size_t n = wcslen(w);
            wchar_t* low = (wchar_t*)HeapAlloc(LowHeap(), 0, (n + 1) * 2);
            memcpy(low, w, (n + 1) * 2);
            *(uint16_t*)(pp + at) = (uint16_t)(n * 2);
            *(uint16_t*)(pp + at + 2) = (uint16_t)(n * 2 + 2);
            *(uint32_t*)(pp + at + 4) = (uint32_t)(uintptr_t)low;
        };
        ustr(0x38, GetCommandLineW());   // ImagePathName (the command line will do until the real path is kept)
        ustr(0x40, GetCommandLineW());   // CommandLine
        *(uint32_t*)(g_peb32 + 0x10) = (uint32_t)(uintptr_t)pp;   // ProcessParameters
    }
    return g_peb32;
}
// the program's thread-local data (__declspec(thread), C++ magic statics): one block copied from its template,
// its address in an array at TEB+0x2C (ThreadLocalStoragePointer), index 0 written to the program's _tls_index
// a module's block, copied from its template
static uint32_t TlsBlock32(uint32_t dir) {
    auto d = (const IMAGE_TLS_DIRECTORY32*)(uintptr_t)dir;
    uint32_t tmpl = d->StartAddressOfRawData, len = d->EndAddressOfRawData - d->StartAddressOfRawData;
    uint8_t* block = (uint8_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, len + d->SizeOfZeroFill + 16);
    if (block && len) memcpy(block, (const void*)(uintptr_t)tmpl, len);
    return (uint32_t)(uintptr_t)block;
}
// the DLLs' thread-local data too (slot 1 and up, in load order): MSVC's thread-safe statics use it (dxwrapper
// crashed in its first one)
static std::vector<uint32_t> g_tlsDlls;   // slot i + 1 -> its TLS directory
static uint32_t MakeTls32() {
    uint32_t* arr = (uint32_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 64 * 4);
    if (!arr) return 0;
    if (g_img32->tlsDir) {
        auto d = (const IMAGE_TLS_DIRECTORY32*)(uintptr_t)g_img32->tlsDir;
        arr[0] = TlsBlock32(g_img32->tlsDir);
        if (d->AddressOfIndex) *(uint32_t*)(uintptr_t)d->AddressOfIndex = 0;
        if (d->AddressOfCallBacks && *(const uint32_t*)(uintptr_t)d->AddressOfCallBacks)
            Log("32-bit: the program has TLS callbacks (not run yet)");
    }
    for (size_t i = 0; i < g_tlsDlls.size() && i + 1 < 64; i++) arr[i + 1] = TlsBlock32(g_tlsDlls[i]);
    return (uint32_t)(uintptr_t)arr;
}
// a DLL with thread-local data: its slot; the loading thread (already running 32-bit code) gets its block now
static void AddDllTls32(uint32_t dir) {
    auto d = (const IMAGE_TLS_DIRECTORY32*)(uintptr_t)dir;
    g_tlsDlls.push_back(dir);
    uint32_t slot = (uint32_t)g_tlsDlls.size();
    if (d->AddressOfIndex) *(uint32_t*)(uintptr_t)d->AddressOfIndex = slot;
    if (HasState32() && St()->teb32) {
        uint32_t arr = *(uint32_t*)((uint8_t*)(uintptr_t)St()->teb32 + 0x2C);
        if (arr && slot < 64) ((uint32_t*)(uintptr_t)arr)[slot] = TlsBlock32(dir);
    }
    Log("32-bit: a DLL's thread-local data in slot %u", slot);
}

static uint8_t* MakeTeb32(uint32_t stackTop, uint32_t stackBottom) {
    // where a GS access lands in 32-bit mode: the low 32 bits of this thread's 64-bit TEB (see FsToGs)
    uint8_t* want = Teb32Of();
    if (want < g_tebArea || want + 0x1000 > g_tebArea + kTebArea) {
        Log("32-bit: thread block %p outside the area (TEB %p)", want, NtCurrentTeb());
        return nullptr;
    }
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    uint8_t* t = (uint8_t*)VirtualAllocFromApp(want, 0x1000, MEM_COMMIT, PAGE_READWRITE);
#else
    uint8_t* t = (uint8_t*)VirtualAlloc(want, 0x1000, MEM_COMMIT, PAGE_READWRITE);
#endif
    if (t != want) {
        Log("32-bit: thread block %p not committed (error %lu)", want, GetLastError());
        return nullptr;
    }
#if 0   // (granule placement, before the thread block area)
    // the spot must be free, or reserved by an earlier thread block of ours (never memory in use: the main thread's
    // stack, the heap, a program image)
    // whole 64 KB granules, reserved and committed at once and remembered: threads' blocks sit close together, so the
    // next thread's page is often in a granule an earlier thread took
    static std::mutex lock;
    static std::set<uintptr_t> granules;
    uint8_t* t = nullptr;
    {
        std::lock_guard<std::mutex> l(lock);
        uintptr_t g = (uintptr_t)want & ~0xFFFFull;
        if (granules.count(g)) {
            t = want;
        } else {
            MEMORY_BASIC_INFORMATION mi = {};
            VirtualQuery((void*)g, &mi, sizeof mi);
            if (mi.State == MEM_FREE && (uintptr_t)mi.BaseAddress + mi.RegionSize >= g + 0x10000) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
                void* r = VirtualAllocFromApp((void*)g, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
                void* r = VirtualAlloc((void*)g, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
                if (r == (void*)g) {
                    granules.insert(g);
                    t = want;
                }
            }
            if (!t) {
                Log("32-bit: thread block at %p: its 64 KB granule is not free (state 0x%lx, error %lu; this thread's 64-bit TEB %p)",
                    want, mi.State, GetLastError(), NtCurrentTeb());
                return nullptr;
            }
        }
    }
#endif
    memset(t, 0, 0x1000);
    *(uint32_t*)(t + 0x00) = 0xFFFFFFFF;                  // NtTib.ExceptionList: empty
    *(uint32_t*)(t + 0x04) = stackTop;                    // NtTib.StackBase
    *(uint32_t*)(t + 0x08) = stackBottom;                 // NtTib.StackLimit
    *(uint32_t*)(t + 0x18) = (uint32_t)(uintptr_t)t;      // NtTib.Self
    *(uint32_t*)(t + 0x20) = GetCurrentProcessId();       // ClientId
    *(uint32_t*)(t + 0x24) = GetCurrentThreadId();
    *(uint32_t*)(t + 0x2C) = MakeTls32();                     // ThreadLocalStoragePointer
    *(uint32_t*)(t + 0x30) = (uint32_t)(uintptr_t)MakePeb32(); // ProcessEnvironmentBlock
    return t;
}

static LONG CALLBACK Watch32(PEXCEPTION_POINTERS e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;
    Log("32-bit run: exception 0x%08lx at %p (cs %04x, esp %08llx, last function %s)", code, e->ExceptionRecord->ExceptionAddress,
        e->ContextRecord->SegCs, e->ContextRecord->Rsp & 0xFFFFFFFF,
        HasState32() && St()->index < g_fn.size() ? g_fn[St()->index].name.c_str() : "-");
    if (e->ContextRecord->SegCs != 0x23 && code == 0xC0000005) {   // in 64-bit code: which module, which access
        void* at = e->ExceptionRecord->ExceptionAddress;
        const char* mn = MappedImageName(at);
        uint8_t* mb = (uint8_t*)MappedImageBase(at);
        Log("  in %s+0x%llx, %s %p, rcx %p rdx %p r8 %p", mn ? mn : "?", mb ? (unsigned long long)((uint8_t*)at - mb) : 0ull,
            e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", (void*)e->ExceptionRecord->ExceptionInformation[1],
            (void*)e->ContextRecord->Rcx, (void*)e->ContextRecord->Rdx, (void*)e->ContextRecord->R8);
    }
    if (e->ContextRecord->SegCs == 0x23) {   // 32-bit code: the top of its stack (a return address after a bad call)
        const uint32_t* sp = (const uint32_t*)(uintptr_t)(uint32_t)e->ContextRecord->Rsp;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
            Log("  32-bit stack: %08x %08x %08x %08x, eax %08x ecx %08x edx %08x", sp[0], sp[1], sp[2], sp[3],
                (uint32_t)e->ContextRecord->Rax, (uint32_t)e->ContextRecord->Rcx, (uint32_t)e->ContextRecord->Rdx);
        if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {   // text on the stack (a fatal message)
            const char* t = (const char*)sp;
            size_t room = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - t);
            std::string txt;
            for (size_t i = 0; i < std::min<size_t>(room, 400); i++) txt += (t[i] >= 32 && t[i] < 127) ? t[i] : (t[i] ? '.' : ' ');
            Log("  32-bit stack text: %s", txt.c_str());
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// --- 64-bit code calling a 32-bit function (DllMain now; window procedures, threads and callbacks next): the
// arguments go on the 32-bit stack below where the 32-bit code is (or at its top), the return address is g_exit32;
// the bridge state is kept and put back, so it nests inside a bridged call
uint32_t Call32(uint32_t func, const uint32_t* args, int n) {
    State keep = *St();
    // nested: well below the caller's stack (4 KB: room for an exception record and context there)
    uint32_t esp = keep.esp32 ? (keep.esp32 - 0x1000) & ~15u : keep.stackTop;
    esp -= 4 * n;
    for (int i = 0; i < n; i++) ((uint32_t*)(uintptr_t)esp)[i] = args[i];
    esp -= 4;
    *(uint32_t*)(uintptr_t)esp = (uint32_t)(uintptr_t)g_exit32;
    St()->exiting = 0;
    typedef uint32_t (*enter_t)(uint64_t entry, uint64_t esp);
    uint32_t r = ((enter_t)g_enter64)(func, esp);
    if (St()->exiting) r = (uint32_t)St()->exitCode;
    *St() = keep;
    return r;
}
#include "exc32.inc"


// --- winmm timers for 32-bit programs: timeSetEvent's callback runs on a thread of its own with a 32-bit stack,
// every <delay> ms (TIME_PERIODIC) or once; TIME_CALLBACK_EVENT_SET / _PULSE set an event instead
struct Timer32 {
    uint32_t id, delay, proc, user, flags;
    volatile bool stop = false;
};
static std::mutex g_timerLock;
static std::map<uint32_t, Timer32*> g_timers;
static uint32_t g_timerNext = 1;
static DWORD WINAPI Timer32Main(void* p) {
    Timer32* t = (Timer32*)p;
    uint8_t* stack = AllocBelow4G(256 << 10, 0x10000000);
    uint8_t* teb = stack ? MakeTeb32((uint32_t)(uintptr_t)(stack + (256 << 10) - 16), (uint32_t)(uintptr_t)stack) : nullptr;
    if (!teb) return 0;
    St()->teb32 = (uint64_t)teb;
    St()->stackTop = (uint32_t)(uintptr_t)(stack + (256 << 10) - 16);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    do {
        next.QuadPart += freq.QuadPart * t->delay / 1000;
        for (;;) {
            QueryPerformanceCounter(&now);
            int64_t left = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
            if (left <= 0 || t->stop) break;
            Sleep(left > 2 ? (DWORD)left - 1 : 0);
        }
        if (t->stop) break;
        if (t->flags & 0x30) {   // TIME_CALLBACK_EVENT_SET (0x10) / _PULSE (0x20)
            HANDLE e = (HANDLE)(intptr_t)(int32_t)t->proc;
            SetEvent(e);   // (PulseEvent: not on the console)
        } else {
            uint32_t a[5] = {t->id, 0, t->user, 0, 0};
            Call32(t->proc, a, 5);
        }
    } while (t->flags & 1);   // TIME_PERIODIC
    return 0;
}
static uint64_t B_timeSetEvent(uint64_t delay, uint64_t, uint64_t proc, uint64_t user, uint64_t flags) {
    auto t = new Timer32{0, (uint32_t)delay ? (uint32_t)delay : 1, (uint32_t)proc, (uint32_t)user, (uint32_t)flags};
    {
        std::lock_guard<std::mutex> l(g_timerLock);
        t->id = g_timerNext++;
        g_timers[t->id] = t;
    }
    HANDLE h = CreateThread(nullptr, 0, Timer32Main, t, 0, nullptr);
    if (!h) return 0;
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(h);
    static int told = 0;
    if (told++ < 4) Log("32-bit timer %u: every %u ms%s", t->id, t->delay, (flags & 1) ? "" : " (once)");
    return t->id;
}
static uint64_t B_timeKillEvent(uint64_t id) {
    std::lock_guard<std::mutex> l(g_timerLock);
    auto it = g_timers.find((uint32_t)id);
    if (it == g_timers.end()) return 97;   // MMSYSERR_INVALPARAM
    it->second->stop = true;   // (the thread ends by itself; the record stays, it is small)
    g_timers.erase(it);
    return 0;
}

// --- mmio for 32-bit programs: RIFF (wave) files read whole into memory below 2 GB; buffered access (GetInfo /
// Advance / SetInfo) points straight into that copy
struct Mmio32 {
    uint8_t* data = nullptr;   // below 2 GB
    uint32_t size = 0, pos = 0;
    bool own = false;
};
static std::mutex g_mmioLock;
static std::map<uint32_t, Mmio32> g_mmio;
static uint32_t g_mmioNext = 0x7F000000;
static Mmio32* MmioGet(uint64_t h) {
    auto it = g_mmio.find((uint32_t)h);
    return it == g_mmio.end() ? nullptr : &it->second;
}
#define M32(p) ((uint8_t*)(uintptr_t)(uint32_t)(p))
static uint64_t B_mmioOpenA(uint64_t name, uint64_t info, uint64_t flags) {
    Mmio32 m;
    uint8_t* mi = info ? M32(info) : nullptr;
    if (!name && mi && *(uint32_t*)(mi + 4) == 0x204D454D) {   // FOURCC_MEM: a file in the game's memory
        m.data = M32(*(uint32_t*)(mi + 24));
        m.size = *(uint32_t*)(mi + 20);
    } else if (name) {
        std::string n = (const char*)M32(name);
        std::wstring w(n.begin(), n.end());
        std::vector<uint8_t> all = ReadFile32(w);
        if (all.empty()) {
            if (mi) *(uint32_t*)(mi + 12) = 257;   // MMIOERR_FILENOTFOUND
            return 0;
        }
        m.data = (uint8_t*)Alloc32(all.size());
        if (!m.data) return 0;
        memcpy(m.data, all.data(), all.size());
        m.size = (uint32_t)all.size();
        m.own = true;
    } else {
        return 0;
    }
    std::lock_guard<std::mutex> l(g_mmioLock);
    g_mmioNext += 4;
    g_mmio[g_mmioNext] = m;
    if (mi) *(uint32_t*)(mi + 68) = g_mmioNext;
    return g_mmioNext;
}
static uint64_t B_mmioClose(uint64_t h, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    g_mmio.erase((uint32_t)h);   // (the copy stays in the low heap; wave files are small)
    return 0;
}
static uint64_t B_mmioRead(uint64_t h, uint64_t buf, uint64_t n) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m) return (uint64_t)-1;
    uint32_t k = std::min<uint32_t>((uint32_t)n, m->size - std::min(m->pos, m->size));
    memcpy(M32(buf), m->data + m->pos, k);
    m->pos += k;
    return k;
}
static uint64_t B_mmioWrite(uint64_t, uint64_t, uint64_t) { return (uint64_t)-1; }
static uint64_t B_mmioSeek(uint64_t h, uint64_t off, uint64_t origin) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m) return (uint64_t)-1;
    int64_t base = origin == 1 ? m->pos : origin == 2 ? m->size : 0;
    int64_t at = base + (int32_t)off;
    if (at < 0) return (uint64_t)-1;
    m->pos = (uint32_t)at;
    return m->pos;
}
static uint32_t Rd32(Mmio32* m, uint32_t at) { return at + 4 <= m->size ? *(uint32_t*)(m->data + at) : 0; }
// MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags
static uint64_t B_mmioDescend(uint64_t h, uint64_t ckP, uint64_t parentP, uint64_t flags) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !ckP) return 265;   // MMIOERR_CHUNKNOTFOUND
    uint32_t* ck = (uint32_t*)M32(ckP);
    uint32_t end = m->size;
    if (parentP) {
        uint32_t* par = (uint32_t*)M32(parentP);
        end = std::min(end, par[3] + par[1]);
    }
    uint32_t wantId = ck[0], wantType = ck[2];
    for (uint32_t at = m->pos; at + 8 <= end;) {
        uint32_t id = Rd32(m, at), size = Rd32(m, at + 4);
        bool list = id == 0x46464952 || id == 0x5453494C;   // "RIFF" / "LIST"
        uint32_t type = list ? Rd32(m, at + 8) : 0;
        bool hit = (flags & 0x20) ? (id == 0x46464952 && type == wantType)
                 : (flags & 0x40) ? (id == 0x5453494C && type == wantType)
                 : (flags & 0x10) ? id == wantId
                                  : true;
        if (hit) {
            ck[0] = id, ck[1] = size, ck[2] = type, ck[3] = at + 8, ck[4] = 0;
            m->pos = at + 8 + (list ? 4 : 0);
            return 0;
        }
        at += 8 + size + (size & 1);
    }
    return 265;
}
static uint64_t B_mmioAscend(uint64_t h, uint64_t ckP, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !ckP) return 265;
    uint32_t* ck = (uint32_t*)M32(ckP);
    m->pos = ck[3] + ck[1] + (ck[1] & 1);
    return 0;
}
// MMIOINFO (72 bytes): cchBuffer +20, pchBuffer +24, pchNext +28, pchEndRead +32, pchEndWrite +36, lBufOffset +40,
// lDiskOffset +44, hmmio +68
static uint64_t B_mmioGetInfo(uint64_t h, uint64_t info, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !info) return 258;   // MMIOERR_INVALIDFILE
    uint8_t* mi = M32(info);
    memset(mi, 0, 72);
    uint32_t base = (uint32_t)(uintptr_t)m->data;
    *(uint32_t*)(mi + 20) = m->size;
    *(uint32_t*)(mi + 24) = base;
    *(uint32_t*)(mi + 28) = base + std::min(m->pos, m->size);
    *(uint32_t*)(mi + 32) = base + m->size;
    *(uint32_t*)(mi + 36) = base + m->size;
    *(uint32_t*)(mi + 44) = m->size;
    *(uint32_t*)(mi + 68) = (uint32_t)h;
    return 0;
}
static uint64_t B_mmioSetInfo(uint64_t h, uint64_t info, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !info) return 258;
    uint32_t next = *(uint32_t*)(M32(info) + 28);
    m->pos = next - (uint32_t)(uintptr_t)m->data;
    return 0;
}
static uint64_t B_mmioAdvance(uint64_t h, uint64_t info, uint64_t) {   // the whole file is the buffer: nothing more
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !info) return 258;
    uint8_t* mi = M32(info);
    uint32_t base = (uint32_t)(uintptr_t)m->data;
    m->pos = std::min(*(uint32_t*)(mi + 28) - base, m->size);
    *(uint32_t*)(mi + 32) = base + m->size;
    return 0;
}
static uint64_t B_mmioCreateChunk(uint64_t, uint64_t, uint64_t) { return 262; }   // MMIOERR_CANNOTWRITE

// --- threads a 32-bit program starts: a real thread, its own 32-bit stack and thread block, the start function
// called through Call32 with the parameter
struct Start32 {
    uint32_t func, param;
};
// --- the sampler (testing): each 32-bit thread's place every 5 s, with a few return addresses from its stack
static std::mutex g_threadsLock;
static std::vector<std::pair<DWORD, HANDLE>> g_threads32;
static std::map<DWORD, void*> g_threadState32;   // each thread's State (its saved 32-bit esp while in the loader)
static void NoteThread32() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard<std::mutex> l(g_threadsLock);
    g_threads32.push_back({GetCurrentThreadId(), h});
    g_threadState32[GetCurrentThreadId()] = St();
}
bool g_sample32 = false;
static DWORD WINAPI Sampler32(void*) {
    for (int round = 0; round < 60; round++) {
        Sleep(5000);
        std::vector<std::pair<DWORD, HANDLE>> list;
        {
            std::lock_guard<std::mutex> l(g_threadsLock);
            list = g_threads32;
        }
        for (auto& [tid, h] : list) {
            if (SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &c)) {
                char more[160] = "";
                uint32_t esp32 = (uint32_t)c.Rsp;
                if (c.SegCs != 0x23) {   // in the loader: where the game called from (its saved 32-bit stack)
                    std::lock_guard<std::mutex> l(g_threadsLock);
                    auto it = g_threadState32.find(tid);
                    esp32 = it != g_threadState32.end() ? ((State*)it->second)->esp32 : 0;
                    if (it != g_threadState32.end())
                        sprintf(more, " [%s]", ((State*)it->second)->index < g_fn.size() ? g_fn[((State*)it->second)->index].name.c_str() : "?");
                }
                if (esp32) {
                    const uint32_t* sp = (const uint32_t*)(uintptr_t)esp32;
                    MEMORY_BASIC_INFORMATION mbi;
                    int n = 0;
                    if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
                        for (int i = 0; i < 64 && n < 6 && (const uint8_t*)(sp + i) < (const uint8_t*)mbi.BaseAddress + mbi.RegionSize; i++)
                            if (sp[i] >= 0x401000 && sp[i] < 0x1000000) n += sprintf(more + strlen(more), " %08x", sp[i]) ? 1 : 0;
                }
                Log("sample %d: thread %lu %s eip %08llx esp %08llx eax %08llx ecx %08llx |%s", round, tid,
                    c.SegCs == 0x23 ? "32-bit" : "64-bit", c.Rip, c.Rsp & 0xFFFFFFFF, c.Rax & 0xFFFFFFFF, c.Rcx & 0xFFFFFFFF, more);
            }
            ResumeThread(h);
        }
    }
    return 0;
}
void StartSampler32() {
    static bool started = false;
    if (started || !g_sample32) return;
    started = true;
    CloseHandle(CreateThread(nullptr, 0, Sampler32, nullptr, 0, nullptr));
    Log("sampler on (sample.txt): thread positions every 5 s");
}
static DWORD WINAPI Thread32Main(void* p) {
    Start32 st = *(Start32*)p;
    delete (Start32*)p;
    uint8_t* stack = AllocBelow4G(1 << 20, 0x10000000);
    if (!stack) {
        Log("32-bit thread: no room below 2 GB for its stack (error %lu), not started", GetLastError());
        return 0;
    }
    uint32_t top = (uint32_t)(uintptr_t)(stack + (1 << 20) - 16);
    uint8_t* teb = MakeTeb32(top, (uint32_t)(uintptr_t)stack);
    if (!teb) {
        Log("32-bit thread: no thread block (error %lu), not started", GetLastError());
        VirtualFree(stack, 0, MEM_RELEASE);
        return 0;
    }
    St()->teb32 = (uint64_t)teb;
    St()->stackTop = top;
    Log("32-bit thread %lu: thread block %p, state %p, stack top %08x", GetCurrentThreadId(), teb, St(), top);
    NoteThread32();
    uint32_t r = Call32(st.func, &st.param, 1);
    Log("32-bit thread %lu ended (%u)", GetCurrentThreadId(), r);
    VirtualFree(stack, 0, MEM_RELEASE);
    return r;
}
static uint64_t B_CreateThread(uint64_t, uint64_t, uint64_t start, uint64_t param, uint64_t flags, uint64_t tid32) {
    auto st = new Start32{(uint32_t)start, (uint32_t)param};
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, Thread32Main, st, (DWORD)flags & CREATE_SUSPENDED, &tid);
    if (!h) {
        delete st;
        return 0;
    }
    if (tid32) *(uint32_t*)(uintptr_t)tid32 = tid;
    Log("32-bit program starts a thread at 0x%08x (thread %lu)", (uint32_t)start, tid);
    return (uint64_t)h;
}

static Mod32* FindDllByName32(const std::string& dll) {
    for (auto& m : g_dlls)
        if (m.name == dll) return &m;
    return nullptr;
}
static bool InMappedImage32(uint64_t at) {
    if (g_img32 && at >= g_img32->base && at < (uint64_t)g_img32->base + g_img32->size) return true;
    for (auto& m : g_dlls)
        if (at >= m.img.base && at < (uint64_t)m.img.base + m.img.size) return true;
    return false;
}
static Mod32* FindDll32(uint64_t base) {
    for (auto& m : g_dlls)
        if (m.img.base == (uint32_t)base && base) return &m;
    return nullptr;
}
static uint32_t ExportOf32(Mod32* m, const char* name) { return Export32(*m, name, 0); }
static uint32_t BaseOf32(Mod32* m) { return m->img.base; }
static const char* NameOf32(Mod32* m) { return m->name.c_str(); }
// each DLL after the DLLs it imports from (the C runtime before the DLLs that call malloc in their DllMain)
static void StartDll32(Mod32& m) {
    if (m.started) return;
    m.started = true;
    auto d = g_deps32.find(m.name);
    if (d != g_deps32.end())
        for (const std::string& dep : d->second)
            for (auto& o : g_dlls)
                if (o.name == dep) StartDll32(o);
    if (!m.img.entry || m.img.entry == m.img.base) return;
    uint32_t a[3] = {m.img.base, 1 /*DLL_PROCESS_ATTACH*/, 0};
    DWORD was = g_dllStartTid;
    g_dllStartTid = GetCurrentThreadId(), g_dllStartCalls = 0;
    uint32_t ok = Call32(m.img.entry, a, 3);
    g_dllStartTid = was;
    Log("32-bit DLL %s started: %s", m.name.c_str(), ok ? "yes" : "it said no");
}
static void StartDlls32() {
    for (auto& m : g_dlls) StartDll32(m);
}

uint32_t MainImageBase32() { return g_img32 ? g_img32->base : 0x400000; }
// GetModuleFileName for 32-bit programs: the program (null or its own base 0x400000) and the DLLs mapped from its
// folder; Caladrius builds its folder from it (an empty answer made it open "\SILVER.INI")
const std::wstring& ProgramPathW();
static std::wstring ModulePath32(uint64_t m) {
    if (!(uint32_t)m || (uint32_t)m == MainImageBase32()) return ProgramPathW();
    if (Mod32* d = FindDll32(m)) {
        std::wstring n(d->name.begin(), d->name.end());
        std::vector<uint8_t> probe;
        return g_folder32 + n;
    }
    return L"";
}
static uint64_t B_GetModuleFileNameW(uint64_t m, uint64_t out, uint64_t size) {
    std::wstring p = ModulePath32(m);
    if (p.empty()) return GetModuleFileNameW((HMODULE)(uintptr_t)(uint32_t)m, (LPWSTR)(uintptr_t)(uint32_t)out, (DWORD)size);
    DWORD n = (DWORD)std::min<size_t>(p.size(), size ? (size_t)size - 1 : 0);
    wchar_t* o = (wchar_t*)(uintptr_t)(uint32_t)out;
    if (size) memcpy(o, p.c_str(), n * 2), o[n] = 0;
    return n;
}
static uint64_t B_GetModuleFileNameA(uint64_t m, uint64_t out, uint64_t size) {
    std::wstring p = ModulePath32(m);
    if (p.empty()) return GetModuleFileNameA((HMODULE)(uintptr_t)(uint32_t)m, (LPSTR)(uintptr_t)(uint32_t)out, (DWORD)size);
    std::string a;
    for (wchar_t c : p) a += (char)c;
    DWORD n = (DWORD)std::min<size_t>(a.size(), size ? (size_t)size - 1 : 0);
    char* o = (char*)(uintptr_t)(uint32_t)out;
    if (size) memcpy(o, a.c_str(), n), o[n] = 0;
    return n;
}
// psapi GetModuleInformation for 32-bit programs: MODULEINFO { base, size, entry } of the program or one of its DLLs
static uint64_t B_GetModuleInformation(uint64_t, uint64_t m, uint64_t out, uint64_t cb) {
    if (!out || (uint32_t)cb < 12) return 0;
    uint32_t* mi = (uint32_t*)(uintptr_t)(uint32_t)out;
    uint32_t base = (uint32_t)m ? (uint32_t)m : MainImageBase32();
    const uint8_t* b = (const uint8_t*)(uintptr_t)base;
    if (!base || b[0] != 'M' || b[1] != 'Z') { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    auto nt = (const IMAGE_NT_HEADERS32*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    mi[0] = base;
    mi[1] = nt->OptionalHeader.SizeOfImage;
    mi[2] = base + nt->OptionalHeader.AddressOfEntryPoint;
    return 1;
}
// a 64-bit function the 32-bit program can call (a patch jumps to it): its thunk's address
uint32_t BridgeThunk32(const char* name, void* fn, int args, bool cdecl_) {
    g_fn.push_back({name, fn, args, cdecl_});
    return MakeThunk32((uint32_t)(g_fn.size() - 1));
}
bool g_running32 = false;   // a 32-bit program runs (its structures have 32-bit layouts: arcade_io.cpp)
int Run32(const LoadedImage32& img) {
    g_img32 = &img;
    g_running32 = true;
    static bool watching = false;
    if (!watching) {
        typedef PVOID(WINAPI * add_t)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        auto add = (add_t)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "AddVectoredExceptionHandler");
        watching = add && add(1, Watch32);
    }
    uint32_t stackTop = (uint32_t)(uintptr_t)(g_arena + 0x200000 - 16), stackBottom = (uint32_t)(uintptr_t)(g_arena + 0x40000);
    NoteThread32();
    StartSampler32();
    uint8_t* teb = MakeTeb32(stackTop, stackBottom);
    if (!teb) {
        Log("32-bit: no thread block");
        return -1;
    }
    // the 32-bit stack: the entry point's return address (back to us)
    uint32_t esp = stackTop - 4;
    *(uint32_t*)(uintptr_t)esp = (uint32_t)(uintptr_t)g_exit32;
    St()->teb32 = (uint64_t)teb;
    St()->stackTop = stackTop;
    Log("32-bit main thread %lu: thread block %p, state %p", GetCurrentThreadId(), teb, St());
    RingOpen();
    St()->esp32 = 0;
    StartDlls32();
    Log("32-bit entry point starts");
    typedef uint32_t (*enter_t)(uint64_t entry, uint64_t esp);
    int result = (int)((enter_t)g_enter64)(img.entry, esp);
    if (St()->exiting) Log("32-bit program ended (exit code %d)", St()->exitCode);
    else Log("32-bit entry point returned %d", result);
    return St()->exiting ? St()->exitCode : result;
}

}  // namespace arcade
