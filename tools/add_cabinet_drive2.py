# one-off edit: the cabinet drive D:\ for file checks too (GetFileAttributesA/W, GetDriveTypeA, GetDiskFreeSpaceA/ExA):
# Battle Gear 4 loops checking D:\ until it looks like the cabinet's drive. Also fixes trace.txt's path (was L"\t...").
from pathlib import Path

src = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src")
p = src / "peload32.cpp"
s = p.read_text(encoding="utf-8")
code = r'''// the cabinet drive (D:\ = the game's folder, profiles that say so) for file checks; misses logged (what a game waits for)
static uint64_t B_GetFileAttributesA(uint64_t name) {
    const char* n = (const char*)(uintptr_t)(uint32_t)name;
    std::string cab;
    if (RedirectCabinetPath(n, cab)) n = cab.c_str();
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
'''
anchor = "static uint64_t B_GlobalMemoryStatus(uint64_t out) {"
assert s.count(anchor) == 1
s = s.replace(anchor, code + anchor)
row = '''    {"kernel32.dll", "GlobalMemoryStatus", 1, false, (void*)B_GlobalMemoryStatus},'''
s = s.replace(row, row + '''
    {"kernel32.dll", "GetFileAttributesA", 1, false, (void*)B_GetFileAttributesA},
    {"kernel32.dll", "GetDriveTypeA", 1, false, (void*)B_GetDriveTypeA},
    {"kernel32.dll", "GetDiskFreeSpaceA", 5, false, (void*)B_GetDiskFreeSpaceA},''')
# RedirectCabinetPath must be declared before these
if s.index("bool RedirectCabinetPath(const char* in, std::string& out);") > s.index(code[:60]):
    s = s.replace(code[:60], "bool RedirectCabinetPath(const char* in, std::string& out);\n" + code[:60], 1)
p.write_text(s, encoding="utf-8")

u = src / "uwp_main.cpp"
t = u.read_text(encoding="utf-8")
bad = 'ReadAll(local + L"\\trace.txt")'
good = 'ReadAll(local + L"\\\\trace.txt")'
if bad in t and good not in t:
    t = t.replace(bad, good)
u.write_text(t, encoding="utf-8")
print("ok")
