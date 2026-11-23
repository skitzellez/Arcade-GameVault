# one-off edit: the cabinet's game drive D:\ is the game's folder for profiles that say so (Battle Gear 4: D:\config.bin)
from pathlib import Path

src = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src")


def edit(name, pairs):
    p = src / name
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert s.count(a) == 1, (name, a[:60])
        s = s.replace(a, b)
    p.write_text(s, encoding="utf-8")


edit("arcade_io.cpp", [
    ("    bool battleGear = false;", "    bool battleGear = false;\n    bool driveD = false;       // the cabinet's game drive D:\\ is the game's folder (Taito Type X+: D:\\config.bin)"),
    ('L"BattleGear4Tuned", true, true},', 'L"BattleGear4Tuned", true, true, true},'),
    ("void ChooseProfile(const std::wstring& exePath) {\n",
     r'''static std::string g_gameDirA;   // the game's folder, ends with a backslash
// "D:\config.bin" -> "<game folder>\config.bin" for games whose profile says the cabinet drive is theirs
bool RedirectCabinetPath(const char* in, std::string& out) {
    if (!g_profile || !g_profile->driveD || !in || g_gameDirA.empty()) return false;
    if (!((in[0] == 'd' || in[0] == 'D') && in[1] == ':' && (in[2] == '\\' || in[2] == '/'))) return false;
    out = g_gameDirA + (in + 3);
    static int told = 0;
    if (told++ < 5) Log("cabinet drive: %s -> %s", in, out.c_str());
    return true;
}
void ChooseProfile(const std::wstring& exePath) {
    {
        std::wstring d = exePath.substr(0, exePath.find_last_of(L"\\/") + 1);
        g_gameDirA.clear();
        for (wchar_t c : d) g_gameDirA += (char)c;
    }
'''),
])
edit("peload32.cpp", [
    ("static uint64_t B_CreateFileA(uint64_t name, uint64_t acc, uint64_t share, uint64_t, uint64_t disp, uint64_t flags, uint64_t) {\n    const char* n = (const char*)(uintptr_t)(uint32_t)name;\n",
     "bool RedirectCabinetPath(const char* in, std::string& out);   // arcade_io.cpp\n"
     "static uint64_t B_CreateFileA(uint64_t name, uint64_t acc, uint64_t share, uint64_t, uint64_t disp, uint64_t flags, uint64_t) {\n"
     "    const char* n = (const char*)(uintptr_t)(uint32_t)name;\n"
     "    std::string cab;\n"
     "    if (RedirectCabinetPath(n, cab)) n = cab.c_str();\n"),
    ("static uint64_t B_CreateFileW(uint64_t name, uint64_t acc, uint64_t share, uint64_t, uint64_t disp, uint64_t flags, uint64_t) {\n    const wchar_t* n = (const wchar_t*)(uintptr_t)(uint32_t)name;\n",
     "static uint64_t B_CreateFileW(uint64_t name, uint64_t acc, uint64_t share, uint64_t, uint64_t disp, uint64_t flags, uint64_t) {\n"
     "    const wchar_t* n = (const wchar_t*)(uintptr_t)(uint32_t)name;\n"
     "    std::wstring cabW;\n"
     "    if (n) {\n"
     "        std::string a, cab;\n"
     "        for (const wchar_t* c = n; *c; c++) a += (char)*c;\n"
     "        if (RedirectCabinetPath(a.c_str(), cab)) cabW.assign(cab.begin(), cab.end()), n = cabW.c_str();\n"
     "    }\n"),
])
print("ok")
