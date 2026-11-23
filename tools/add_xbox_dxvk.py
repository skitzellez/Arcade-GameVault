# one-off edit: Xbox app runs 64-bit DLLs (program folder + package x64 folder), DXVK settings, only.txt filter
from pathlib import Path

root = Path(__file__).resolve().parent.parent
p = root / "src" / "uwp_main.cpp"
s = p.read_text(encoding="utf-8")


def rep(a, b):
    global s
    assert a in s, a[:70]
    s = s.replace(a, b, 1)


rep('''void RunFolder(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;''', '''std::wstring g_only;   // LocalState\\only.txt: run only the programs whose file name has this text (testing)
std::wstring g_pkg;
void RunFolder(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;''')
rep('''        std::wstring path = dir + L"\\\\" + fd.cFileName;
        char name[MAX_PATH];''', '''        std::wstring path = dir + L"\\\\" + fd.cFileName;
        if (!g_only.empty()) {
            std::wstring low = fd.cFileName;
            for (auto& c : low) c = towlower(c);
            if (low.find(g_only) == std::wstring::npos) continue;
        }
        char name[MAX_PATH];''')
rep('''        arcade::SetProgramFolder32(dir);   // the program's own 32-bit DLLs are beside it''',
    '''        arcade::SetProgramFolder32(dir);   // the program's own 32-bit DLLs are beside it
        arcade::AddDllFolder64(dir);       // and its 64-bit ones; then ours (DXVK's d3d9.dll)
        arcade::AddDllFolder64(g_pkg + L"\\\\x64");''')
rep('''        std::wstring pkg = Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();''',
    '''        std::wstring pkg = Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
        g_pkg = pkg;
        {
            auto only = ReadAll(local + L"\\\\only.txt");
            for (uint8_t c : only)
                if (c > ' ') g_only += (wchar_t)towlower(c);
            if (!g_only.empty()) arcade::Log("only programs named *%ls* (only.txt)", g_only.c_str());
        }
        // for programs and DXVK: where to write (LocalState); DXVK's log and its pipeline cache go there
        SetEnvironmentVariableW(L"ARCADE_LOCAL", local.c_str());
        SetEnvironmentVariableW(L"DXVK_LOG_PATH", local.c_str());
        SetEnvironmentVariableW(L"DXVK_STATE_CACHE_PATH", local.c_str());
        SetEnvironmentVariableW(L"DXVK_LOG_LEVEL", L"info");''')
p.write_text(s, encoding="utf-8")

b = root / "build_xbox.bat"
t = b.read_text(encoding="utf-8")
if "d3d9_test" not in t:
    t = t.replace('copy /y out\\exc32.exe pkg\\programs\\ >nul\n', '''copy /y out\\exc32.exe pkg\\programs\\ >nul
copy /y out\\d3d9_test.exe pkg\\programs\\ >nul
rem DXVK (D3D9 on Vulkan, zlib licence) for the programs, mapped by the loader; Mesa's Vulkan-on-D3D12 driver (Dozen,
rem MIT) loaded as a packaged DLL, with the host-window hook (deps\\mesa-arcade, branch gv-arcade-dzn)
if not exist pkg\\x64 mkdir pkg\\x64
copy /y ..\\deps\\dxvk-src\\build64\\src\\d3d9\\d3d9.dll pkg\\x64\\ >nul || exit /b 1
copy /y ..\\deps\\mesa-arcade\\build-arcade-dzn\\src\\microsoft\\vulkan\\vulkan_dzn.dll pkg\\ >nul || exit /b 1
''', 1)
    b.write_text(t, encoding="utf-8")
print("ok")
