# one-off edit: ARCADE_RING=<file> writes every bridged call into a memory-mapped file (survives a crash, no I/O per call)
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")
ring = '''// testing: ARCADE_RING=<file> (PC build): every bridged call's name into a memory-mapped file, which keeps what
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
    int n = snprintf(line, sizeof line, "%s <- %08x\\n", s.c_str(), ret);
    if (n <= 0) return;
    if (g_ringPos + n >= g_ringSize) g_ringPos = 0;
    memcpy(g_ring + g_ringPos, line, n);
    g_ringPos += n;
}

'''
if "RingPut" not in s:
    s = s.replace("bool g_trace32 = false;", ring + "bool g_trace32 = false;", 1)
    s = s.replace("    g_st->resumeEip = stack[0];\n", "    g_st->resumeEip = stack[0];\n    if (i < g_fn.size()) RingPut(g_fn[i].name, stack[0]);\n", 1)
    s = s.replace("    g_st->teb32 = (uint64_t)teb;", "    g_st->teb32 = (uint64_t)teb;\n    RingOpen();", 1)
s = s.replace('#include <vector>', '#include <vector>\n#include <cstdio>', 1)
p.write_text(s, encoding="utf-8")
print("ok", "RingPut(g_fn" in s)
