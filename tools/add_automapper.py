# one-off edit: the auto mapper (controls.cpp) wired into the JVS emulator and the game profiles
from pathlib import Path
src = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src")
BS = chr(92)
def edit(name, pairs):
    p = src / name
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert s.count(a) == 1, (name, a[:60])
        s = s.replace(a, b)
    p.write_text(s, encoding="utf-8")
edit("peload.h", [("void SetGameScreen", """struct PadIn {   // one Xbox pad, XInput-style (controls.cpp)
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
    bool service;
};
bool ControlsLoad(const std::wstring& teknoParrotProfileXml);   // controls.cpp: the auto mapper
bool ControlsOn();
void ControlsSwitches(const PadIn& p, int players, int per, std::vector<uint8_t>& reply);
void ControlsAnalog(const PadIn& p, int channels, std::vector<uint8_t>& reply);
void SetProfilesFolder(const std::wstring& dir);   // arcade_io.cpp: TeknoParrot game profiles shipped with us
void SetGameScreen""")])
edit("arcade_io.cpp", [
("""    // pad layout: 0 = generic (A/RT button 1, B/LT button 2, sticks on channels 0-3, triggers 4-5);
    // 1 = Star Wars Battle Pod (TeknoParrot's NamcoMachStorm layout, below)
    int layout = 0;""", """    const wchar_t* tp = nullptr;   // its TeknoParrot game profile (tpprofiles/<name>.xml): the controls (controls.cpp)"""),
("""     1280, 720, true, 1},""", """     1280, 720, true, L"StarWars"},"""),
("""     0x13, 0x18, false, false, 0x09616171, true},""", """     0x13, 0x18, false, false, 0x09616171, true, 0, 0, false, L"ChaosBreaker"},"""),
("""static const Profile* g_profile = nullptr;
""", """static const Profile* g_profile = nullptr;
static std::wstring g_profilesDir;
void SetProfilesFolder(const std::wstring& dir) { g_profilesDir = dir; }
"""),
("""    if (g_profile)
        Log("game profile: %s (JVS %s, dongle %s)",""", """    {   // controls: a TeknoParrot profile beside the game (teknoparrot.xml) first, else the one we ship for it
        std::wstring dir = exePath.substr(0, exePath.find_last_of(L"BSBS/") + 1);
        if (!ControlsLoad(dir + L"teknoparrot.xml") && g_profile && g_profile->tp && !g_profilesDir.empty())
            if (!ControlsLoad(g_profilesDir + L"BSBS" + g_profile->tp + L".xml"))
                Log("controls: no TeknoParrot profile %ls in %ls", g_profile->tp, g_profilesDir.c_str());
    }
    if (g_profile)
        Log("game profile: %s (JVS %s, dongle %s)",""".replace("BS", BS)),
("""    r.push_back(test ? 0x80 : 0x00);
    for (int pl = 0; pl < players; pl++) {""", """    r.push_back(test ? 0x80 : 0x00);
    if (ControlsOn()) {   // the game's TeknoParrot profile, mapped (controls.cpp)
        PadIn in = {b, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry, service};
        ControlsSwitches(in, players, per, r);
        return;
    }
    for (int pl = 0; pl < players; pl++) {"""),
("""    if (g_profile && g_profile->layout == 1) {   // Battle Pod: 1 throttle (LT), 2 stick X, 3 stick Y (reversed)
        const uint8_t bp[8] = {0x80, p.lt, Stick8(p.lx, false), Stick8(p.ly, true), 0x80, 0x80, 0x80, 0x80};
        memcpy(v, bp, sizeof v);
    }""", """    if (ControlsOn()) {
        PadIn in = {p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry, false};
        ControlsAnalog(in, channels, r);
        return;
    }"""),
])
s = (src / "arcade_io.cpp").read_text(encoding="utf-8")
a = s.index("        if (pl == 0 && g_profile && g_profile->layout == 1) {")
b = s.index("        if (pl == 0 && GetEnvironmentVariableA(\"ARCADE_JVS_PROBE\"")
s = s[:a] + s[b:]
(src / "arcade_io.cpp").write_text(s, encoding="utf-8")
print("ok")
