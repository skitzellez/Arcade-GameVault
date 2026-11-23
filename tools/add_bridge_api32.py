# one-off edit: peload32.cpp gives the D3D9 bridge its pieces (bridge entries, low memory), 12 arguments per call,
# and asks the bridge for d3d9.dll imports of 32-bit programs
from pathlib import Path

root = Path(__file__).resolve().parent.parent


def edit(rel, pairs):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert a in s, (rel, a[:60])
        s = s.replace(a, b, 1)
    p.write_text(s, encoding="utf-8")


edit("src/peload32.cpp", [
    ('''        uint64_t a[8] = {};''', '''        uint64_t a[12] = {};'''),
    ('''        for (int k = 0; k < f.args && k < 8; k++) a[k] = (uint64_t)(int64_t)(int32_t)stack[1 + k];''',
     '''        for (int k = 0; k < f.args && k < 12; k++) a[k] = (uint64_t)(int64_t)(int32_t)stack[1 + k];'''),
    ('''            typedef uint64_t (*f8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            r = ((f8)f.target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);''',
     '''            typedef uint64_t (*f12)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
            r = ((f12)f.target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);'''),
    ('''static void* ResolveFor32(const std::string& dllIn, const std::string& fn, int& args, bool& cdecl_) {
    std::string dll = RealDll32(dllIn);''', '''void* FindD3D9Bridge32(const char* dll, const char* func, int& args);   // d3d9_bridge32.cpp
static void* ResolveFor32(const std::string& dllIn, const std::string& fn, int& args, bool& cdecl_) {
    std::string dll = RealDll32(dllIn);
    if (void* d = FindD3D9Bridge32(dll.c_str(), fn.c_str(), args)) {   // Direct3D 9 through the bridge (DXVK)
        cdecl_ = false;
        return d;
    }'''),
    ('''static uint32_t g_dispatch32, g_unhandledThunk;''', '''static uint32_t g_dispatch32, g_unhandledThunk;
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
void* Alloc32(size_t size) { return HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, size); }
void Free32(void* p) {
    if (p) HeapFree(LowHeap(), 0, p);
}'''),
])
print("ok")
