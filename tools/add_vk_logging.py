# one-off edit: log app memory limit (App vs Game), Vulkan instance/device results, Mesa's log to a file
from pathlib import Path

root = Path(__file__).resolve().parent.parent


def edit(rel, pairs):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert a in s, (rel, a[:70])
        s = s.replace(a, b, 1)
    p.write_text(s, encoding="utf-8")


edit("src/uwp_main.cpp", [
    ('#include <winrt/Windows.Storage.h>\n', '#include <winrt/Windows.Storage.h>\n#include <winrt/Windows.System.h>\n'),
    ('        arcade::Log("Game Vault arcade loader test");\n',
     '''        arcade::Log("Game Vault arcade loader test");
        {   // Direct3D 12 (Dozen, under DXVK) needs app type Game on an Xbox; an App gets about 1 GB
            uint64_t limit = Windows::System::MemoryManager::AppMemoryUsageLimit();
            arcade::Log("memory limit %llu MB (%s)", (unsigned long long)(limit >> 20), limit >= (2048ull << 20) ? "Game" : "App");
        }
        // Mesa's own messages (Dozen) into LocalState\\mesa_log.txt
        _wputenv_s(L"MESA_LOG", L"file");
        _wputenv_s(L"MESA_LOG_FILE", (local + L"\\\\mesa_log.txt").c_str());
'''),
])

edit("src/shims.cpp", [
    ('''static VkVoid_t WINAPI S_vkGetInstanceProcAddr(void* instance, const char* name) {''',
     '''// the start-up calls are logged (what a driver refuses shows in the loader's log)
typedef LONG(WINAPI* VkCreateInstance_t)(const void*, const void*, void**);
typedef LONG(WINAPI* VkEnumPhys_t)(void*, uint32_t*, void**);
static VkCreateInstance_t g_vkCreateInstance;
static VkEnumPhys_t g_vkEnumPhys;
static LONG WINAPI W_vkCreateInstance(const void* info, const void* alloc, void** out) {
    // VkInstanceCreateInfo: sType, pNext, flags, pApplicationInfo (apiVersion at +0x28), layers, extensions
    auto ci = (const uint8_t*)info;
    const uint8_t* app = *(const uint8_t* const*)(ci + 0x18);
    uint32_t api = app ? *(const uint32_t*)(app + 0x28) : 0;
    uint32_t next = *(const uint32_t*)(ci + 0x30);
    LONG r = g_vkCreateInstance(info, alloc, out);
    Log("Vulkan: vkCreateInstance (API %u.%u, %u extensions) = %ld", api >> 22, (api >> 12) & 0x3FF, next, r);
    return r;
}
static LONG WINAPI W_vkEnumeratePhysicalDevices(void* inst, uint32_t* n, void** devs) {
    LONG r = g_vkEnumPhys(inst, n, devs);
    if (!devs) Log("Vulkan: %u graphics device(s) (result %ld)", n ? *n : 0, r);
    return r;
}
static VkVoid_t WINAPI S_vkGetInstanceProcAddr(void* instance, const char* name) {'''),
    ('''    if (!f && !strcmp(name, "vkEnumerateInstanceLayerProperties"))''',
     '''    if (f && !strcmp(name, "vkCreateInstance")) {
        g_vkCreateInstance = (VkCreateInstance_t)f;
        return (VkVoid_t)W_vkCreateInstance;
    }
    if (f && !strcmp(name, "vkEnumeratePhysicalDevices")) {
        g_vkEnumPhys = (VkEnumPhys_t)f;
        return (VkVoid_t)W_vkEnumeratePhysicalDevices;
    }
    if (!f && !strcmp(name, "vkEnumerateInstanceLayerProperties"))'''),
])
print("ok")
