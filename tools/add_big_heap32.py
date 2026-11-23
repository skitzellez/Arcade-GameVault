# one-off edit: big heap blocks of 32-bit programs (and the D3D9 bridge's lock copies) below 2 GB
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")


def rep(a, b):
    global s
    assert a in s, a[:60]
    s = s.replace(a, b, 1)


rep('''static uint64_t B_GetProcessHeap() { return (uint64_t)LowHeap(); }''', '''static uint64_t B_GetProcessHeap() { return (uint64_t)LowHeap(); }
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
}''')
rep('''    {"kernel32.dll", "HeapCreate", 3, false, (void*)B_HeapCreate},''', '''    {"kernel32.dll", "HeapCreate", 3, false, (void*)B_HeapCreate},
    {"kernel32.dll", "HeapAlloc", 3, false, (void*)B_HeapAlloc},
    {"kernel32.dll", "HeapFree", 3, false, (void*)B_HeapFree},
    {"kernel32.dll", "HeapSize", 3, false, (void*)B_HeapSize},
    {"kernel32.dll", "HeapReAlloc", 4, false, (void*)B_HeapReAlloc},''')
rep('''void* Alloc32(size_t size) { return HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, size); }
void Free32(void* p) {
    if (p) HeapFree(LowHeap(), 0, p);
}''', '''void* Alloc32(size_t size) { return (void*)(uintptr_t)B_HeapAlloc((uint64_t)LowHeap(), HEAP_ZERO_MEMORY, size); }
void Free32(void* p) {
    if (p) B_HeapFree((uint64_t)LowHeap(), 0, (uint64_t)(uintptr_t)p);
}''')
if "#include <map>" not in s:
    s = s.replace("#include <deque>", "#include <deque>\n#include <map>", 1)
p.write_text(s, encoding="utf-8")
print("ok")
