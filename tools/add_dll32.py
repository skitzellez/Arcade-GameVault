# one-off edit: 32-bit DLLs from the program's folder, export lookup, DllMain through a re-entrant Call32
from pathlib import Path

root = Path(__file__).resolve().parent.parent
p = root / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")

# stable references while the table grows during a call (GetProcAddress adds entries)
s = s.replace("static std::vector<Bridged> g_fn;", "static std::deque<Bridged> g_fn;   // a deque: entries stay put while it grows", 1)
s = s.replace("#include <vector>\n", "#include <vector>\n#include <deque>\n", 1)

# MapProgram32 -> MapImage32(file, out, name, isDll) + wrapper
s = s.replace("bool MapProgram32(const std::vector<uint8_t>& file, LoadedImage32& out) {",
              "static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll) {", 1)
s = s.replace("        Log(\"32-bit: no room below 4 GB for the program\");", "        Log(\"32-bit: no room below 2 GB for %s\", self.c_str());", 1)
s = s.replace("""                Bridged b{dll + "!" + fn, nullptr, 0, true};
                if (!byOrd) b.target = ResolveFor32(dll, fn, b.args, b.cdecl_);""",
              """                // the program's own DLLs (its folder): mapped too, the function's own address
                if (Mod32* m = LoadDll32(dll)) {
                    uint32_t at = Export32(*m, byOrd ? "" : fn, byOrd ? IMAGE_ORDINAL32(names->u1.Ordinal) : 0);
                    if (at) {
                        thunk->u1.Function = at;
                        bridged++;
                        continue;
                    }
                }
                Bridged b{dll + "!" + fn, nullptr, 0, true};
                if (!byOrd) b.target = ResolveFor32(dll, fn, b.args, b.cdecl_);""", 1)
s = s.replace("""    Log("32-bit program loaded at 0x%08x%s, %u bytes, entry +0x%x; imports: %d bridged, %d missing", out.base,
        moved ? " (moved)" : "", out.size, oh.AddressOfEntryPoint, bridged, missing);""",
              """    out.exportDir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size ? out.base + oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress : 0;
    out.exportSize = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    Log("32-bit %s %s loaded at 0x%08x%s, %u bytes, entry +0x%x; imports: %d bridged, %d missing", isDll ? "DLL" : "program",
        self.c_str(), out.base, moved ? " (moved)" : "", out.size, oh.AddressOfEntryPoint, bridged, missing);""", 1)
# wrapper after the function: find the end "    return true;\n}\n" following MapImage32
start = s.index("static bool MapImage32(")
end = s.index("    return true;\n}\n", start) + len("    return true;\n}\n")
s = s[:end] + """
bool MapProgram32(const std::vector<uint8_t>& file, LoadedImage32& out) { return MapImage32(file, out, "program", false); }
""" + s[end:]

dll_code = '''// --- the program's own 32-bit DLLs (TeknoParrot games bring theirs: libavs, libacio, d3dx9_42, ...) -----------------
struct Mod32 {
    std::string name;
    LoadedImage32 img;
    bool started = false;
};
static std::deque<Mod32> g_dlls;
static std::wstring g_folder32;   // the program's folder, ends with a backslash
void SetProgramFolder32(const std::wstring& folder) {
    g_folder32 = folder;
    if (!g_folder32.empty() && g_folder32.back() != L'\\\\') g_folder32 += L'\\\\';
}
static std::vector<uint8_t> ReadFile32(const std::wstring& path) {
    std::vector<uint8_t> v;
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
static Mod32* LoadDll32(const std::string& dll) {
    for (auto& m : g_dlls)
        if (m.name == dll) return &m;
    if (g_folder32.empty()) return nullptr;
    std::wstring path = g_folder32 + std::wstring(dll.begin(), dll.end());
    std::vector<uint8_t> bytes = ReadFile32(path);
    if (bytes.empty()) return nullptr;   // not in the folder: a system DLL (bridged)
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

'''
if "struct Mod32" not in s:
    # before MapImage32 (it uses LoadDll32/Export32); they use MapImage32 and MakeThunk32 -> forward declarations
    s = s.replace("static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll) {",
                  "static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll);\n"
                  + dll_code +
                  "static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll) {", 1)

call32 = '''// --- 64-bit code calling a 32-bit function (DllMain now; window procedures, threads and callbacks next): the
// arguments go on the 32-bit stack below where the 32-bit code is (or at its top), the return address is g_exit32;
// the bridge state is kept and put back, so it nests inside a bridged call
uint32_t Call32(uint32_t func, const uint32_t* args, int n) {
    State keep = *g_st;
    bool keepExit = g_exiting;
    uint32_t esp = keep.esp32 ? (keep.esp32 - 256) & ~15u : g_stackTop32;
    esp -= 4 * n;
    for (int i = 0; i < n; i++) ((uint32_t*)(uintptr_t)esp)[i] = args[i];
    esp -= 4;
    *(uint32_t*)(uintptr_t)esp = (uint32_t)(uintptr_t)g_exit32;
    g_exiting = false;
    typedef uint32_t (*enter_t)(uint64_t entry, uint64_t esp);
    uint32_t r = ((enter_t)g_enter64)(func, esp);
    *g_st = keep;
    g_exiting = keepExit;
    return r;
}
static void StartDlls32() {
    for (auto& m : g_dlls) {
        if (m.started || !m.img.entry || m.img.entry == m.img.base) continue;
        m.started = true;
        uint32_t a[3] = {m.img.base, 1 /*DLL_PROCESS_ATTACH*/, 0};
        uint32_t ok = Call32(m.img.entry, a, 3);
        Log("32-bit DLL %s started: %s", m.name.c_str(), ok ? "yes" : "it said no");
    }
}

'''
if "uint32_t Call32(" not in s:
    s = s.replace("int Run32(const LoadedImage32& img) {", call32 + "int Run32(const LoadedImage32& img) {", 1)
    s = s.replace("static State* g_st = nullptr;", "static State* g_st = nullptr;\nstatic uint32_t g_stackTop32 = 0;", 1)
    s = s.replace("    uint32_t esp = stackTop - 4;\n", "    g_stackTop32 = stackTop;\n    uint32_t esp = stackTop - 4;\n", 1)
    s = s.replace("    Log(\"32-bit entry point starts\");", "    g_st->esp32 = 0;\n    StartDlls32();\n    Log(\"32-bit entry point starts\");", 1)
p.write_text(s, encoding="utf-8")

h = root / "src" / "peload.h"
t = h.read_text(encoding="utf-8")
if "exportDir" not in t:
    t = t.replace("    uint32_t tlsDir = 0;   // its IMAGE_TLS_DIRECTORY32, 0 if none\n",
                  "    uint32_t tlsDir = 0;   // its IMAGE_TLS_DIRECTORY32, 0 if none\n    uint32_t exportDir = 0, exportSize = 0;\n", 1)
    t = t.replace("int Run32(const LoadedImage32& img);",
                  "int Run32(const LoadedImage32& img);\n// where the program's own 32-bit DLLs are (set before MapProgram32)\nvoid SetProgramFolder32(const std::wstring& folder);\n"
                  "// 64-bit code calling a 32-bit function (stdcall), during Run32\nuint32_t Call32(uint32_t func, const uint32_t* args, int n);", 1)
    h.write_text(t, encoding="utf-8")
print("ok")
