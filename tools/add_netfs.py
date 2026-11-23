# one-off edit: games played straight from the NAS (netfs.cpp) wired into the loader
from pathlib import Path

root = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader")
src = root / "src"
BS = chr(92)


def edit(name, pairs):
    p = src / name if not isinstance(name, Path) else name
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert s.count(a) == 1, (str(p), a[:70])
        s = s.replace(a, b)
    p.write_text(s, encoding="utf-8")


edit("peload.h", [("void SetProfilesFolder(", """// netfs.cpp: a game played straight from the NAS (its folder is \\\\gvnas\\game for the program)
bool NetFsMount(const std::wstring& exeUrl, const std::wstring& rootUrl, const std::wstring& credFile, const std::wstring& local,
                std::wstring& exeOut, std::string& error);
bool NetFsOn();
bool NetFsReadAll(const std::wstring& path, std::vector<uint8_t>& out);
bool NetFsHandle(void* h);
void NetFsAddRef(void* h);
uint64_t NetFsSize(void* h);
uint64_t NetFsPos(void* h);
size_t NetFsPread(void* h, uint64_t at, void* out, size_t n);
void* FindNetFsShim(const char* dll, const char* func);
void* FindIoShim(const char* dll, const char* func);
std::wstring PatchedFileFor(const std::wstring& path);   // arcade_io.cpp: a game's data file patched on the way in
void SetProfilesFolder(""")])

# the loader's own file readers: the share first
for name, fn in [("peload.cpp", "static std::vector<uint8_t> ReadAll64(const std::wstring& path) {\n    std::vector<uint8_t> v;\n"),
                 ("peload32.cpp", "static std::vector<uint8_t> ReadFile32(const std::wstring& path) {\n    std::vector<uint8_t> v;\n"),
                 ("uwp_main.cpp", "std::vector<uint8_t> ReadAll(const std::wstring& path) {\n    std::vector<uint8_t> v;\n")]:
    edit(name, [(fn, fn + "    if (arcade::NetFsReadAll(path, v)) return v;   // (a game played from the NAS)\n" if name == "uwp_main.cpp"
                 else fn + "    if (NetFsReadAll(path, v)) return v;   // (a game played from the NAS)\n")])

edit("arcade_io.cpp", [
    ("""static uint32_t Crc32File(const std::wstring& path) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }""", """static uint32_t Crc32File(const std::wstring& path) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    {   // a game played from the NAS
        std::vector<uint8_t> all;
        if (NetFsReadAll(path, all)) {
            uint32_t crc = 0xFFFFFFFF;
            for (uint8_t b : all) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }
    }"""),
    # the patched copy: its original may be on the share
    ("""        HANDLE in = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
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
        if (got != data.size()) return L"";""",
     """        std::vector<uint8_t> data;
        if (!NetFsReadAll(path, data)) {   // (the original on the NAS: read from there)
            HANDLE in = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            if (in == INVALID_HANDLE_VALUE) return L"";
            LARGE_INTEGER sz;
            GetFileSizeEx(in, &sz);
            if ((uint64_t)sz.QuadPart != pf.size) {
                CloseHandle(in);
                Log("%s: %ls is another version (%lld bytes), used as it is", pf.why, file.c_str(), (long long)sz.QuadPart);
                return L"";
            }
            data.resize((size_t)pf.size);
            DWORD n = 0;
            size_t got = 0;
            while (got < data.size() && ReadFile(in, data.data() + got, (DWORD)std::min<size_t>(data.size() - got, 1 << 24), &n, nullptr) && n)
                got += n;
            CloseHandle(in);
            if (got != data.size()) return L"";
        } else if (data.size() != pf.size) {
            Log("%s: %ls is another version (%llu bytes), used as it is", pf.why, file.c_str(), (unsigned long long)data.size());
            return L"";
        }"""),
    ("""static HANDLE WINAPI S_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {""",
     """std::wstring PatchedFileFor(const std::wstring& path) { return PatchedCopy(path); }
static HANDLE WINAPI S_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {"""),
])

# netfs: what is not on the share goes to the board stand-ins (JVS ports) when the game has them, then the system;
# a data file patched on the way in (Battle Pod's SWArcGame.upk) is opened from its patched copy
edit("netfs.cpp", [
    ("#include \"peload.h\"\n#include <windows.h>", "#include \"peload.h\"\n#include <winsock2.h>\n#include <windows.h>"),
    ("""typedef HANDLE(WINAPI* cfw_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
cfw_t RealCreateFileW() {
    static cfw_t f = (cfw_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW");
    return f;
}""", """typedef HANDLE(WINAPI* cfw_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
cfw_t RealCreateFileW() {   // the board stand-ins' (a JVS port) when there are some, else the system's
    static cfw_t f = [] {
        void* io = FindIoShim("kernel32.dll", "CreateFileW");
        return io ? (cfw_t)io : (cfw_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW");
    }();
    return f;
}"""),
    ("""    std::wstring ov = Overlay(rel);
    bool write = (acc & (GENERIC_WRITE""", """    if (!(acc & GENERIC_WRITE) && disp == OPEN_EXISTING) {
        std::wstring patched = PatchedFileFor(name);
        if (!patched.empty()) return RealCreateFileW()(patched.c_str(), acc, share, nullptr, disp, fl, nullptr);
    }
    std::wstring ov = Overlay(rel);
    bool write = (acc & (GENERIC_WRITE"""),
    ("""template <typename R, typename... A> R Real(const char* name, A... a) {
    static auto f = (R(WINAPI*)(A...))GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), name);
    return f(a...);
}""", """template <typename R, typename... A> R Real(const char* name, A... a) {
    // (ReadFile, CloseHandle: the board stand-ins' when the game has a JVS port, they pass the rest on)
    static auto f = [name] {
        void* io = FindIoShim("kernel32.dll", name);
        return io ? (R(WINAPI*)(A...))io : (R(WINAPI*)(A...))GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), name);
    }();
    return f(a...);
}"""),
    ("""static BOOL WINAPI N_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf(h);
    if (!f) return FALSE;""", """static BOOL WINAPI N_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    std::unique_lock<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf(h);
    if (!f) {
        l.unlock();
        typedef BOOL(WINAPI * rf_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
        static rf_t next = [] {
            void* io = FindIoShim("kernel32.dll", "ReadFile");
            return io ? (rf_t)io : (rf_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "ReadFile");
        }();
        return next(h, b, n, done, ov);
    }"""),
])

# FindShim: the share's stand-ins first (they pass on what is not theirs)
edit("shims.cpp", [("""    if (void* t = FindMemTallyShim(dll, func)) return t;   // (testing) memory counted by module""",
                    """    if (void* n = FindNetFsShim(dll, func)) return n;      // a game played from the NAS (netfs.cpp)
    if (void* t = FindMemTallyShim(dll, func)) return t;   // (testing) memory counted by module""")])

# 32-bit programs' CreateFile (peload32's own stand-ins): the share too
edit("peload32.cpp", [
    ("""    const char* n = (const char*)(uintptr_t)(uint32_t)name;
    HANDLE h = RealCreateFileA()(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);""",
     """    const char* n = (const char*)(uintptr_t)(uint32_t)name;
    if (NetFsOn() && n)
        if (void* nf = FindNetFsShim("kernel32.dll", "CreateFileA")) {   // a game played from the NAS
            HANDLE h = ((HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE))nf)(n, (DWORD)acc, (DWORD)share, nullptr,
                                                                                               (DWORD)disp, (DWORD)flags, nullptr);
            return h == INVALID_HANDLE_VALUE ? 0xFFFFFFFFull : (uint64_t)(uint32_t)(uintptr_t)h;
        }
    HANDLE h = RealCreateFileA()(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);"""),
    ("""    const wchar_t* n = (const wchar_t*)(uintptr_t)(uint32_t)name;
    HANDLE h = RealCreateFileW()(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);""",
     """    const wchar_t* n = (const wchar_t*)(uintptr_t)(uint32_t)name;
    if (NetFsOn() && n)
        if (void* nf = FindNetFsShim("kernel32.dll", "CreateFileW")) {   // a game played from the NAS
            HANDLE h = ((HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE))nf)(n, (DWORD)acc, (DWORD)share, nullptr,
                                                                                                (DWORD)disp, (DWORD)flags, nullptr);
            return h == INVALID_HANDLE_VALUE ? 0xFFFFFFFFull : (uint64_t)(uint32_t)(uintptr_t)h;
        }
    HANDLE h = RealCreateFileW()(n, (DWORD)acc, (DWORD)share, nullptr, (DWORD)disp, (DWORD)flags, nullptr);"""),
])

# the link: cmd=smb://... with root= and netcred=; the current folder through netfs
edit("uwp_main.cpp", [
    ("static std::wstring g_cmd, g_onExit;", "static std::wstring g_cmd, g_onExit, g_root, g_cred;"),
    ("""                        if (e.Name() == L"launchOnExit") g_onExit = e.Value().c_str();""",
     """                        if (e.Name() == L"launchOnExit") g_onExit = e.Value().c_str();
                        if (e.Name() == L"root") g_root = Unquote(e.Value().c_str());
                        if (e.Name() == L"netcred") g_cred = Unquote(e.Value().c_str());"""),
    ("""            if (!g_cmd.empty()) {   // Game Vault's game; then back to Game Vault
                std::wstring w = g_cmd;""", """            if (!g_cmd.empty()) {   // Game Vault's game; then back to Game Vault
                std::wstring w = g_cmd;
                if (!_wcsnicmp(w.c_str(), L"smb://", 6)) {   // played straight from the NAS (netfs.cpp)
                    std::wstring exe;
                    std::string err;
                    if (arcade::NetFsMount(w, g_root, g_cred, local, exe, err)) w = exe;
                    else arcade::Log("NAS: %s", err.c_str());
                }"""),
    ("""    SetCurrentDirectoryW(dir.c_str());
    arcade::SetProfilesFolder""", """    if (void* cd = arcade::FindNetFsShim("kernel32.dll", "SetCurrentDirectoryW"))   // (a game on the NAS: its folder)
        ((BOOL(WINAPI*)(LPCWSTR))cd)(dir.c_str());
    else
        SetCurrentDirectoryW(dir.c_str());
    arcade::SetProfilesFolder"""),
])

# lazy sound-bank reads (filebacked.cpp) on a file from the NAS: read through netfs
edit("filebacked.cpp", [
    ("""static DWORD ReadAt(HANDLE f, uint64_t from, void* to, DWORD len) {""",
     """static DWORD ReadAt(HANDLE f, uint64_t from, void* to, DWORD len) {
    if (NetFsHandle(f)) return (DWORD)NetFsPread(f, from, to, len);   // a file on the NAS"""),
    ("""    LARGE_INTEGER at = {}, fileSize = {};
    if (ov) at.QuadPart = ((LONGLONG)ov->OffsetHigh << 32) | ov->Offset;
    else if (!SetFilePointerEx(h, at, &at, FILE_CURRENT)) return false;
    if (!GetFileSizeEx(h, &fileSize) || at.QuadPart >= fileSize.QuadPart) return false;""",
     """    LARGE_INTEGER at = {}, fileSize = {};
    bool net = NetFsHandle(h);   // (a file on the NAS: its place and size from netfs)
    if (ov) at.QuadPart = ((LONGLONG)ov->OffsetHigh << 32) | ov->Offset;
    else if (net) at.QuadPart = (LONGLONG)NetFsPos(h);
    else if (!SetFilePointerEx(h, at, &at, FILE_CURRENT)) return false;
    if (net) fileSize.QuadPart = (LONGLONG)NetFsSize(h);
    else if (!GetFileSizeEx(h, &fileSize)) return false;
    if (at.QuadPart >= fileSize.QuadPart) return false;"""),
    ("""        HANDLE ours = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &ours, 0, FALSE, DUPLICATE_SAME_ACCESS)) {""",
     """        HANDLE ours = nullptr;
        if (net) {   // our own reference to the NAS file
            NetFsAddRef(h);
            ours = h;
        } else if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &ours, 0, FALSE, DUPLICATE_SAME_ACCESS)) {"""),
    ("""    if (!ov) {
        LARGE_INTEGER step;
        step.QuadPart = len;
        SetFilePointerEx(h, step, nullptr, FILE_CURRENT);
    } else {
        ov->Internal = 0;
        ov->InternalHigh = len;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    if (done) *done = len;
    static int told = 0;""", """    if (!ov) {
        LARGE_INTEGER step;
        step.QuadPart = len;
        if (net) {   // (the NAS file's place: through its stand-in)
            typedef BOOL(WINAPI * sfp_t)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
            if (void* s = FindNetFsShim("kernel32.dll", "SetFilePointerEx")) ((sfp_t)s)(h, step, nullptr, FILE_CURRENT);
        } else {
            SetFilePointerEx(h, step, nullptr, FILE_CURRENT);
        }
    } else {
        ov->Internal = 0;
        ov->InternalHigh = len;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    if (done) *done = len;
    static int told = 0;"""),
])

# arcade_io's ReadFile stand-in: a NAS file's lazy read, else netfs, else the system
edit("arcade_io.cpp", [("""        BigPrepare(b, n);
""", """        BigPrepare(b, n);
        if (NetFsHandle(h)) {   // a file on the NAS (netfs.cpp)
            if (void* r = FindNetFsShim("kernel32.dll", "ReadFile"))
                return ((BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED))r)(h, b, n, done, ov);
        }
""")])

for bat in ["build_xbox.bat", "build_pc.bat"]:
    edit(root / bat, [("src" + BS + "gvaudio.cpp", "src" + BS + "gvaudio.cpp src" + BS + "netfs.cpp")])
print("ok")
