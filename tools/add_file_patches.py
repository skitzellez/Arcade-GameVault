# one-off edit: game data files patched on the way in (arcade_io.cpp). Star Wars Battle Pod's SWArcGame.upk gets the
# flat view (tools/battlepod_flat.py's three changes) in a copy in the loader's own storage, made once; the game's
# files stay as they are.
from pathlib import Path
p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\arcade_io.cpp")
s = p.read_text(encoding="utf-8")
BS = chr(92)
a = """static HANDLE WINAPI S_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    char a[MAX_PATH] = "";
    if (name) WideCharToMultiByte(CP_ACP, 0, name, -1, a, MAX_PATH, nullptr, nullptr);
    if (IsJvsName(a)) {
        Log("JVS: the game opened %s (emulated I/O board)", a);
        return kJvsHandle;
    }"""
b = """// --- data files patched on the way in: a copy in the loader's own storage (LocalState\\patched), made once, opened
// instead of the game's file (which stays as it is). Each change is made only where the original bytes are there.
struct FilePatch {
    uint64_t at;
    const char* oldHex;
    const char* newHex;
};
struct PatchedFile {
    const wchar_t* exe;     // the game
    const wchar_t* file;    // its data file's name
    uint64_t size;          // that file's size (only this version)
    const char* why;
    FilePatch p[4];
};
static const PatchedFile kPatchedFiles[] = {
    // Star Wars Battle Pod, flat view on a TV (tools/battlepod_flat.py): SWArcDomeGame.PostBeginPlay's dome view
    // and dome back buffer off, the default back buffer 1280x720 (decompressed SWArcGame.upk, 39317014 bytes)
    {L"SWArcGame-Win64-Shipping.exe", L"SWArcGame.upk", 39317014, "Battle Pod flat view",
     {{0xD4CD7, "02", "00"}, {0xD4CF2, "27", "28"}, {0xD5B56, "00005c450080f744", "0000a04400003444"}, {0, nullptr, nullptr}}},
};
static std::wstring PatchedCopy(const std::wstring& path) {
    size_t k = path.find_last_of(L"BSBS/");
    std::wstring file = path.substr(k == std::wstring::npos ? 0 : k + 1);
    for (const PatchedFile& pf : kPatchedFiles) {
        if (_wcsicmp(pf.exe, g_exeName.c_str()) || _wcsicmp(pf.file, file.c_str())) continue;
        wchar_t local[MAX_PATH] = L"";
        if (!GetEnvironmentVariableW(L"ARCADE_LOCAL", local, MAX_PATH)) return L"";
        std::wstring dir = std::wstring(local) + L"BSBSpatched", out = dir + L"BSBS" + file;
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (GetFileAttributesExFromAppW(out.c_str(), GetFileExInfoStandard, &ad) && ad.nFileSizeLow == (DWORD)pf.size) return out;
        HANDLE in = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (in == INVALID_HANDLE_VALUE) return L"";
        LARGE_INTEGER sz;
        GetFileSizeEx(in, &sz);
        if ((uint64_t)sz.QuadPart != pf.size) {
            CloseHandle(in);
            Log("%s: %ls is another version (%lld bytes), used as it is", pf.why, file.c_str(), (long long)sz.QuadPart);
            return L"";
        }
        std::vector<uint8_t> data((size_t)pf.size);
        DWORD n = 0;
        size_t got = 0;
        while (got < data.size() && ReadFile(in, data.data() + got, (DWORD)std::min<size_t>(data.size() - got, 1 << 24), &n, nullptr) && n)
            got += n;
        CloseHandle(in);
        if (got != data.size()) return L"";
        for (const FilePatch& fp : pf.p) {
            if (!fp.oldHex) break;
            size_t len = strlen(fp.oldHex) / 2;
            for (size_t i = 0; i < len; i++) {
                unsigned o, nw;
                sscanf_s(fp.oldHex + i * 2, "%2x", &o);
                sscanf_s(fp.newHex + i * 2, "%2x", &nw);
                if (data[fp.at + i] != (uint8_t)o) {
                    Log("%s: %ls not as expected at 0x%llx, used as it is", pf.why, file.c_str(), (unsigned long long)fp.at);
                    return L"";
                }
                data[fp.at + i] = (uint8_t)nw;
            }
        }
        CreateDirectoryFromAppW(dir.c_str(), nullptr);
        HANDLE o = CreateFile2FromAppW(out.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (o == INVALID_HANDLE_VALUE) return L"";
        DWORD w = 0;
        size_t put = 0;
        while (put < data.size() && WriteFile(o, data.data() + put, (DWORD)std::min<size_t>(data.size() - put, 1 << 24), &w, nullptr) && w)
            put += w;
        CloseHandle(o);
        Log("%s: patched copy of %ls made (%s)", pf.why, file.c_str(), put == data.size() ? "ok" : "FAILED");
        return put == data.size() ? out : L"";
    }
    return L"";
}
static HANDLE WINAPI S_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    char a[MAX_PATH] = "";
    if (name) WideCharToMultiByte(CP_ACP, 0, name, -1, a, MAX_PATH, nullptr, nullptr);
    if (IsJvsName(a)) {
        Log("JVS: the game opened %s (emulated I/O board)", a);
        return kJvsHandle;
    }
    if (name && disp == OPEN_EXISTING && !(acc & GENERIC_WRITE)) {
        std::wstring patched = PatchedCopy(name);
        if (!patched.empty()) name = patched.c_str(), a[0] = 0;
        if (!patched.empty())
            return Real<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>("CreateFileW")(
                name, acc, share, sa, disp, fl, t);
    }""".replace("BSBS", BS + BS)
assert s.count(a) == 1
s = s.replace(a, b)
a = """void ChooseProfile(const std::wstring& exePath) {
    std::wstring name = exePath.substr(exePath.find_last_of(L"BSBS/") + 1);""".replace("BSBS", BS + BS)
assert s.count(a) == 1, "choose"
s = s.replace(a, a + "\n    g_exeName = name;")
p.write_text(s, encoding="utf-8")
print("ok")
