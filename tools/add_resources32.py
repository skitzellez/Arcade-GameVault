# one-off edit: resources of 32-bit images (FindResourceA/W, FindResourceExA/W, LoadResource, LockResource,
# SizeofResource, FreeResource) read straight from the images the loader mapped (d3dx9_42 keeps data there)
from pathlib import Path

p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\peload32.cpp")
s = p.read_text(encoding="utf-8")
code = r'''// --- resources of 32-bit images: the loader mapped them itself, so Windows does not know them. A resource is found in
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
'''
anchor = "static uint64_t B_GlobalMemoryStatus(uint64_t out) {"
assert s.count(anchor) == 1
s = s.replace(anchor, code + anchor)
row = '''    {"kernel32.dll", "GlobalMemoryStatus", 1, false, (void*)B_GlobalMemoryStatus},'''
s = s.replace(row, row + '''
    {"kernel32.dll", "FindResourceA", 3, false, (void*)B_FindResourceA},
    {"kernel32.dll", "FindResourceW", 3, false, (void*)B_FindResourceW},
    {"kernel32.dll", "FindResourceExA", 4, false, (void*)B_FindResourceExA},
    {"kernel32.dll", "FindResourceExW", 4, false, (void*)B_FindResourceExW},
    {"kernel32.dll", "LoadResource", 2, false, (void*)B_LoadResource},
    {"kernel32.dll", "LockResource", 1, false, (void*)B_LockResource},
    {"kernel32.dll", "SizeofResource", 2, false, (void*)B_SizeofResource},
    {"kernel32.dll", "FreeResource", 1, false, (void*)B_FreeResource},''')
# the main image's base, for a null module handle
if "uint32_t MainImageBase32() {" not in s:
    s = s.replace("int Run32(const LoadedImage32& img) {", "uint32_t MainImageBase32() { return g_img32 ? g_img32->base : 0x400000; }\nint Run32(const LoadedImage32& img) {", 1)
p.write_text(s, encoding="utf-8")
print("ok")
