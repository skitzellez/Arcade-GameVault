# one-off edit: timeGetTime/timeBeginPeriod, SHGetFolderPath (-> a per-game folder), CoInitialize stand-ins; memory
# use logged every 5 s on the Xbox
from pathlib import Path

root = Path(__file__).resolve().parent.parent


def edit(rel, pairs):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert a in s, (rel, a[:60])
        s = s.replace(a, b, 1)
    p.write_text(s, encoding="utf-8")


edit("src/shims.cpp", [
    ('// shcore: DPI', r'''// winmm: the game clock (a missing-function stub answering 0 stops time)
static DWORD WINAPI S_timeGetTime() { return (DWORD)GetTickCount64(); }
static UINT WINAPI S_timePeriod(UINT) { return 0; }   // TIMERR_NOERROR
// shell32: special folders (Documents, AppData) -> a "UserData" folder beside the program, made on first use
static std::wstring UserDataFolder() {
    std::wstring d = g_path.substr(0, g_path.find_last_of(L"\\/") + 1) + L"UserData";
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    CreateDirectoryFromAppW(d.c_str(), nullptr);
#else
    CreateDirectoryW(d.c_str(), nullptr);
#endif
    return d;
}
static HRESULT WINAPI S_SHGetFolderPathW(HWND, int csidl, HANDLE, DWORD, LPWSTR out) {
    std::wstring d = UserDataFolder();
    wcsncpy_s(out, MAX_PATH, d.c_str(), _TRUNCATE);
    static bool said = false;
    if (!said) Log("special folder %d -> %s", csidl, Narrow(d.c_str()).c_str());
    said = true;
    return S_OK;
}
static HRESULT WINAPI S_SHGetFolderPathA(HWND, int, HANDLE, DWORD, LPSTR out) {
    std::string d = Narrow(UserDataFolder().c_str());
    strncpy_s(out, MAX_PATH, d.c_str(), _TRUNCATE);
    return S_OK;
}
// ole32: CoInitialize is CoInitializeEx in an app
static HRESULT WINAPI S_CoInitialize(void*) { return CoInitializeEx(nullptr, COINIT_MULTITHREADED); }

// shcore: DPI'''),
    ('''    {"shcore.dll", "SetProcessDpiAwareness", (void*)S_SetProcessDpiAwareness},''',
     '''    {"shcore.dll", "SetProcessDpiAwareness", (void*)S_SetProcessDpiAwareness},
    {"winmm.dll", "timeGetTime", (void*)S_timeGetTime}, {"winmm.dll", "timeBeginPeriod", (void*)S_timePeriod},
    {"winmm.dll", "timeEndPeriod", (void*)S_timePeriod}, {"shell32.dll", "SHGetFolderPathW", (void*)S_SHGetFolderPathW},
    {"shell32.dll", "SHGetFolderPathA", (void*)S_SHGetFolderPathA}, {"ole32.dll", "CoInitialize", (void*)S_CoInitialize},'''),
])
edit("src/uwp_main.cpp", [
    ('''        arcade::Log("Game Vault arcade loader test");''', '''        arcade::Log("Game Vault arcade loader test");
        std::thread([] {   // (testing) memory use every 5 s while a game runs: the app's limit covers graphics too
            uint64_t peak = 0;
            for (;;) {
                Sleep(5000);
                uint64_t now = Windows::System::MemoryManager::AppMemoryUsage();
                if (now > peak) peak = now;
                arcade::Log("memory: %llu MB in use, peak %llu MB", (unsigned long long)(now >> 20), (unsigned long long)(peak >> 20));
            }
        }).detach();'''),
])
print("ok")
