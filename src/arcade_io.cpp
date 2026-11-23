// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// ARCADE HARDWARE STAND-INS, per game (a "profile", chosen from the program's file name before it is mapped):
//   - JVS I/O board on a serial port (COM3 for Namco ES3): the game's CreateFile/WriteFile/ReadFile and comm calls on
//     that port reach a JVS emulator here. Ported from TeknoParrotUI's JvsPackageEmulator/JvsHelper/SerialPortHandler
//     (GPL-3.0, Reaver/TeknoGods) - only the parts the supported games use.
//   - Sentinel HASP dongle: hasp_login/read/decrypt/... answered with the dongle data OpenParrot gives (GPL-3.0,
//     OpenParrot StarWars.cpp).
//   - XInput: the cabinet's controls show as a connected pad for games that check for one.
// Input from the player (Xbox pad -> JVS switches/analog) is still to come: everything reads as "not pressed".

#include "peload.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace arcade {
HANDLE RingEdgeOpen(const char* name);   // ringedge.cpp
bool RingEdgeHandle(HANDLE h);
bool RingEdgeIoctl(HANDLE h, DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize, DWORD* ret);
bool RingEdgeRead(HANDLE h, void* b, DWORD n, DWORD* done);
bool RingEdgeWrite(HANDLE h, const void* b, DWORD n, DWORD* done);
bool RingEdgeSeek(HANDLE h, int64_t to, DWORD how, int64_t* now);
void RingEdgeStart(const std::wstring& gameDir, const char* gameId);

// --- profiles ---------------------------------------------------------------------------------------------------
struct Profile {
    const wchar_t* exe;        // program file name
    const char* name;
    const char* jvsPort;       // "COM3"; null: no JVS
    bool namco;                // Namco custom JVS commands (0x70..)
    const char* jvsId;         // reply to "identify"
    uint8_t jvsVersion, commVersion, cmdRevision, switches;
    bool hasp;                 // HASP dongle stand-in
    bool padAlways;            // XInputGetState: always a connected pad
    uint32_t crc;              // CRC-32 of the exe (0: any), for exes with a common name (game.exe)
    bool taitoStick;           // Taito Type X stick panel (TeknoParrot "TaitoStick": 2 players x 2 switch bytes)
    int screenW = 0, screenH = 0;
    bool fileBacked = false;
    const wchar_t* tp = nullptr;   // its TeknoParrot game profile (tpprofiles/<name>.xml): the controls (controls.cpp)   // big file reads mapped from the file (filebacked.cpp), for games that load whole files   // the screen size the game is told (0: the TV's); the picture is stretched to the TV
    bool typeXSense = false;   // Taito Type X sense line (0x10 only once addressed) without the stick panel layout
    bool battleGear = false;
    bool driveD = false;
    bool ringEdge = false;     // Sega RingEdge: its platform services and drivers (ringedge.cpp)       // the cabinet's game drive D:\ is the game's folder (Taito Type X+: D:\config.bin)   // TeknoParrot's TaitoBattleGear board: its feature list and analog layout
};
static const Profile kProfiles[] = {
    // Star Wars Battle Pod (Namco ES3X, 2014): NA-JV board on COM3, HASP HL dongle
    {L"SWArcGame-Win64-Shipping.exe", "Star Wars Battle Pod", "COM3", true,
     "namco ltd.;NA-JV;Ver4.00;JPN,Multipurpose.", 0x31, 0x31, 0x31, 0x18, true, true, 0, false,
     1280, 720, true, L"StarWars"},   // the flat-view patch (tools/battlepod_flat.py) draws 1280x720; less memory than the TV's size
    // Chaos Breaker (Taito Type X, 2004): JVS on COM2, TeknoParrot's TaitoTypeXGeneric settings
    {L"game.exe", "Chaos Breaker", "COM2", false, "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10", 0x30, 0x10,
     0x13, 0x18, false, false, 0x09616171, true, 0, 0, false, L"ChaosBreaker"},
    // Battle Fantasia (Taito Type X, 2007): JVS on COM2, TeknoParrot's TaitoTypeXGeneric settings, as Chaos Breaker
    {L"game.exe", "Battle Fantasia", "COM2", false, "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10", 0x30, 0x10,
     0x13, 0x18, false, false, 0xc38fc27d, true, 0, 0, false, L"BattleFantasia"},
    // Battle Gear 4 Tuned (Taito Type X+, 2007): JVS on COM2 (wheel, pedals, shifter: TeknoParrot's BattleGear4Tuned)
    {L"game.exe", "Battle Gear 4 Tuned", "COM2", false, "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10", 0x30, 0x10,
     0x13, 0x18, false, false, 0x93c2bf69, false, 800, 600, false, L"BattleGear4Tuned", true, true, true},   // (draws 800x600)   // (Type X sense line, Battle Gear board)
    // Caladrius AC (Sega RingEdge 2, 2013): amLib's services by PCP, its drivers; JVS board through \\.\mxjvs
    {L"SILVER_AMP_R_ROM.exe", "Caladrius AC", "mxjvs", false, "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-14572;Ver1.00;2005/10", 0x30, 0x10,
     0x13, 0x18, false, false, 0, false, 0, 0, false, L"CaladriusAC", false, false, false, true},
    // Ford Racing Full Blown (Sega Europa-R, 2006): USB HID board, patched in europa.cpp (OpenParrot)
    {L"fordracing.exe", "Ford Racing Full Blown", nullptr, false, nullptr, 0, 0, 0, 0, false, false, 0, false, 0, 0, false, L"FR"},
};
static uint32_t Crc32File(const std::wstring& path) {
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
    }
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
#else
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
#endif
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint32_t crc = 0xFFFFFFFF;
    static uint8_t buf[65536];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n)
        for (DWORD i = 0; i < n; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    CloseHandle(h);
    return ~crc;
}
static const Profile* g_profile = nullptr;
static std::wstring g_exeName;   // the game's program (for its patched data files)
static std::wstring g_profilesDir;
void SetProfilesFolder(const std::wstring& dir) { g_profilesDir = dir; }
// byte patches a game gets after it is mapped (only where the original bytes are there)
struct Patch {
    const wchar_t* exe;
    uint32_t rva;
    const char* oldHex;
    const char* newHex;
    const char* why;
};
static const Patch kPatches[] = {
    // Battle Pod: the cabinet's volume setting (ASWArcArcadeIO virtual 0x720) -> 12 of 15 (0 = silent before)
    {L"SWArcGame-Win64-Shipping.exe", 0x8bbf54, "488b03488bcbff9020070000", "b80c000000909090909090 90",
     "volume 12 of 15"},
};
static int Hex(const char* h, uint8_t* out) {
    int n = 0;
    for (; *h; h++) {
        if (*h == ' ') continue;
        unsigned v;
        sscanf_s(h, "%2x", &v);
        out[n++] = (uint8_t)v;
        h++;
    }
    return n;
}
void ApplyProfilePatches(uint8_t* base, size_t size, const std::wstring& exePath) {
    std::wstring name = exePath.substr(exePath.find_last_of(L"\\/") + 1);
    for (const Patch& p : kPatches) {
        if (_wcsicmp(p.exe, name.c_str())) continue;
        uint8_t o[64], n[64];
        int a = Hex(p.oldHex, o), b = Hex(p.newHex, n);
        if (a != b || p.rva + a > size || memcmp(base + p.rva, o, a)) {
            Log("patch %s: original bytes not there, skipped", p.why);
            continue;
        }
        DWORD old;
        VirtualProtect(base + p.rva, a, PAGE_EXECUTE_READWRITE, &old);
        memcpy(base + p.rva, n, a);
        VirtualProtect(base + p.rva, a, old, &old);
        Log("patch: %s", p.why);
    }
}

static std::string g_gameDirA;   // the game's folder, ends with a backslash
// "D:\config.bin" -> "<game folder>\config.bin" for games whose profile says the cabinet drive is theirs
// NESiCAxLive's countdown picture (NxLCountDownRenderer_SD.tga / _HD...): the NESiCA client's own file, which some
// dumps lack (Dragon Dance read past the failed load and crashed): a blank, see-through stand-in of ours instead
static bool NesicaCountdown(const char* in, std::string& out) {
    const char* f = in;
    for (const char* k = in; *k; k++)
        if (*k == '\\' || *k == '/') f = k + 1;
    if (_strnicmp(f, "NxLCountDownRenderer", 20)) return false;
    size_t n = strlen(f);
    if (n < 4 || _stricmp(f + n - 4, ".tga")) return false;
    std::string here = (in[0] && in[1] == ':') || in[0] == '\\' ? std::string(in) : g_gameDirA + in;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(here.c_str(), GetFileExInfoStandard, &fa)) return false;   // the dump has it
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    out = std::string(tmp) + "gv_" + f;
    if (!GetFileAttributesExA(out.c_str(), GetFileExInfoStandard, &fa)) {
        const int w = 256, h = 64;
        std::vector<uint8_t> t(18 + w * h * 4, 0);
        t[2] = 2;   // uncompressed true colour
        t[12] = (uint8_t)w, t[13] = (uint8_t)(w >> 8), t[14] = (uint8_t)h, t[15] = (uint8_t)(h >> 8);
        t[16] = 32, t[17] = 0x28;   // 32 bits, 8 of them alpha, rows from the top
        std::wstring ow(out.begin(), out.end());
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
        HANDLE fh = CreateFile2(ow.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
#else
        HANDLE fh = CreateFileW(ow.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
#endif
        if (fh == INVALID_HANDLE_VALUE) return false;
        DWORD done = 0;
        WriteFile(fh, t.data(), (DWORD)t.size(), &done, nullptr);
        CloseHandle(fh);
    }
    Log("NESiCA countdown picture %s: not in the game, a blank one of ours (%s)", f, out.c_str());
    return true;
}
bool RedirectCabinetPath(const char* in, std::string& out) {
    if (in && NesicaCountdown(in, out)) return true;
    if (!g_profile || !g_profile->driveD || !in || g_gameDirA.empty()) return false;
    if (!((in[0] == 'd' || in[0] == 'D' || in[0] == 'e' || in[0] == 'E') && in[1] == ':' && (in[2] == '\\' || in[2] == '/'))) return false;
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
    std::wstring name = exePath.substr(exePath.find_last_of(L"\\/") + 1);
    g_exeName = name;
    g_profile = nullptr;
    uint32_t crc = 0;
    for (const Profile& p : kProfiles) {
        if (_wcsicmp(p.exe, name.c_str())) continue;
        if (p.crc) {
            if (!crc) crc = Crc32File(exePath);
            if (crc != p.crc) continue;
        }
        g_profile = &p;
    }
    if (g_profile && g_profile->screenW) {
        SetGameScreen(g_profile->screenW, g_profile->screenH);
        Log("game screen: %dx%d (stretched to the TV)", g_profile->screenW, g_profile->screenH);
    }
    if (g_profile && g_profile->fileBacked) {
        SetFileBacked(true);
        Log("big file reads: mapped from the files");
    }
    {   // controls: a TeknoParrot profile beside the game (teknoparrot.xml) first, else the one we ship for it
        std::wstring dir = exePath.substr(0, exePath.find_last_of(L"\\/") + 1);
        if (!ControlsLoad(dir + L"teknoparrot.xml") && g_profile && g_profile->tp && !g_profilesDir.empty())
            if (!ControlsLoad(g_profilesDir + L"\\" + g_profile->tp + L".xml"))
                Log("controls: no TeknoParrot profile %ls in %ls", g_profile->tp, g_profilesDir.c_str());
    }
    if (g_profile && g_profile->ringEdge) RingEdgeStart(exePath.substr(0, exePath.find_last_of(L"\\/") + 1), "SBWU");
    if (g_profile)
        Log("game profile: %s (JVS %s, dongle %s)", g_profile->name, g_profile->jvsPort ? g_profile->jvsPort : "none",
            g_profile->hasp ? "stand-in" : "none");
}

// --- JVS emulator --------------------------------------------------------------------------------------------------
static std::mutex g_jvsLock;
static std::deque<uint8_t> g_jvsIn;    // unescaped bytes the game wrote
static std::deque<uint8_t> g_jvsOut;   // escaped reply bytes for the game to read
static bool g_jvsEscape = false;
static bool g_jvsAddressed = false;
static int g_coins[2] = {0, 0};
static const HANDLE kJvsHandle = (HANDLE)(uintptr_t)0x0A5C3A00;
// Battle Gear 4's steering drive board (COM1): commands of 2 bytes, each answered by 2. 0x20 reset -> A0 00 (position
// no longer reported), 0x11 -> the position from then on for every command, 0x1F motor off -> 1F 00 while not reported,
// others -> 8C A0 while not; position = steering 0..255 as 1024..2044, big-endian; 0x0D = the motor's torque
static const HANDLE kWheelHandle = (HANDLE)(uintptr_t)0x0A5C3B00;
static bool IsOurs(HANDLE h) { return h == kJvsHandle || h == kWheelHandle; }
static std::mutex g_wheelLock;
static std::deque<uint8_t> g_wheelOut;
static uint8_t g_wheelCmd = 0;
static bool g_wheelHalf = false, g_wheelReport = false;
static int WheelSteering();   // 0..255 from the pad (below)
static void Bg4TunedTick();   // (below)
static void WheelCommand(uint8_t cmd, uint8_t arg) {
    static int told = 0;
    if (told++ < 20) Log("steering board: command %02x %02x", cmd, arg);
    auto answer = [](uint8_t a, uint8_t b) { g_wheelOut.push_back(a), g_wheelOut.push_back(b); };
    if (cmd == 0x20) {
        g_wheelReport = false;
        answer(0xA0, 0x00);
        return;
    }
    if (cmd == 0x11) g_wheelReport = true;
    if (g_wheelReport) {
        int v = WheelSteering() * 4 + 1024;
        answer((uint8_t)(v >> 8), (uint8_t)v);
    } else if (cmd == 0x1F) {
        answer(0x1F, 0x00);
    } else {
        answer(0x8C, 0xA0);
    }
}


static void JvsEncode(const std::vector<uint8_t>& pkg) {
    g_jvsOut.push_back(0xE0);
    for (uint8_t b : pkg) {
        if (b == 0xD0 || b == 0xE0) {
            g_jvsOut.push_back(0xD0);
            g_jvsOut.push_back(b - 1);
        } else {
            g_jvsOut.push_back(b);
        }
    }
}
// --- the player's controls: Xbox pad 1 (XInput) -> JVS --------------------------------------------------------
// View = coin (one per press), View + LB = Service, View + Menu = Test.
// Switch bytes (TeknoParrot's layout): system byte bit 7 = Test; per player byte 1: 7 Start, 6 Service, 5 Up, 4 Down,
// 3 Left, 2 Right, 1 Button 1, 0 Button 2; byte 2: 7..4 Buttons 3-6. Analog: 16 bits per channel, high byte used.
struct PadState {
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
typedef DWORD(WINAPI* XGetRaw_t)(DWORD, void*);
static XGetRaw_t RealXInputGetState() {
    static XGetRaw_t real = [] {
        HMODULE m = GetModuleHandleW(L"xinputuap.dll");
        if (!m) m = GetModuleHandleW(L"xinput1_4.dll");
#if !(defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP)
        if (!m) m = LoadLibraryW(L"xinput1_4.dll");
#endif
        return m ? (XGetRaw_t)GetProcAddress(m, "XInputGetState") : nullptr;
    }();
    return real;
}
static bool (*g_padReader)(unsigned, void*) = nullptr;
void SetPadReader(bool (*reader)(unsigned, void*)) { g_padReader = reader; }
static PadState Pad();
PadIn CurrentPad() {
    PadState s = Pad();
    return PadIn{s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry, false};
}
static PadState Pad() {
    struct {
        DWORD packet;
        PadState g;
    } st = {};
    if (g_padReader) {
        if (!g_padReader(0, &st)) memset(&st, 0, sizeof st);
        return st.g;
    }
    XGetRaw_t get = RealXInputGetState();
    if (!get || get(0, &st) != ERROR_SUCCESS) memset(&st, 0, sizeof st);
    return st.g;
}
enum : uint16_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kStart = 0x10, kBack = 0x20, kLThumb = 0x40, kRThumb = 0x80,
                  kLB = 0x100, kRB = 0x200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000 };
static uint8_t Stick8(int16_t v, bool flip) {   // -32768..32767 -> 0..255, centre 0x80
    int x = ((int)v + 32768) >> 8;
    return (uint8_t)(flip ? 255 - x : x);
}
static void JvsSwitches(int players, int per, std::vector<uint8_t>& r) {
    PadState p = Pad();
    uint16_t b = p.buttons;
    bool test = (b & kBack) && (b & kStart);   // View + Menu together: the cabinet's Test button
    bool service = (b & kBack) && (b & kLB);
    static bool viewWas = false;
    bool view = (b & kBack) && !test && !service;
    if (view && !viewWas) {   // a coin in slot 1
        g_coins[0]++;
        Log("JVS: coin in (pad View), %d now", g_coins[0]);
    }
    viewWas = (b & kBack) != 0;
    r.push_back(test ? 0x80 : 0x00);
    if (ControlsOn()) {   // the game's TeknoParrot profile, mapped (controls.cpp)
        PadIn in = {b, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry, service};
        ControlsSwitches(in, players, per, r);
        return;
    }
    for (int pl = 0; pl < players; pl++) {
        uint8_t s1 = 0, s2 = 0, s3 = 0;
        if (pl == 0) {
            if ((b & kStart) && !test) s1 |= 0x80;
            if (service) s1 |= 0x40;   // Service
            if (b & kUp) s1 |= 0x20;
            if (b & kDown) s1 |= 0x10;
            if (b & kLeft) s1 |= 0x08;
            if (b & kRight) s1 |= 0x04;
            if ((b & kA) || p.rt > 64) s1 |= 0x02;   // Button 1: A or right trigger
            if ((b & kB) || p.lt > 64) s1 |= 0x01;   // Button 2: B or left trigger
            if (b & kX) s2 |= 0x80;
            if (b & kY) s2 |= 0x40;
            if (b & kLB) s2 |= 0x20;
            if (b & kRB) s2 |= 0x10;
            if (b & kLThumb) s2 |= 0x08;
            if (b & kRThumb) s2 |= 0x04;
        }
        if (pl == 0 && GetEnvironmentVariableA("ARCADE_JVS_PROBE", nullptr, 0)) {   // (testing) which bits a game reads
            s1 = (b & kA) ? 0xFF : 0;
            s2 = (b & kB) ? 0xFF : 0;
            s3 = (b & kX) ? 0xFF : 0;
            if (b & kY) r[r.size() - 1 - pl] |= 0x7F;
        }
        const uint8_t bytes[3] = {s1, s2, s3};
        if (pl == 0) {   // (testing) what a press sends
            static uint32_t was = 0;
            uint32_t now = (uint32_t)s1 << 16 | (uint32_t)s2 << 8 | (test ? 0x80 : 0);
            if (now != was) Log("JVS switches: system %02X, player 1 %02X %02X (%d bytes asked)", test ? 0x80 : 0, s1, s2, per);
            was = now;
        }
        for (int i = 0; i < per; i++) r.push_back(i < 3 ? bytes[i] : 0);
    }
}
static void JvsAnalog(int channels, std::vector<uint8_t>& r) {
    PadState p = Pad();
    uint8_t v[8] = {Stick8(p.lx, false), Stick8(p.ly, true), Stick8(p.rx, false), Stick8(p.ry, true), p.lt, p.rt,
                    0x80, 0x80};
    if (ControlsOn()) {
        PadIn in = {p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry, false};
        ControlsAnalog(in, channels, r);
        return;
    }
    for (int i = 0; i < channels; i++) {
        r.push_back(i < 8 ? v[i] : 0x80);
        r.push_back(0x00);
    }
}

// one command of a package: its reply bytes and how many request bytes it used (0: unknown, stop)
static size_t JvsCommand(const uint8_t* c, size_t left, bool multi, std::vector<uint8_t>& r) {
    const Profile& p = *g_profile;
    auto report = [&] { if (multi) r.push_back(0x01); };
    switch (c[0]) {
    // Taito commands (TeknoParrot JvsTaito01..6D: answered the same way for every board)
    case 0x01: r.push_back(0x01); r.push_back(0x01); return 2;
    case 0x03: r.push_back(0x01); return 2;
    case 0x04: return 1;
    case 0x05: return 3;
    case 0x23: return 2;
    case 0x34: return left > 1 ? (size_t)c[1] + 2 : 2;
    case 0x65:
        if (multi) r.push_back(0x01);
        r.push_back(0x00);
        r.push_back(0x00);
        return 2;
    case 0x6A: return 9;
    case 0x6B: return 2;
    case 0x6D: return 2;
    case 0xF1:   // set address
        g_jvsAddressed = true;
        return 2;
    case 0x10: {   // identify
        const char* id = p.jvsId;
        r.insert(r.end(), id, id + strlen(id) + 1);
        return 1;
    }
    case 0x11: report(); r.push_back(p.cmdRevision); return 1;
    case 0x12: report(); r.push_back(p.jvsVersion); return 1;
    case 0x13: report(); r.push_back(p.commVersion); return 1;
    case 0x14: {   // features
        report();
        if (p.battleGear) {   // (TeknoParrot JvsGetSlaveFeatures, TaitoBattleGear)
            const uint8_t f[] = {0x01, 0x02, 0x14, 0x00, 0x03, 0x08, 0x10, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            r.insert(r.end(), f, f + sizeof f);
            return 1;
        }
        if (p.taitoStick) {   // (TeknoParrot JvsGetSlaveFeatures, TaitoStick)
            const uint8_t f[] = {0x01, 0x02, 0x10, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            r.insert(r.end(), f, f + sizeof f);
            return 1;
        }
        const uint8_t f[] = {0x01, 0x02, p.switches, 0x00,   // switches: 2 players
                             0x02, 0x02, 0x00, 0x00,         // coins: 2 slots
                             0x03, 0x08, 0x0A, 0x00,         // analog: 8 channels, 10 bits
                             0x12, 0x14, 0x00, 0x00,         // general outputs: 20
                             0x00};
        r.insert(r.end(), f, f + sizeof f);
        return 1;
    }
    case 0x15: {   // main board id (text up to 0)
        size_t n = 1;
        while (n < left && c[n]) n++;
        const uint8_t ok[] = {0x01, 0x01, 0x05};
        r.insert(r.end(), ok, ok + 3);
        return n + 1;
    }
    case 0x20: {   // switches: players, bytes per player
        report();
        size_t at = r.size();
        JvsSwitches(left > 1 ? c[1] : 2, p.taitoStick ? 2 : (left > 2 ? c[2] : 2), r);   // Taito: 2 bytes each
        // Battle Gear 4 Tuned: its NESYS entry key reader's sensor (player 1 bit 10, active low) says "no key"; with
        // it clear the game says "エントリーキーでは遊べません" (the key cannot be read)
        if (p.battleGear && p.crc == 0x93c2bf69 && r.size() > at + 1) r[at + 1] |= 0x04;
        return 3;
    }
    case 0x21: {   // coins
        report();
        int slots = left > 1 ? c[1] : 2;
        for (int i = 0; i < slots; i++) {
            int n = i < 2 ? g_coins[i] : 0;
            r.push_back((uint8_t)(n >> 8));
            r.push_back((uint8_t)n);
        }
        return 2;
    }
    case 0x22: {   // analog
        report();
        if (p.battleGear && p.crc == 0x93c2bf69) {   // (Tuned: once its screen +0x4e3668 is 8, its pause flag held at 1)
            Bg4TunedTick();
        }
        if (p.battleGear) {   // (TeknoParrot TaitoBattleGear: gas, brake, then six idle channels)
            uint8_t gas = 0, brake = 0;
            {
                PadState ps = Pad();
                gas = ps.rt, brake = ps.lt;
            }
            r.insert(r.end(), {0x04, gas, 0x04, brake});
            for (int i = 0; i < 6; i++) r.insert(r.end(), {0x80, 0x00});
            return 2;
        }
        JvsAnalog(left > 1 ? c[1] : 8, r);
        return 2;
    }
    case 0x2E: report(); r.insert(r.end(), {0, 0, 0, 0}); return 2;          // hopper
    case 0x2F: r.insert(r.end(), {0, 0, 0, 0, 0}); return 1;                 // retransmit
    case 0x30:
    case 0x31: {   // coin decrease / increase
        if (left >= 4) {
            int slot = c[1] - 1, n = (c[2] << 8) | c[3];
            if (slot >= 0 && slot < 2) g_coins[slot] = c[0] == 0x30 ? (g_coins[slot] > n ? g_coins[slot] - n : 0) : g_coins[slot] + n;
        }
        report();
        return 4;
    }
    case 0x32: report(); return left > 1 ? (size_t)c[1] + 2 : 2;            // general outputs (lamps)
    case 0x33: report(); return left > 1 ? (size_t)c[1] * 2 + 2 : 2;        // analog outputs
    case 0x36: report(); return 4;
    case 0x37: report(); return 3;
    case 0x70:   // Namco custom (Taito: a 2-byte command with no reply)
        if (p.taitoStick || p.battleGear || p.typeXSense) return 2;
        if (!p.namco || left < 2) break;
        switch (c[1]) {
        case 0x18: report(); r.push_back(0x01); return left;
        case 0x05: report(); r.push_back(0x01); return left > 2 ? c[2] : left;
        case 0x03: report(); r.push_back(0x00); return 4;
        case 0x15:
        case 0x16: report(); r.push_back(0x01); return 4;
        }
        break;
    }
    if (c[0] >= 0x78 && c[0] <= 0x80 && p.namco) {   // Namco: skipped
        r.push_back(0x01);
        return 15;
    }
    if (p.namco) return 1;   // (TeknoParrot: unknown Namco commands are skipped one byte at a time)
    return 0;
}
static void JvsPackage(const std::vector<uint8_t>& pk) {   // pk: E0 node size data... sum
    if (pk.size() < 4) return;
    uint8_t node = pk[1];
    if (node != 0xFF && node > 0x01) return;   // one board
    if (pk[3] == 0xF0) {                       // reset
        g_jvsAddressed = false;
        return;
    }
    std::vector<uint8_t> data(pk.begin() + 3, pk.end() - 1), r;
    size_t pos = 0;
    bool multi = false;
    while (pos < data.size()) {
        size_t used = JvsCommand(&data[pos], data.size() - pos, multi, r);
        if (!used) {
            static int said = 0;
            if (said++ < 10) Log("JVS: command 0x%02x not known", data[pos]);
            break;
        }
        pos += used;
        multi = true;
    }
    // reply: node 0, size, status OK, report OK, data, sum
    std::vector<uint8_t> out = {0x00, (uint8_t)(r.size() + 3), 0x01, 0x01};
    out.insert(out.end(), r.begin(), r.end());
    unsigned sum = 0;
    for (size_t i = 0; i < out.size(); i++) sum += out[i];
    out.push_back((uint8_t)sum);
    JvsEncode(out);
    static int logged = 0;
    if (logged++ < 12) {
        char hex[200] = "";
        for (size_t i = 0; i < pk.size() && i < 60; i++) sprintf_s(hex + strlen(hex), sizeof hex - strlen(hex), "%02X ", pk[i]);
        Log("JVS: package %s-> %zu reply bytes", hex, out.size());
    }
}
static void JvsWrite(const uint8_t* b, size_t n) {
    std::lock_guard<std::mutex> l(g_jvsLock);
    static int said = 0;
    if (said++ < 20) {   // (testing) what the game sends
        char hex[200] = "";
        for (size_t i = 0; i < n && i < 50; i++) sprintf_s(hex + strlen(hex), sizeof hex - strlen(hex), "%02X ", b[i]);
        Log("JVS: game wrote %zu bytes: %s", n, hex);
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t x = b[i];
        if (x == 0xE0) {   // a new package
            g_jvsIn.clear();
            g_jvsIn.push_back(0xE0);
            g_jvsEscape = false;
            continue;
        }
        if (x == 0xD0) {
            g_jvsEscape = true;
            continue;
        }
        if (g_jvsEscape) {
            x++;
            g_jvsEscape = false;
        }
        if (g_jvsIn.empty()) continue;   // waiting for a sync byte
        g_jvsIn.push_back(x);
        if (g_jvsIn.size() >= 3 && g_jvsIn.size() == (size_t)g_jvsIn[2] + 3) {
            JvsPackage(std::vector<uint8_t>(g_jvsIn.begin(), g_jvsIn.end()));
            g_jvsIn.clear();
        }
    }
}
static DWORD JvsRead(uint8_t* b, DWORD n) {
    std::lock_guard<std::mutex> l(g_jvsLock);
    static int said = 0;
    if (said++ < 12) Log("JVS: game reads up to %lu bytes, %zu waiting", (unsigned long)n, g_jvsOut.size());
    DWORD k = 0;
    while (k < n && !g_jvsOut.empty()) {
        b[k++] = g_jvsOut.front();
        g_jvsOut.pop_front();
    }
    return k;
}

// --- kernel32 file and serial port calls: the JVS port is ours, anything else goes to Windows ---------------------
template <typename T> static T Real(const char* name) {
    static HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    return (T)GetProcAddress(kb, name);
}
static bool IsJvsName(const char* n) {
    if (!g_profile || !g_profile->jvsPort || !n) return false;
    if (!_strnicmp(n, "\\\\.\\", 4)) n += 4;
    return !_stricmp(n, g_profile->jvsPort);
}
static HANDLE WINAPI S_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    if (HANDLE d = RingEdgeOpen(name)) return d;
    if (g_profile && g_profile->battleGear && name && (!_stricmp(name, "COM1") || !_stricmp(name, "\\\\.\\COM1"))) {
        Log("steering board: the game opened %s (emulated drive board)", name);
        return kWheelHandle;
    }
    if (IsJvsName(name)) {
        Log("JVS: the game opened %s (emulated I/O board)", name);
        return kJvsHandle;
    }
    return Real<HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>("CreateFileA")(name, acc, share, sa, disp, fl, t);
}
static int WheelSteering() {   // the left stick (or the triggers' profile wheel): 0 left .. 255 right
    int v = (Pad().lx + 32768) >> 8;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}
static void Bg4TunedTick() {
    __try {
        if (*(volatile uint8_t*)0x8E3668 == 8) *(volatile uint8_t*)0x82D29C = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// coin button presses so far (View alone), for games that count coins themselves (Konami's libacio: acio32.cpp)
int CoinPresses() {
    static int presses = 0;
    static bool was = false;
    uint16_t b = Pad().buttons;
    bool view = (b & kBack) && !(b & kStart) && !(b & kLB);
    if (view && !was) {
        presses++;
        Log("coin in (pad View), %d so far", presses);
    }
    was = (b & kBack) != 0;
    return presses;
}
// --- NESiCAxLive / Type X "Fast I/O" board (our iDmacDrv32.dll asks through "gvio.dll", bridged): register reads in
// the board's layout (OpenParrot FastIoEmu.cpp, aderumier/windowsarcadeloader DESIGN.md; GPL-3.0). byte0: start 0x10,
// test 0x40, service 0x04; byte1: up 0x01 down 0x04 left 0x10 right 0x40; byte2: buttons 1-4 = 0x01 0x04 0x10 0x40;
// byte3: buttons 5-6 = 0x01 0x04 (player 1's bits; player 2's are the bits above). Pad: A B X Y = buttons 1-4,
// RB = 5, RT = 6, Menu = start, View = coin, View + LB = service, View + Menu = test.
static uint64_t G_FastIoRead(uint64_t command) {
    PadState p = Pad();
    uint16_t b = p.buttons;
    bool view = (b & kBack) != 0;
    bool test = view && (b & kStart), service = view && (b & kLB);
    bool up = (b & kUp) || p.ly > 16000, down = (b & kDown) || p.ly < -16000;
    bool left = (b & kLeft) || p.lx < -16000, right = (b & kRight) || p.lx > 16000;
    uint8_t b0 = (uint8_t)(((b & kStart) && !view ? 0x10 : 0) | (test ? 0x40 : 0) | (service ? 0x04 : 0));
    uint8_t b1 = (uint8_t)((up ? 0x01 : 0) | (down ? 0x04 : 0) | (left ? 0x10 : 0) | (right ? 0x40 : 0));
    uint8_t b2 = (uint8_t)((b & kA ? 0x01 : 0) | (b & kB ? 0x04 : 0) | (b & kX ? 0x10 : 0) | (b & kY ? 0x40 : 0));
    uint8_t b3 = (uint8_t)((b & kRB ? 0x01 : 0) | (p.rt > 100 ? 0x04 : 0));
    switch ((uint32_t)command) {
    case 0x400: return 0x00010201;
    case 0x4000: return 0x00FF00FF;
    case 0x4004: return 0x00FF0000;
    case 0x4120: return b0 | (uint32_t)b1 << 8 | (uint32_t)b2 << 16 | (uint32_t)b3 << 24;   // players 1 and 2
    case 0x4124: case 0x41A4: return 0x01100000;
    case 0x4128: return (uint32_t)Stick8(p.lx, false) | (uint32_t)Stick8(p.ly, true) << 8;   // analogs
    case 0x4140: {   // coin: 1 once per press
        static int seen = 0;
        int now = CoinPresses();
        if (now != seen) {
            seen = now;
            return 1;
        }
        return 0;
    }
    case 0x4150: return 0x1823C;
    default: return 0;
    }
}
// the pad itself, for our 32-bit DirectInput (its keyboard and joystick): buttons, triggers, sticks (12 bytes)
static uint64_t G_PadRead(uint64_t out) {
    if (!(uint32_t)out) return 0;
    PadState p = Pad();
    memcpy((void*)(uintptr_t)(uint32_t)out, &p, sizeof p);
    return 1;
}
void* FindGvIo32(const char* dll, const char* func, int& args) {
    if (_stricmp(dll, "gvio.dll")) return nullptr;
    static bool told = false;
    if (!told) told = true, Log("Fast I/O board (iDmacDrv32): the pad");
    if (!strcmp(func, "GvFastIoRead")) return args = 1, (void*)G_FastIoRead;
    if (!strcmp(func, "GvPadRead")) return args = 1, (void*)G_PadRead;
    return nullptr;
}
// --- data files patched on the way in: a copy in the loader's own storage (LocalState\patched), made once, opened
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
#if !(defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP)
    (void)path;
    return L"";   // (the PC test build: the game's own files)
#else
    size_t k = path.find_last_of(L"\\/");
    std::wstring file = path.substr(k == std::wstring::npos ? 0 : k + 1);
    for (const PatchedFile& pf : kPatchedFiles) {
        if (_wcsicmp(pf.exe, g_exeName.c_str()) || _wcsicmp(pf.file, file.c_str())) continue;
        wchar_t local[MAX_PATH] = L"";
        if (!GetEnvironmentVariableW(L"ARCADE_LOCAL", local, MAX_PATH)) return L"";
        std::wstring dir = std::wstring(local) + L"\\patched", out = dir + L"\\" + file;
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (GetFileAttributesExFromAppW(out.c_str(), GetFileExInfoStandard, &ad) && ad.nFileSizeLow == (DWORD)pf.size) return out;
        std::vector<uint8_t> data;
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
        }
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
#endif
}
std::wstring PatchedFileFor(const std::wstring& path) { return PatchedCopy(path); }
static HANDLE WINAPI S_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    if (name) {
        char a[64] = "";
        for (int k = 0; k < 63 && name[k]; k++) a[k] = (char)name[k], a[k + 1] = 0;
        if (HANDLE d = RingEdgeOpen(a)) return d;
    }
    if (g_profile && g_profile->battleGear && name && (!_wcsicmp(name, L"COM1") || !_wcsicmp(name, L"\\\\.\\COM1"))) {
        Log("steering board: the game opened COM1 (emulated drive board)");
        return kWheelHandle;
    }
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
    }
    return Real<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>("CreateFileW")(name, acc, share, sa, disp, fl, t);
}
static void Complete(LPOVERLAPPED ov, DWORD n) {
    if (!ov) return;
    ov->Internal = 0;
    ov->InternalHigh = n;
    if (ov->hEvent) SetEvent(ov->hEvent);
}
// an OVERLAPPED done at once: its result and its event (a 32-bit program's OVERLAPPED: 4-byte fields, hEvent at +16)
extern bool g_running32;   // peload32.cpp
static void CompleteOverlapped(LPOVERLAPPED ov, DWORD got) {
    if (g_running32 && (uintptr_t)ov < 0x80000000ull) {
        uint32_t* o = (uint32_t*)ov;
        o[0] = 0, o[1] = got;
        if (o[4]) SetEvent((HANDLE)(intptr_t)(int32_t)o[4]);
    } else {
        ov->Internal = 0, ov->InternalHigh = got;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
}
// an OVERLAPPED's file offset (a 32-bit program's: Offset at +8)
static int64_t OverlappedOffset(LPOVERLAPPED ov) {
    if (g_running32 && (uintptr_t)ov < 0x80000000ull) {
        const uint32_t* o = (const uint32_t*)ov;
        return (int64_t)((uint64_t)o[3] << 32 | o[2]);
    }
    return (int64_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset);
}
static BOOL WINAPI S_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (RingEdgeHandle(h)) {   // the SRAM is a disk: an OVERLAPPED gives the place
        if (ov) RingEdgeSeek(h, OverlappedOffset(ov), FILE_BEGIN, nullptr);
        DWORD k = 0;
        RingEdgeWrite(h, b, n, &k);
        if (done) *done = k;
        if (ov) CompleteOverlapped(ov, k);
        return TRUE;
    }
    if (h == kWheelHandle) {
        std::lock_guard<std::mutex> l(g_wheelLock);
        for (DWORD i = 0; i < n; i++) {
            uint8_t c = ((const uint8_t*)b)[i];
            if (!g_wheelHalf) g_wheelCmd = c;
            else WheelCommand(g_wheelCmd, c);
            g_wheelHalf = !g_wheelHalf;
        }
        if (done) *done = n;
        if (ov) ov->InternalHigh = n, ov->Internal = 0;
        return TRUE;
    }
    if (h != kJvsHandle) return Real<decltype(&WriteFile)>("WriteFile")(h, b, n, done, ov);
    JvsWrite((const uint8_t*)b, n);
    if (done) *done = n;
    Complete(ov, n);
    return TRUE;
}
static BOOL WINAPI S_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (RingEdgeHandle(h)) {
        if (ov) RingEdgeSeek(h, OverlappedOffset(ov), FILE_BEGIN, nullptr);
        DWORD k = 0;
        RingEdgeRead(h, b, n, &k);
        if (done) *done = k;
        if (ov) CompleteOverlapped(ov, k);
        return TRUE;
    }
    if (h == kWheelHandle) {
        std::lock_guard<std::mutex> l(g_wheelLock);
        DWORD k = 0;
        while (k < n && !g_wheelOut.empty()) ((uint8_t*)b)[k++] = g_wheelOut.front(), g_wheelOut.pop_front();
        if (done) *done = k;
        if (ov) ov->InternalHigh = k, ov->Internal = 0;
        return TRUE;
    }
    static int said = 0;
    if (said++ < 30) Log("ReadFile handle %p (board %p), %lu bytes, overlapped %p", h, kJvsHandle, (unsigned long)n, ov);
    if (h != kJvsHandle) {
        if (BigRead(h, b, n, done, ov)) return TRUE;   // filebacked.cpp: a whole file into a big block
        if (BigLazyRead(h, b, n, done, ov)) return TRUE;   // a piece into a big block: read when touched
        BigPrepare(b, n);
        if (NetFsHandle(h)) {   // a file on the NAS (netfs.cpp)
            if (void* r = FindNetFsShim("kernel32.dll", "ReadFile"))
                return ((BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED))r)(h, b, n, done, ov);
        }
        return Real<decltype(&ReadFile)>("ReadFile")(h, b, n, done, ov);
    }
    DWORD k = JvsRead((uint8_t*)b, n);
    if (done) *done = k;
    Complete(ov, k);
    return TRUE;
}
static BOOL WINAPI S_CloseHandle(HANDLE h) {
    if (IsOurs(h) || RingEdgeHandle(h)) return TRUE;
    return Real<decltype(&CloseHandle)>("CloseHandle")(h);
}
static BOOL WINAPI S_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD done, BOOL wait) {
    if (!IsOurs(h)) return Real<decltype(&GetOverlappedResult)>("GetOverlappedResult")(h, ov, done, wait);
    if (done) *done = ov ? (DWORD)ov->InternalHigh : 0;
    return TRUE;
}
typedef BOOL(WINAPI* Comm2_t)(HANDLE, void*);
static BOOL WINAPI S_GetCommState(HANDLE h, LPDCB d) {
    if (!IsOurs(h)) return Real<Comm2_t>("GetCommState")(h, d);
    memset(d, 0, sizeof *d);
    d->DCBlength = sizeof *d;
    d->BaudRate = 115200;
    d->fBinary = 1;
    d->ByteSize = 8;
    return TRUE;
}
static BOOL WINAPI S_SetCommState(HANDLE h, LPDCB d) {
    return IsOurs(h) ? TRUE : Real<Comm2_t>("SetCommState")(h, d);
}
static BOOL WINAPI S_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) {
    if (!IsOurs(h)) return Real<Comm2_t>("GetCommTimeouts")(h, t);
    memset(t, 0, sizeof *t);
    return TRUE;
}
static BOOL WINAPI S_SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) {
    return IsOurs(h) ? TRUE : Real<Comm2_t>("SetCommTimeouts")(h, t);
}
static BOOL WINAPI S_SetupComm(HANDLE h, DWORD a, DWORD b) {
    return IsOurs(h) ? TRUE : Real<BOOL(WINAPI*)(HANDLE, DWORD, DWORD)>("SetupComm")(h, a, b);
}
static BOOL WINAPI S_PurgeComm(HANDLE h, DWORD f) {
    if (!IsOurs(h)) return Real<BOOL(WINAPI*)(HANDLE, DWORD)>("PurgeComm")(h, f);
    std::lock_guard<std::mutex> l(g_jvsLock);
    if (f & PURGE_RXCLEAR) g_jvsOut.clear();
    if (f & PURGE_TXCLEAR) g_jvsIn.clear();
    return TRUE;
}
static BOOL WINAPI S_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st) {
    if (h == kWheelHandle) {
        if (err) *err = 0;
        if (st) {
            memset(st, 0, sizeof *st);
            std::lock_guard<std::mutex> l(g_wheelLock);
            st->cbInQue = (DWORD)g_wheelOut.size();
        }
        return TRUE;
    }
    if (h != kJvsHandle) return Real<BOOL(WINAPI*)(HANDLE, LPDWORD, LPCOMSTAT)>("ClearCommError")(h, err, st);
    if (err) *err = 0;
    if (st) {
        memset(st, 0, sizeof *st);
        std::lock_guard<std::mutex> l(g_jvsLock);
        st->cbInQue = (DWORD)g_jvsOut.size();
    }
    return TRUE;
}
static BOOL WINAPI S_EscapeCommFunction(HANDLE h, DWORD f) {
    return IsOurs(h) ? TRUE : Real<BOOL(WINAPI*)(HANDLE, DWORD)>("EscapeCommFunction")(h, f);
}
static BOOL WINAPI S_GetCommModemStatus(HANDLE h, LPDWORD s) {
    if (h == kWheelHandle) {
        *s = MS_CTS_ON | MS_DSR_ON;
        return TRUE;
    }
    if (h != kJvsHandle) return Real<BOOL(WINAPI*)(HANDLE, LPDWORD)>("GetCommModemStatus")(h, s);
    // the JVS sense line (OpenParrot amJvs.cpp): Type X games read bit 0x10 as "board addressed" (off until then;
    // Chaos Breaker resets the board for ever while it is on); other games want 0x10 always and 0x20 once addressed
    if (g_profile && (g_profile->taitoStick || g_profile->typeXSense)) *s = g_jvsAddressed ? 0x10 : 0x00;
    else *s = g_jvsAddressed ? 0x30 : 0x10;
    return TRUE;
}
static BOOL WINAPI S_SetCommMask(HANDLE h, DWORD m) {
    return IsOurs(h) ? TRUE : Real<BOOL(WINAPI*)(HANDLE, DWORD)>("SetCommMask")(h, m);
}
static BOOL WINAPI S_GetCommMask(HANDLE h, LPDWORD m) {
    if (!IsOurs(h)) return Real<BOOL(WINAPI*)(HANDLE, LPDWORD)>("GetCommMask")(h, m);
    *m = 0;
    return TRUE;
}
static BOOL WINAPI S_WaitCommEvent(HANDLE h, LPDWORD m, LPOVERLAPPED ov) {
    if (h == kWheelHandle) {   // (an answer is always there right after a command)
        if (m) *m = EV_RXCHAR;
        return TRUE;
    }
    if (h != kJvsHandle) return Real<BOOL(WINAPI*)(HANDLE, LPDWORD, LPOVERLAPPED)>("WaitCommEvent")(h, m, ov);
    if (m) *m = EV_RXCHAR;
    Complete(ov, 0);
    return TRUE;
}
static BOOL WINAPI S_FlushFileBuffers(HANDLE h) {
    return IsOurs(h) ? TRUE : Real<BOOL(WINAPI*)(HANDLE)>("FlushFileBuffers")(h);
}
static DWORD WINAPI S_GetFileType(HANDLE h) {
    return IsOurs(h) ? FILE_TYPE_CHAR : Real<DWORD(WINAPI*)(HANDLE)>("GetFileType")(h);
}

// --- HASP dongle (OpenParrot StarWars.cpp) -------------------------------------------------------------------------
static uint8_t g_hasp[0xD40];
static void HaspData() {
    static bool made = false;
    if (made) return;
    made = true;
    memset(g_hasp, 0, sizeof g_hasp);
    const uint8_t a[] = {0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0x0A, 0, 0, 0, 0x04,
                         0x3B, 0x6B, 0x40, 0x87};
    memcpy(g_hasp, a, sizeof a);
    memcpy(g_hasp + 0x20, a + 0x10, 0x10);   // second record: 0x23, 0x27, 0x2B, 0x2C..0x2F
    memcpy(g_hasp + 0xD00, "274320990002", 12);
    g_hasp[0xD3E] = 0x6A;
    g_hasp[0xD3F] = 0x95;
}
static int __cdecl H_login(int, const void*, int* handle) {
    HaspData();
    if (handle) *handle = 1;
    static bool said = false;
    if (!said) Log("dongle: hasp_login (stand-in)");
    said = true;
    return 0;
}
static int __cdecl H_ok1(int) { return 0; }
static int __cdecl H_crypt(int, void*, unsigned) { return 0; }
static int __cdecl H_get_size(int, int, unsigned* size) {
    if (size) *size = sizeof g_hasp;
    return 0;
}
static int __cdecl H_read(int, int, unsigned offset, unsigned length, uint8_t* buffer) {
    HaspData();
    if (offset >= sizeof g_hasp) return 0;
    if (length > sizeof g_hasp - offset) length = sizeof g_hasp - offset;
    memcpy(buffer, g_hasp + offset, length);
    return 0;
}
static int __cdecl H_write(int, int, unsigned, unsigned, const uint8_t*) { return 0; }

// --- XInput: a connected pad for games that check (the cabinet controls) ------------------------------------------
typedef DWORD(WINAPI* XGet_t)(DWORD, void*);
static DWORD WINAPI S_XInputGetState(DWORD i, void* st) {
    static int told = 0;
    if (told++ < 3) Log("XInputGetState(%lu) by the game", (unsigned long)i);
    DWORD r = ERROR_DEVICE_NOT_CONNECTED;
    if (g_padReader) r = g_padReader(i, st) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    else if (XGet_t real = (XGet_t)RealXInputGetState()) r = real(i, st);
    if (r != ERROR_SUCCESS && g_profile && g_profile->padAlways && i == 0) {
        memset(st, 0, 16);   // XINPUT_STATE
        r = ERROR_SUCCESS;
    }
    return r;
}

// --- lookup ---------------------------------------------------------------------------------------------------------
struct IoShim {
    const char* func;
    void* fn;
};
static BOOL WINAPI S_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD ret,
                                     LPOVERLAPPED ov) {
    if (RingEdgeIoctl(h, code, in, inSize, out, outSize, ret)) return TRUE;
    if (h == kJvsHandle) {   // Sega's \.\mxjvs / AMEX JVS driver: hello, the sense line, one packet in -> its reply out
        if (ret) *ret = 0;
        if (code == 0x80006004 && out && outSize >= 2) {   // hello
            memset(out, 0, 2);
            if (ret) *ret = 2;
            return TRUE;
        }
        if (code == 0x8000600C && out && outSize >= 1) {   // sense: 2 addressed (the driver addresses the board itself)
            g_jvsAddressed = true;
            *(uint8_t*)out = 2;
            if (ret) *ret = 1;
            return TRUE;
        }
        if (code != 0x9C402000 && code != 0x8000E008) {
            static int told = 0;
            if (told++ < 10) Log("JVS driver: unknown request %08lx", code);
            SetLastError(ERROR_INVALID_FUNCTION);
            return FALSE;
        }
        g_jvsAddressed = true;   // (the driver's own reset and address: the game never sends them)
        static int told = 0;
        if (told++ < 3) Log("JVS driver: request %08lx, %lu bytes in, %lu out room", code, inSize, outSize);
        DWORD n = 0;
        S_WriteFile(kJvsHandle, in, inSize, &n, nullptr);
        DWORD got = 0;
        S_ReadFile(kJvsHandle, out, outSize, &got, nullptr);
        if (ret) *ret = got;
        if (ov) CompleteOverlapped(ov, got);
        if (!got && code == 0x8000E008) {
            SetLastError(ERROR_NO_DATA_DETECTED);
            return FALSE;
        }
        return TRUE;
    }
    if (h == kWheelHandle) return TRUE;
    return Real<decltype(&DeviceIoControl)>("DeviceIoControl")(h, code, in, inSize, out, outSize, ret, ov);
}
static DWORD WINAPI S_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD how) {
    int64_t now = 0;
    if (RingEdgeSeek(h, hi ? ((int64_t)*hi << 32 | (uint32_t)lo) : lo, how, &now)) {
        if (hi) *hi = (LONG)(now >> 32);
        return (DWORD)now;
    }
    return Real<decltype(&SetFilePointer)>("SetFilePointer")(h, lo, hi, how);
}
static BOOL WINAPI S_SetFilePointerEx(HANDLE h, LARGE_INTEGER to, PLARGE_INTEGER now, DWORD how) {
    int64_t n = 0;
    if (RingEdgeSeek(h, to.QuadPart, how, &n)) {
        if (now) now->QuadPart = n;
        return TRUE;
    }
    return Real<decltype(&SetFilePointerEx)>("SetFilePointerEx")(h, to, now, how);
}
static const IoShim kSerial[] = {
    {"DeviceIoControl", (void*)S_DeviceIoControl}, {"SetFilePointer", (void*)S_SetFilePointer},
    {"SetFilePointerEx", (void*)S_SetFilePointerEx},
    {"CreateFileA", (void*)S_CreateFileA}, {"CreateFileW", (void*)S_CreateFileW}, {"WriteFile", (void*)S_WriteFile},
    {"ReadFile", (void*)S_ReadFile}, {"CloseHandle", (void*)S_CloseHandle},
    {"GetOverlappedResult", (void*)S_GetOverlappedResult}, {"GetCommState", (void*)S_GetCommState},
    {"SetCommState", (void*)S_SetCommState}, {"GetCommTimeouts", (void*)S_GetCommTimeouts},
    {"SetCommTimeouts", (void*)S_SetCommTimeouts}, {"SetupComm", (void*)S_SetupComm}, {"PurgeComm", (void*)S_PurgeComm},
    {"ClearCommError", (void*)S_ClearCommError}, {"EscapeCommFunction", (void*)S_EscapeCommFunction},
    {"GetCommModemStatus", (void*)S_GetCommModemStatus}, {"SetCommMask", (void*)S_SetCommMask},
    {"GetCommMask", (void*)S_GetCommMask}, {"WaitCommEvent", (void*)S_WaitCommEvent},
    {"FlushFileBuffers", (void*)S_FlushFileBuffers}, {"GetFileType", (void*)S_GetFileType},
};
static const IoShim kHasp[] = {
    {"hasp_login", (void*)H_login}, {"hasp_logout", (void*)H_ok1}, {"hasp_encrypt", (void*)H_crypt},
    {"hasp_decrypt", (void*)H_crypt}, {"hasp_get_size", (void*)H_get_size}, {"hasp_read", (void*)H_read},
    {"hasp_write", (void*)H_write},
};
void* FindIoShim(const char* dll, const char* func) {
    if (!g_profile || !func) return nullptr;
    if (g_profile->jvsPort && (!_stricmp(dll, "kernel32.dll") || !_stricmp(dll, "kernelbase.dll") ||
                               !_strnicmp(dll, "api-ms-win-core-", 16)))
        for (const IoShim& s : kSerial)
            if (!strcmp(s.func, func)) return s.fn;
    if (g_profile->hasp && !_strnicmp(dll, "hasp_", 5))
        for (const IoShim& s : kHasp)
            if (!strcmp(s.func, func)) return s.fn;
    if (!_strnicmp(dll, "xinput", 6) && !strcmp(func, "XInputGetState")) return (void*)S_XInputGetState;
    return nullptr;
}
// imports by ordinal from system DLLs with well-known ordinals (XInput 1.3: 2 = XInputGetState)
const char* KnownOrdinalName(const char* dll, uint16_t ordinal) {
    if (!_strnicmp(dll, "xinput1_", 8)) {
        switch (ordinal) {
        case 2: return "XInputGetState";
        case 3: return "XInputSetState";
        case 4: return "XInputGetCapabilities";
        case 5: return "XInputEnable";
        }
    }
    return nullptr;
}

}  // namespace arcade
