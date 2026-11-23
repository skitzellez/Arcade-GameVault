# one-off edit: a small registry for 32-bit programs (peload32.cpp): keys and values the game makes are kept in memory,
# others are "not found" (missing functions answered 0 = success, and Dragon Dance used an empty key: crash)
from pathlib import Path

p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\peload32.cpp")
s = p.read_text(encoding="utf-8")

code = r'''// --- the registry for 32-bit programs: an arcade game's settings (Dragon Dance: Taito's keys) kept in memory. A key
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
static std::string RegRoot(uint64_t h) {
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
'''
anchor = "static uint64_t B_GlobalMemoryStatus(uint64_t out) {"
assert s.count(anchor) == 1
s = s.replace(anchor, code + anchor)
row = '''    {"kernel32.dll", "GlobalMemoryStatus", 1, false, (void*)B_GlobalMemoryStatus},'''
assert s.count(row) == 1
s = s.replace(row, row + '''
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
    {"advapi32.dll", "RegFlushKey", 1, false, (void*)B_RegFlushKey},''')
for inc in ["#include <map>", "#include <set>", "#include <mutex>"]:
    if inc not in s:
        s = s.replace("#include <vector>", "#include <vector>\n" + inc, 1)
p.write_text(s, encoding="utf-8")
print("ok")
