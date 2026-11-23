# one-off edit: file mappings for 32-bit programs below 2 GB (a 64-bit view address lost its top half: Dragon Dance)
from pathlib import Path
p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\peload32.cpp")
s = p.read_text(encoding="utf-8")
code = r'''// --- file mappings for 32-bit programs: views must be below 2 GB. Memory-only ones (no file: shared memory between
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
        if (m.mem && m.file) {   // a file's contents, read once (what the game writes there stays in memory)
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
'''
anchor = "static uint64_t B_GlobalMemoryStatus(uint64_t out) {"
assert s.count(anchor) == 1
s = s.replace(anchor, code + anchor)
row = '''    {"kernel32.dll", "GlobalMemoryStatus", 1, false, (void*)B_GlobalMemoryStatus},'''
s = s.replace(row, row + '''
    {"kernel32.dll", "CreateFileMappingA", 6, false, (void*)B_CreateFileMappingA},
    {"kernel32.dll", "CreateFileMappingW", 6, false, (void*)B_CreateFileMappingW},
    {"kernel32.dll", "OpenFileMappingA", 3, false, (void*)B_OpenFileMappingA},
    {"kernel32.dll", "OpenFileMappingW", 3, false, (void*)B_OpenFileMappingW},
    {"kernel32.dll", "MapViewOfFile", 5, false, (void*)B_MapViewOfFile},
    {"kernel32.dll", "MapViewOfFileEx", 6, false, (void*)B_MapViewOfFileEx},
    {"kernel32.dll", "UnmapViewOfFile", 1, false, (void*)B_UnmapViewOfFile},
    {"kernel32.dll", "FlushViewOfFile", 2, false, (void*)B_FlushViewOfFile},''')
p.write_text(s, encoding="utf-8")
print("ok")
