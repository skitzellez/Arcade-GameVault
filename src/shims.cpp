// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// Stand-ins for Windows calls a game makes that an Xbox app cannot (or must not) make as they are: the game's own
// module handle and file name, its command line, process exit, message boxes, ... Grows game by game; every call
// still missing shows in the log (peload.cpp).

#include "peload.h"
#include <windows.h>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <algorithm>

namespace arcade {

static const LoadedImage* g_img = nullptr;
static std::wstring g_path, g_cmdW;
static std::string g_cmdA;
// the program's base: null while its DLLs load (before SetProgram)
static uint8_t* ProgramBase() { return g_img ? g_img->base : nullptr; }

const std::wstring& ProgramPathW() { return g_path; }
const std::wstring& ProgramCommandLineW() { return g_cmdW; }   // "\"path\" args" (32-bit GetCommandLine too)   // the program's full path (32-bit GetModuleFileName too)
void SetProgram(const LoadedImage* img, const std::wstring& path, const std::wstring& commandLine) {
    g_img = img;
    g_path = path;
    g_cmdW = L"\"" + path + L"\"" + (commandLine.empty() ? L"" : L" " + commandLine);
    int n = WideCharToMultiByte(CP_ACP, 0, g_cmdW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    g_cmdA.assign(n, 0);
    WideCharToMultiByte(CP_ACP, 0, g_cmdW.c_str(), -1, &g_cmdA[0], n, nullptr, nullptr);
}

static std::string Narrow(const wchar_t* w) {
    if (!w) return "(null)";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    if (!s.empty() && !s.back()) s.pop_back();
    return s;
}

// --- kernel32 ---------------------------------------------------------------------------------------------
static HMODULE WINAPI S_GetModuleHandleA(LPCSTR name) {
    if (!name) return (HMODULE)ProgramBase();
    if (void* d = Dll64Loaded(name)) return (HMODULE)d;
    return GetModuleHandleA(name);
}
static HMODULE WINAPI S_GetModuleHandleW(LPCWSTR name) {
    if (!name) return (HMODULE)ProgramBase();
    if (void* d = Dll64Loaded(Narrow(name).c_str())) return (HMODULE)d;
    return GetModuleHandleW(name);
}
// the program, a DLL of ours, or (known = false) a module of the process
static std::wstring ModulePath(HMODULE m, bool& known) {
    std::wstring p;
    known = true;
    if (!m || m == (HMODULE)ProgramBase()) return g_path;
    if (Dll64Path(m, p)) return p;
    known = false;
    return p;
}
static DWORD WINAPI S_GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD size) {
    bool known;
    std::wstring path = ModulePath(m, known);
    if (!known) return GetModuleFileNameW(m, out, size);
    DWORD n = (DWORD)std::min<size_t>(path.size(), size ? size - 1 : 0);
    if (size) {
        memcpy(out, path.c_str(), n * 2);
        out[n] = 0;
    }
    return n;
}
static DWORD WINAPI S_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD size) {
    bool known;
    std::wstring path = ModulePath(m, known);
    if (!known) return GetModuleFileNameA(m, out, size);
    std::string p = Narrow(path.c_str());
    DWORD n = (DWORD)std::min<size_t>(p.size(), size ? size - 1 : 0);
    if (size) {
        memcpy(out, p.c_str(), n);
        out[n] = 0;
    }
    return n;
}
static LPWSTR WINAPI S_GetCommandLineW() { return &g_cmdW[0]; }
static LPSTR WINAPI S_GetCommandLineA() { return &g_cmdA[0]; }
static void WINAPI S_ExitProcess(UINT code) { ExitProgram((int)code); }
static BOOL WINAPI S_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess()) ExitProgram((int)code);
    return FALSE;
}
static void WINAPI S_OutputDebugStringA(LPCSTR s) { Log("program says: %s", s ? s : ""); }
static void WINAPI S_OutputDebugStringW(LPCWSTR s) { Log("program says: %s", Narrow(s).c_str()); }

// --- user32 -----------------------------------------------------------------------------------------------
static int WINAPI S_MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT) {
    Log("message box \"%s\": %s", Narrow(title).c_str(), Narrow(text).c_str());
    return IDOK;
}
static int WINAPI S_MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) {
    Log("message box \"%s\": %s", title ? title : "", text ? text : "");
    return IDOK;
}

static int __cdecl S_wsprintfA(LPSTR out, LPCSTR fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, 1024, fmt, ap);   // wsprintf's limit is 1024 characters
    va_end(ap);
    return n;
}

// --- ucrt: exit goes through us too, or it would end the whole app ----------------------------------------
static void __cdecl S_exit(int code) { ExitProgram(code); }

// --- vcruntime: C++ throw. The real one finds the program's base with RtlPcToFileHeader, which knows only modules
// Windows loaded itself; for our mapped program it gets 0 and the catch side reads garbage. Same record, right base.
static void __stdcall S_CxxThrowException(void* object, void* throwInfo) {
    ULONG_PTR args[4] = {0x19930520, (ULONG_PTR)object, (ULONG_PTR)throwInfo, 0};
    if (void* mb = MappedImageBase(throwInfo)) {
        args[3] = (ULONG_PTR)mb;
    } else {
        PVOID b = nullptr;
        RtlPcToFileHeader(throwInfo, &b);
        args[3] = (ULONG_PTR)b;
    }
    // a throwInfo with the "pure" attribute (/clr:pure) uses another magic number; not used by games
    RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 4, args);
}

// which module an address is in: Windows does not know the images we map (msvcr100's start-up asks for its own)
static PVOID WINAPI S_RtlPcToFileHeader(PVOID pc, PVOID* base) {
    if (void* mb = MappedImageBase(pc)) {
        *base = mb;
        return mb;
    }
    return RtlPcToFileHeader(pc, base);
}

// --- run-time lookups: LoadLibrary / GetProcAddress by the game. An app can only reach DLLs already in the
// process, so other system DLLs get a stand-in handle that remembers the name; GetProcAddress then gives our
// stand-in for that function when there is one, else the real one, else nullptr (as for a missing function).
static std::vector<std::string> g_fakeDlls;
static std::string BaseName(const char* p) {
    std::string s = p ? p : "";
    size_t k = s.find_last_of("\\/");
    if (k != std::string::npos) s = s.substr(k + 1);
    if (s.find('.') == std::string::npos) s += ".dll";
    return s;
}
static HMODULE FakeDll(const std::string& name) {
    for (size_t i = 0; i < g_fakeDlls.size(); i++)
        if (!_stricmp(g_fakeDlls[i].c_str(), name.c_str())) return (HMODULE)(uintptr_t)(0xFADE0000 + i * 16);
    g_fakeDlls.push_back(name);
    return (HMODULE)(uintptr_t)(0xFADE0000 + (g_fakeDlls.size() - 1) * 16);
}
static const char* FakeName(HMODULE m) {
    uintptr_t v = (uintptr_t)m;
    if (v < 0xFADE0000 || v >= 0xFADE0000 + g_fakeDlls.size() * 16) return nullptr;
    return g_fakeDlls[(v - 0xFADE0000) / 16].c_str();
}
static bool FindShimFor(const char* dll);   // (below the stand-in table)
static HMODULE WINAPI S_LoadLibraryA(LPCSTR name) {
    std::string n = BaseName(name);
    if (void* d = Dll64Load(n.c_str())) {   // beside the program, or ours (x64 folder)
        Log("program loads %s: mapped by us", n.c_str());
        return (HMODULE)d;
    }
    HMODULE m = GetModuleHandleA(n.c_str());
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    if (!m) {   // shipped in our package (d3dcompiler_47.dll: Microsoft's redistributable shader compiler)
        std::wstring w(n.begin(), n.end());
        m = LoadPackagedLibrary(w.c_str(), 0);
    }
#endif
    // (no real loading by name on the PC either: it stays like the Xbox. A PC's own PhysXUpdateLoader64.dll swapped
    // in another PhysX than the game's and crashed it)
    if (!m) {
        // only DLLs we have stand-ins for get a stand-in handle; any other is "not found", as on a PC without it
        // (a game then skips what it is for: a stand-in handle made one take XAudio2 2.7 as installed)
        static const char* kStandIn[] = {"user32.dll", "gdi32.dll", "shell32.dll", "shcore.dll", "d3d11.dll",
                                         "dxgi.dll", "kernel32.dll", "vulkan-1.dll", "dinput8.dll",
                                         "xaudio2_7.dll"};   // xaudio27.cpp (on the system's XAudio2 2.9)
        bool known = false;
        for (const char* k : kStandIn) known |= !_stricmp(k, n.c_str());
        if (!known) {
            Log("program loads %s: not there", n.c_str());
            SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
        m = FakeDll(n);
    }
    Log("program loads %s: %s", n.c_str(), FakeName(m) ? "stand-in" : "in the process");
    return m;
}
static HMODULE WINAPI S_LoadLibraryW(LPCWSTR name) { return S_LoadLibraryA(Narrow(name).c_str()); }
static HMODULE WINAPI S_LoadLibraryExA(LPCSTR name, HANDLE, DWORD) { return S_LoadLibraryA(name); }
static HMODULE WINAPI S_LoadLibraryExW(LPCWSTR name, HANDLE, DWORD) { return S_LoadLibraryA(Narrow(name).c_str()); }
static BOOL WINAPI S_FreeLibrary(HMODULE m) {
    std::wstring p;
    return FakeName(m) || Dll64Path(m, p) ? TRUE : FreeLibrary(m);
}
static FARPROC WINAPI S_GetProcAddress(HMODULE m, LPCSTR func) {
    void* own;
    if (Dll64Export(m, func, &own)) {   // a DLL of ours: its stand-ins first, like its imports
        std::wstring p;
        Dll64Path(m, p);
        std::string dll = BaseName(Narrow(p.c_str()).c_str());
        if ((uintptr_t)func > 0xFFFF)
            if (void* s = FindShim(dll.c_str(), func)) return (FARPROC)s;
        if (!own && (uintptr_t)func > 0xFFFF) Log("program asks for %s!%s: none", dll.c_str(), func);
        return (FARPROC)own;
    }
    if ((uintptr_t)func <= 0xFFFF) return FakeName(m) ? nullptr : GetProcAddress(m, func);   // by ordinal
    if (!m || m == (HMODULE)ProgramBase()) return nullptr;   // the game's own exports: not kept
    char path[MAX_PATH] = "";
    const char* fake = FakeName(m);
    if (fake) strcpy_s(path, fake);
    else GetModuleFileNameA(m, path, MAX_PATH);
    std::string dll = BaseName(path);
    if (void* s = FindShim(dll.c_str(), func)) return (FARPROC)s;
    FARPROC p = fake ? nullptr : GetProcAddress(m, func);
    if (!p) Log("program asks for %s!%s: none", dll.c_str(), func);
    return p;
}
// --- vulkan-1.dll: an Xbox has no Vulkan loader. A program (DXVK) gets Mesa's Vulkan-on-Direct3D 12 driver, Dozen
// (vulkan_dzn.dll in our package), straight, the way xemu does: vkGetInstanceProcAddr is the driver's own. ------
typedef void(WINAPI* VkVoid_t)();
typedef VkVoid_t(WINAPI* VkGipa_t)(void*, const char*);
static VkGipa_t VulkanDriver() {
    static VkGipa_t gipa = nullptr;
    static bool tried = false;
    if (tried) return gipa;
    tried = true;
    _putenv("MESA_LOG=file");   // Mesa's errors to stderr, not to a debugger
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    HMODULE m = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
#else
    HMODULE m = LoadLibraryW(L"vulkan_dzn.dll");   // PC testing: Dozen beside the loader, else the system's loader
    if (!m) {
        if (HMODULE v = LoadLibraryW(L"vulkan-1.dll")) {
            gipa = (VkGipa_t)GetProcAddress(v, "vkGetInstanceProcAddr");
            Log("Vulkan: the PC's own Vulkan loader");
            return gipa;
        }
    }
#endif
    if (!m) {
        Log("Vulkan: vulkan_dzn.dll could not be loaded (error %lu)", GetLastError());
        return nullptr;
    }
    typedef LONG(WINAPI * Negotiate_t)(uint32_t*);
    auto negotiate = (Negotiate_t)GetProcAddress(m, "vk_icdNegotiateLoaderICDInterfaceVersion");
    gipa = (VkGipa_t)GetProcAddress(m, "vk_icdGetInstanceProcAddr");
    uint32_t version = 5;
    if (negotiate) negotiate(&version);
    Log("Vulkan: Mesa Dozen loaded (driver interface %u)", version);
    return gipa;
}
static LONG WINAPI S_vkEnumerateInstanceLayerProperties(uint32_t* n, void*) {
    *n = 0;
    return 0;
}
static LONG WINAPI S_vkEnumerateDeviceLayerProperties(void*, uint32_t* n, void*) {
    *n = 0;
    return 0;
}
// the start-up calls are logged (what a driver refuses shows in the loader's log)
typedef LONG(WINAPI* VkCreateInstance_t)(const void*, const void*, void**);
typedef LONG(WINAPI* VkEnumPhys_t)(void*, uint32_t*, void**);
static VkCreateInstance_t g_vkCreateInstance;
static VkEnumPhys_t g_vkEnumPhys;
static LONG WINAPI W_vkCreateInstance(const void* info, const void* alloc, void** out) {
    // VkInstanceCreateInfo: sType, pNext, flags, pApplicationInfo (apiVersion at +0x28), layers, extensions
    // VkApplicationInfo: apiVersion at +0x2C. Raised to 1.2 (Dozen's): a driver hands out only the core functions of
    // the version asked for, and DXVK 1.10 asks for 1.1 but calls vkResetQueryPool(EXT), core in 1.2
    uint8_t ciCopy[0x40], appCopy[0x30];
    memcpy(ciCopy, info, sizeof ciCopy);
    const uint8_t* app = *(const uint8_t* const*)(ciCopy + 0x18);
    uint32_t api = app ? *(const uint32_t*)(app + 0x2C) : 0;
    const uint32_t v12 = (1u << 22) | (2u << 12);
    if (app && api < v12) {
        memcpy(appCopy, app, sizeof appCopy);
        *(uint32_t*)(appCopy + 0x2C) = v12;
        *(const uint8_t**)(ciCopy + 0x18) = appCopy;
    }
    uint32_t next = *(const uint32_t*)(ciCopy + 0x30);
    LONG r = g_vkCreateInstance(ciCopy, alloc, out);
    Log("Vulkan: vkCreateInstance (API %u.%u asked, %u extensions) = %ld", api >> 22, (api >> 12) & 0x3FF, next, r);
    return r;
}
typedef LONG(WINAPI* VkCreateDevice_t)(void*, const void*, const void*, void**);
static VkCreateDevice_t g_vkCreateDevice;
static LONG WINAPI W_vkCreateDevice(void* phys, const void* info, const void* alloc, void** out) {
    // VkDeviceCreateInfo: +0x14 queue infos, +0x28 extension count, +0x38 pEnabledFeatures; pNext chain at +8
    auto ci = (const uint8_t*)info;
    int chain = 0;
    for (auto p = *(const uint8_t* const*)(ci + 8); p && chain < 64; p = *(const uint8_t* const*)(p + 8)) chain++;
    LONG r = g_vkCreateDevice(phys, info, alloc, out);
    Log("Vulkan: vkCreateDevice (%u extensions, %d feature blocks) = %ld", *(const uint32_t*)(ci + 0x28), chain, r);
    return r;
}
static LONG WINAPI W_vkEnumeratePhysicalDevices(void* inst, uint32_t* n, void** devs) {
    LONG r = g_vkEnumPhys(inst, n, devs);
    if (!devs) Log("Vulkan: %u graphics device(s) (result %ld)", n ? *n : 0, r);
    return r;
}
// device calls whose failures are logged (swap chains: the picture to the screen)
typedef LONG(WINAPI* VkCall4_t)(void*, const void*, const void*, void*);
typedef LONG(WINAPI* VkAcquire_t)(void*, void*, uint64_t, void*, void*, uint32_t*);
typedef LONG(WINAPI* VkPresent_t)(void*, const void*);
static VkCall4_t g_vkCreateSwapchain;
static VkAcquire_t g_vkAcquire;
static VkPresent_t g_vkPresent;
static VkGipa_t g_vkGdpa;
static LONG WINAPI W_vkCreateSwapchainKHR(void* dev, const void* info, const void* alloc, void* out) {
    // VkSwapchainCreateInfoKHR: +0x20 minImageCount, +0x24 format, +0x2C extent, +0x60 presentMode
    auto ci = (const uint8_t*)info;
    LONG r = g_vkCreateSwapchain(dev, info, alloc, out);
    Log("Vulkan: vkCreateSwapchainKHR %ux%u, %u images, format %u, mode %u = %ld", *(const uint32_t*)(ci + 0x2C),
        *(const uint32_t*)(ci + 0x30), *(const uint32_t*)(ci + 0x20), *(const uint32_t*)(ci + 0x24),
        *(const uint32_t*)(ci + 0x60), r);
    return r;
}
static LONG WINAPI W_vkAcquireNextImageKHR(void* dev, void* sc, uint64_t timeout, void* sem, void* fence, uint32_t* index) {
    LONG r = g_vkAcquire(dev, sc, timeout, sem, fence, index);
    static int logged = 0;
    if (r < 0 && logged++ < 5) Log("Vulkan: vkAcquireNextImageKHR = %ld", r);
    return r;
}
static LONG WINAPI W_vkQueuePresentKHR(void* queue, const void* info) {
    LONG r = g_vkPresent(queue, info);
    static int logged = 0;
    if (r != 0 && logged++ < 5) Log("Vulkan: vkQueuePresentKHR = %ld", r);
    return r;
}
// an extension function the driver only has in core Vulkan (Dozen: vkResetQueryPoolEXT -> vkResetQueryPool)
static VkVoid_t CoreNamed(VkGipa_t get, void* handle, const char* name) {
    size_t n = strlen(name);
    if (n < 4 || (strcmp(name + n - 3, "EXT") && strcmp(name + n - 3, "KHR"))) return nullptr;
    char core[128];
    if (n - 3 >= sizeof core) return nullptr;
    memcpy(core, name, n - 3);
    core[n - 3] = 0;
    return get(handle, core);
}
// (testing, with the memory tally) GPU memory DXVK asks for, by memory type
typedef LONG(WINAPI* VkAllocMem_t)(void*, const void*, const void*, uint64_t*);
typedef void(WINAPI* VkFreeMem_t)(void*, uint64_t, const void*);
static VkAllocMem_t g_vkAllocMem;
static VkFreeMem_t g_vkFreeMem;
static LONG WINAPI W_vkAllocateMemory(void* dev, const void* info, const void* alloc, uint64_t* mem) {
    LONG r = g_vkAllocMem(dev, info, alloc, mem);
    if (r == 0 && mem) {
        static const char* names[16] = {"vk-type0", "vk-type1", "vk-type2", "vk-type3", "vk-type4", "vk-type5",
                                        "vk-type6", "vk-type7", "vk-type8", "vk-type9", "vk-typeX", "vk-typeX",
                                        "vk-typeX", "vk-typeX", "vk-typeX", "vk-typeX"};
        uint32_t type = *(const uint32_t*)((const uint8_t*)info + 0x18);
        MemTallyAdd(names[type & 15], (uintptr_t)*mem, (size_t)*(const uint64_t*)((const uint8_t*)info + 0x10));
    }
    return r;
}
static void WINAPI W_vkFreeMemory(void* dev, uint64_t mem, const void* alloc) {
    if (mem) MemTallyRemove((uintptr_t)mem);
    g_vkFreeMem(dev, mem, alloc);
}
static VkVoid_t WINAPI W_vkGetDeviceProcAddr(void* device, const char* name) {
    VkVoid_t f = g_vkGdpa(device, name);
    if (!f && name) f = CoreNamed(g_vkGdpa, device, name);
    if (!f || !name) return f;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (VkVoid_t)W_vkGetDeviceProcAddr;
    if (MemTallyOn() && !strcmp(name, "vkAllocateMemory")) {
        g_vkAllocMem = (VkAllocMem_t)f;
        return (VkVoid_t)W_vkAllocateMemory;
    }
    if (MemTallyOn() && !strcmp(name, "vkFreeMemory")) {
        g_vkFreeMem = (VkFreeMem_t)f;
        return (VkVoid_t)W_vkFreeMemory;
    }
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        g_vkCreateSwapchain = (VkCall4_t)f;
        return (VkVoid_t)W_vkCreateSwapchainKHR;
    }
    if (!strcmp(name, "vkAcquireNextImageKHR")) {
        g_vkAcquire = (VkAcquire_t)f;
        return (VkVoid_t)W_vkAcquireNextImageKHR;
    }
    if (!strcmp(name, "vkQueuePresentKHR")) {
        g_vkPresent = (VkPresent_t)f;
        return (VkVoid_t)W_vkQueuePresentKHR;
    }
    return f;
}
static VkVoid_t WINAPI S_vkGetInstanceProcAddr(void* instance, const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (VkVoid_t)S_vkGetInstanceProcAddr;
    VkGipa_t gipa = VulkanDriver();
    VkVoid_t f = gipa ? gipa(instance, name) : nullptr;
    if (!f && gipa && instance) f = CoreNamed(gipa, instance, name);
    if (f && !strcmp(name, "vkGetDeviceProcAddr")) {
        g_vkGdpa = (VkGipa_t)f;
        return (VkVoid_t)W_vkGetDeviceProcAddr;
    }
    if (f && !strcmp(name, "vkCreateInstance")) {
        g_vkCreateInstance = (VkCreateInstance_t)f;
        return (VkVoid_t)W_vkCreateInstance;
    }
    if (f && !strcmp(name, "vkCreateDevice")) {
        g_vkCreateDevice = (VkCreateDevice_t)f;
        return (VkVoid_t)W_vkCreateDevice;
    }
    if (f && !strcmp(name, "vkEnumeratePhysicalDevices")) {
        g_vkEnumPhys = (VkEnumPhys_t)f;
        return (VkVoid_t)W_vkEnumeratePhysicalDevices;
    }
    if (!f && !strcmp(name, "vkEnumerateInstanceLayerProperties")) return (VkVoid_t)S_vkEnumerateInstanceLayerProperties;
    if (!f && !strcmp(name, "vkEnumerateDeviceLayerProperties")) return (VkVoid_t)S_vkEnumerateDeviceLayerProperties;
    return f;
}

HRESULT WINAPI S_DirectInput8Create(HINSTANCE, DWORD, REFIID, void**, void*);   // dinput.cpp

// winmm: the game clock (a missing-function stub answering 0 stops time)
static DWORD WINAPI S_timeGetTime() { return (DWORD)GetTickCount64(); }
static UINT WINAPI S_timePeriod(UINT) { return 0; }   // TIMERR_NOERROR
// shell32: special folders (Documents, AppData) -> a "UserData" folder beside the program, made on first use
static std::wstring UserDataFolder() {
    std::wstring d = g_path.substr(0, g_path.find_last_of(L"\\/") + 1) + L"UserData";
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    CreateDirectoryFromAppW(d.c_str(), nullptr);
#else
    CreateDirectoryW(d.c_str(), nullptr);
#endif
    return d;
}
static HRESULT WINAPI S_SHGetFolderPathW(HWND, int csidl, HANDLE, DWORD, LPWSTR out) {
    std::wstring d = UserDataFolder();
    wcsncpy_s(out, MAX_PATH, d.c_str(), _TRUNCATE);
    static bool said = false;
    if (!said) Log("special folder %d -> %s", csidl, Narrow(d.c_str()).c_str());
    said = true;
    return S_OK;
}
static HRESULT WINAPI S_SHGetFolderPathA(HWND, int, HANDLE, DWORD, LPSTR out) {
    std::string d = Narrow(UserDataFolder().c_str());
    strncpy_s(out, MAX_PATH, d.c_str(), _TRUNCATE);
    return S_OK;
}
// ole32: CoInitialize is CoInitializeEx in an app
static HRESULT WINAPI S_CoInitialize(void*) { return CoInitializeEx(nullptr, COINIT_MULTITHREADED); }

// shcore: DPI (the TV counts as 96 DPI, nothing scaled)
static HRESULT WINAPI S_SetProcessDpiAwareness(int) { return S_OK; }
static HRESULT WINAPI S_GetDpiForMonitor(HANDLE, int, UINT* x, UINT* y) {
    if (x) *x = 96;
    if (y) *y = 96;
    return S_OK;
}
static BOOL WINAPI S_SetProcessDPIAware() { return TRUE; }
static UINT WINAPI S_GetDpiForWindow(HWND) { return 96; }
static BOOL WINAPI S_SetProcessDpiAwarenessContext(HANDLE) { return TRUE; }
// asserts in the game: logged, and the game goes on (the real one ends the whole app)
static void __cdecl S_wassert(const wchar_t* msg, const wchar_t* file, unsigned line) {
    Log("program assert: %s (%s:%u)", Narrow(msg).c_str(), Narrow(file).c_str(), line);
}
static void __cdecl S_assert(const char* msg, const char* file, unsigned line) {
    Log("program assert: %s (%s:%u)", msg ? msg : "", file ? file : "", line);
}
static void __cdecl S_abort() {
    Log("program called abort()");
    ExitProgram(3);
}

struct Shim {
    const char* dll;   // nullptr: any DLL
    const char* name;
    void* fn;
};
static const Shim kShims[] = {
    {"kernel32.dll", "GetModuleHandleW", (void*)S_GetModuleHandleW},
    {"kernel32.dll", "GetModuleHandleA", (void*)S_GetModuleHandleA},
    {"kernel32.dll", "GetModuleFileNameW", (void*)S_GetModuleFileNameW},
    {"kernel32.dll", "GetModuleFileNameA", (void*)S_GetModuleFileNameA},
    {"kernel32.dll", "GetCommandLineW", (void*)S_GetCommandLineW},
    {"kernel32.dll", "GetCommandLineA", (void*)S_GetCommandLineA},
    {"kernel32.dll", "ExitProcess", (void*)S_ExitProcess},
    {"kernel32.dll", "TerminateProcess", (void*)S_TerminateProcess},
    {"kernel32.dll", "OutputDebugStringA", (void*)S_OutputDebugStringA},
    {"kernel32.dll", "OutputDebugStringW", (void*)S_OutputDebugStringW},
    {"user32.dll", "MessageBoxW", (void*)S_MessageBoxW},
    {"user32.dll", "MessageBoxA", (void*)S_MessageBoxA},
    {"user32.dll", "wsprintfA", (void*)S_wsprintfA},
    {nullptr, "_CxxThrowException", (void*)S_CxxThrowException},
    {"kernel32.dll", "RtlPcToFileHeader", (void*)S_RtlPcToFileHeader},
    {"vulkan-1.dll", "vkGetInstanceProcAddr", (void*)S_vkGetInstanceProcAddr},
    {"dinput8.dll", "DirectInput8Create", (void*)S_DirectInput8Create},
    {"kernel32.dll", "LoadLibraryA", (void*)S_LoadLibraryA},
    {"kernel32.dll", "LoadLibraryW", (void*)S_LoadLibraryW},
    {"kernel32.dll", "LoadLibraryExA", (void*)S_LoadLibraryExA},
    {"kernel32.dll", "LoadLibraryExW", (void*)S_LoadLibraryExW},
    {"kernel32.dll", "FreeLibrary", (void*)S_FreeLibrary},
    {"kernel32.dll", "GetProcAddress", (void*)S_GetProcAddress},
    {"shcore.dll", "SetProcessDpiAwareness", (void*)S_SetProcessDpiAwareness},
    {"winmm.dll", "timeGetTime", (void*)S_timeGetTime}, {"winmm.dll", "timeBeginPeriod", (void*)S_timePeriod},
    {"winmm.dll", "timeEndPeriod", (void*)S_timePeriod}, {"shell32.dll", "SHGetFolderPathW", (void*)S_SHGetFolderPathW},
    {"shell32.dll", "SHGetFolderPathA", (void*)S_SHGetFolderPathA}, {"ole32.dll", "CoInitialize", (void*)S_CoInitialize},
    {"shcore.dll", "GetDpiForMonitor", (void*)S_GetDpiForMonitor},
    {"user32.dll", "SetProcessDPIAware", (void*)S_SetProcessDPIAware},
    {"user32.dll", "GetDpiForWindow", (void*)S_GetDpiForWindow},
    {"user32.dll", "SetProcessDpiAwarenessContext", (void*)S_SetProcessDpiAwarenessContext},
    {nullptr, "_wassert", (void*)S_wassert},
    {nullptr, "_assert", (void*)S_assert},
    {nullptr, "abort", (void*)S_abort},
    {nullptr, "exit", (void*)S_exit},
    {nullptr, "_exit", (void*)S_exit},
    {nullptr, "quick_exit", (void*)S_exit},
};

// a DLL whose calls are our stand-ins (dinput8, vulkan-1, ...): never loaded for real, even on a PC
static bool FindShimFor(const char* dll) {
    for (const Shim& x : kShims)
        if (x.dll && !_stricmp(x.dll, dll) && _stricmp(dll, "kernel32.dll") && _stricmp(dll, "user32.dll") &&
            _stricmp(dll, "shcore.dll"))
            return true;
    return false;
}
// Media Foundation (cameras, video capture): not there for the program. Every call answers "not implemented", so a game
// skips its camera; a missing-function stub would answer 0 = S_OK and leave the object pointers empty
static HRESULT WINAPI S_MediaFoundationNotThere() { return E_NOTIMPL; }
static bool IsMediaFoundation(const char* dll) {
    return !_stricmp(dll, "mfplat.dll") || !_stricmp(dll, "mf.dll") || !_stricmp(dll, "mfreadwrite.dll") ||
           !_stricmp(dll, "mfuuid.dll") || !_stricmp(dll, "mfcore.dll");
}

void* FindShim(const char* dll, const char* func) {
    if (IsMediaFoundation(dll) && func && !strncmp(func, "MF", 2)) return (void*)S_MediaFoundationNotThere;
    if (void* n = FindNetFsShim(dll, func)) return n;      // a game played from the NAS (netfs.cpp)
    if (void* t = FindMemTallyShim(dll, func)) return t;   // (testing) memory counted by module
    if (void* t = FindTbbShim(dll, func)) return t;        // TBB 3.0's allocator -> the process heap
    if (void* io = FindIoShim(dll, func)) return io;   // the game's arcade hardware (profile)
    if (void* m = FindMsvcrtShim(dll, func)) return m;  // old msvcrt.dll pieces
    if (void* a = FindXAudio27Shim(dll, func)) return a;   // XAudio2 2.7 (COM) on the system's 2.9
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    if (void* w = FindWindowShim(dll, func)) return w;
#endif
    for (const Shim& s : kShims) {
        if (strcmp(s.name, func)) continue;
        if (!s.dll || !_stricmp(s.dll, dll)) return s.fn;
        // kernel32 functions are also imported through kernelbase / api sets
        if (!_stricmp(s.dll, "kernel32.dll") && (!_stricmp(dll, "kernelbase.dll") || !_strnicmp(dll, "api-ms-win-core-", 16)))
            return s.fn;
    }
    return nullptr;
}

}  // namespace arcade
