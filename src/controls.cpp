// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// AUTO MAPPER: a game's controls from its TeknoParrot game profile (GameProfiles/<name>.xml: each control's name, its
// place on the I/O board - "P1ButtonStart", "P1Button3", "ExtensionOne12", "Analog4" - and for analog ones its kind:
// Wheel, Gas, Brake, AnalogJoystick[Reverse]) become an Xbox pad layout by fixed rules:
//   Start = Menu; Test = View + Menu; Service = View + LB; coin = View; menu/direction buttons = D-pad;
//   wheel = left stick X; gas = right trigger, brake = left trigger (gas goes to the left trigger when the game has a
//   trigger/fire button, which then gets the right trigger); joysticks = left stick, a second one the right stick;
//   other buttons in the profile's order: fire-like names -> RT, "view" -> Y, then A B X Y RB LB RT LT R3 L3.
// Switch bit places follow TeknoParrot's JVS reply (JvsPackageEmulator.cs).

#include "peload.h"
#include <windows.h>
#include <fileapifromapp.h>
#include <cctype>
#include <string>
#include <vector>

namespace arcade {

enum Src : int {   // Xbox pad sources (XInput button bits, then analog ones)
    S_None = 0,
    S_LT = 0x10000, S_RT, S_LX, S_LY, S_RX, S_RY,   // analog
};
enum : uint16_t { P_Up = 1, P_Down = 2, P_Left = 4, P_Right = 8, P_Start = 0x10, P_Back = 0x20, P_L3 = 0x40, P_R3 = 0x80,
                  P_LB = 0x100, P_RB = 0x200, P_A = 0x1000, P_B = 0x2000, P_X = 0x4000, P_Y = 0x8000 };

struct Bind {
    std::string name;   // the profile's button name
    int src = S_None;
    // where it goes
    int kind = 0;       // 1 switch, 2 analog channel
    int player = 0;     // 0 = system byte, 1-2 players
    int byteIdx = 0;
    uint8_t bit = 0;
    int channel = 0;
    bool reverse = false, trigger = false;   // analog: reversed axis; 0..255 from a trigger
};
static std::vector<Bind> g_binds;
static bool g_on = false;
static bool g_stickIsDpad = false;   // no analog stick in the game: the left stick also works the D-pad

static std::string Tag(const std::string& s, size_t from, size_t to, const char* tag) {
    std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    size_t a = s.find(open, from);
    if (a == std::string::npos || a >= to) return "";
    a += open.size();
    size_t b = s.find(close, a);
    return b == std::string::npos ? "" : s.substr(a, b - a);
}
static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static const char* SrcName(int s) {
    switch (s) {
    case P_A: return "A"; case P_B: return "B"; case P_X: return "X"; case P_Y: return "Y";
    case P_LB: return "LB"; case P_RB: return "RB"; case P_L3: return "L3"; case P_R3: return "R3";
    case P_Start: return "Menu"; case P_Back: return "View";
    case P_Up: return "D-pad up"; case P_Down: return "D-pad down"; case P_Left: return "D-pad left"; case P_Right: return "D-pad right";
    case S_LT: return "LT"; case S_RT: return "RT"; case S_LX: return "left stick X"; case S_LY: return "left stick Y";
    case S_RX: return "right stick X"; case S_RY: return "right stick Y";
    }
    return "-";
}

// where a TeknoParrot InputMapping goes on the board (false: not a switch/analog we feed)
static bool Place(const std::string& m, Bind& b) {
    auto sw = [&](int pl, int by, uint8_t bit) { b.kind = 1, b.player = pl, b.byteIdx = by, b.bit = bit; return true; };
    if (m == "Test") return sw(0, 0, 0x80);
    if (m == "Service1") return sw(1, 0, 0x40);
    if (m == "Service2") return sw(2, 0, 0x40);
    if (m.size() > 7 && m[0] == 'P' && (m[1] == '1' || m[1] == '2') && !m.compare(2, 6, "Button")) {
        int pl = m[1] - '0';
        std::string r = m.substr(8);
        if (r == "Start") return sw(pl, 0, 0x80);
        if (r == "Up") return sw(pl, 0, 0x20);
        if (r == "Down") return sw(pl, 0, 0x10);
        if (r == "Left") return sw(pl, 0, 0x08);
        if (r == "Right") return sw(pl, 0, 0x04);
        if (r.empty() || !isdigit((unsigned char)r[0])) return false;
        int n = atoi(r.c_str());
        static const uint8_t bits[] = {0, 0x02, 0x01, 0x80, 0x40, 0x20, 0x10};
        if (n >= 1 && n <= 6) return sw(pl, n <= 2 ? 0 : 1, bits[n]);
        return false;
    }
    if (!m.compare(0, 12, "ExtensionOne") || !m.compare(0, 12, "ExtensionTwo")) {
        int pl = m[9] == 'O' ? 1 : 2;
        int n = atoi(m.c_str() + 12);
        if (n >= 1 && n <= 4) return sw(pl, 1, (uint8_t)(1 << (n - 1)));    // second byte, bits 0-3
        if (n >= 11 && n <= 18) return sw(pl, 2, (uint8_t)(1 << (n - 11)));  // third byte
        return false;
    }
    if (!m.compare(0, 6, "Analog") && m.size() > 6 && isdigit((unsigned char)m[6])) {
        int n = atoi(m.c_str() + 6);
        if (m.find("Special") != std::string::npos) return false;
        b.kind = 2;
        b.channel = n / 2;   // TeknoParrot counts analog bytes, two per channel
        return true;
    }
    return false;
}

bool ControlsLoad(const std::wstring& xmlPath) {
    g_binds.clear();
    g_on = false;
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    HANDLE f = CreateFile2FromAppW(xmlPath.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
#else
    HANDLE f = CreateFileW(xmlPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
#endif
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string s;
    char buf[8192];
    DWORD n;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) s.append(buf, n);
    CloseHandle(f);

    std::vector<Bind> binds;
    std::vector<std::string> skipped;
    size_t at = 0;
    while ((at = s.find("<ButtonName>", at)) != std::string::npos) {
        size_t next = s.find("<ButtonName>", at + 1);
        if (next == std::string::npos) next = s.size();
        Bind b;
        b.name = Tag(s, at, next, "ButtonName");
        std::string map = Tag(s, at, next, "InputMapping"), type = Tag(s, at, next, "AnalogType");
        at = next;
        if (Place(map, b)) {
            if (b.kind == 2) {
                b.reverse = type.find("Reverse") != std::string::npos;
                b.trigger = type == "Gas" || type == "Brake";
                b.name += "|" + type;
            }
            binds.push_back(b);
        } else if (!map.empty() && map != "Coin1" && map != "Coin2") {
            skipped.push_back(b.name + " (" + map + ")");
        }
    }
    // --- the rules ---
    uint32_t used = P_Back | P_Start;   // View = coin / with Menu = Test / with LB = Service
    bool rtUsed = false, ltUsed = false, leftStick = false, rightStick = false;
    auto lname = [](const Bind& b) { return Lower(b.name.substr(0, b.name.find('|'))); };
    bool hasFire = false;
    for (auto& b : binds)
        if (b.kind == 1 && b.player == 1) {
            std::string l = lname(b);
            if (l.find("trigger") != std::string::npos || l.find("fire") != std::string::npos ||
                l.find("shoot") != std::string::npos || l.find("gun") != std::string::npos)
                hasFire = true;
        }
    // analog first
    for (auto& b : binds) {
        if (b.kind != 2) continue;
        std::string type = b.name.substr(b.name.find('|') + 1), l = lname(b);
        if (type == "Wheel" && !leftStick) b.src = S_LX, leftStick = true;
        else if (type == "Gas") {
            if (hasFire && !ltUsed) b.src = S_LT, ltUsed = true;
            else if (!rtUsed) b.src = S_RT, rtUsed = true;
            else if (!ltUsed) b.src = S_LT, ltUsed = true;
        } else if (type == "Brake") {
            if (!ltUsed) b.src = S_LT, ltUsed = true;
            else if (!rtUsed) b.src = S_RT, rtUsed = true;
        } else {   // joystick axes, by pairs: X then Y
            bool isY = l.find(" y") != std::string::npos || (l.size() && l.back() == 'y') || l.find("vertical") != std::string::npos ||
                       l.find("up") != std::string::npos;
            int want = !rightStick && leftStick && type.find("Joystick") != std::string::npos && !isY ? 1 : 0;
            static bool lxDone, lyDone, rxDone, ryDone;
            lxDone = lyDone = rxDone = ryDone = false;
            for (auto& o : binds) {
                if (&o == &b) break;
                if (o.src == S_LX) lxDone = true;
                if (o.src == S_LY) lyDone = true;
                if (o.src == S_RX) rxDone = true;
                if (o.src == S_RY) ryDone = true;
            }
            (void)want;
            if (isY) b.src = !lyDone ? S_LY : !ryDone ? S_RY : S_None;
            else b.src = !lxDone ? S_LX : !rxDone ? S_RX : S_None;
            if (b.src == S_LX || b.src == S_LY) leftStick = true;
        }
    }
    if (rtUsed) used |= 0;   // (triggers as analog: not free for buttons)
    auto take = [&](uint32_t p) {
        if (used & p) return false;
        used |= p;
        return true;
    };
    auto takeTrigger = [&](bool right) {
        bool& u = right ? rtUsed : ltUsed;
        if (u) return false;
        u = true;
        return true;
    };
    for (auto& b : binds) {
        if (b.kind != 1 || b.player == 2) continue;   // (one pad: player 2 not mapped yet)
        if (b.player == 0) continue;                  // Test: View + Menu
        if (b.byteIdx == 0 && b.bit == 0x40) continue;   // Service: View + LB
        if (b.byteIdx == 0 && b.bit == 0x80) { b.src = P_Start; continue; }
        if (b.byteIdx == 0 && b.bit == 0x20) { b.src = P_Up; continue; }
        if (b.byteIdx == 0 && b.bit == 0x10) { b.src = P_Down; continue; }
        if (b.byteIdx == 0 && b.bit == 0x08) { b.src = P_Left; continue; }
        if (b.byteIdx == 0 && b.bit == 0x04) { b.src = P_Right; continue; }
    }
    for (auto& b : binds) {
        if (b.kind != 1 || b.player != 1 || b.src) continue;
        if (b.byteIdx == 0 && (b.bit & 0xFC)) continue;
        std::string l = lname(b);
        if ((l.find("trigger") != std::string::npos || l.find("fire") != std::string::npos ||
             l.find("shoot") != std::string::npos || l.find("gun") != std::string::npos) && takeTrigger(true)) {
            b.src = S_RT;
            continue;
        }
        if (l.find("view") != std::string::npos && take(P_Y)) {
            b.src = P_Y;
            continue;
        }
    }
    static const uint32_t order[] = {P_A, P_B, P_X, P_Y, P_RB, P_LB};
    for (auto& b : binds) {
        if (b.kind != 1 || b.player != 1 || b.src) continue;
        if (b.byteIdx == 0 && (b.bit & 0xFC)) continue;
        for (uint32_t p : order)
            if (take(p)) {
                b.src = (int)p;
                break;
            }
        if (!b.src && takeTrigger(true)) b.src = S_RT;
        if (!b.src && takeTrigger(false)) b.src = S_LT;
        if (!b.src && take(P_R3)) b.src = P_R3;
        if (!b.src && take(P_L3)) b.src = P_L3;
    }
    g_binds = binds;
    g_on = true;
    g_stickIsDpad = true;
    for (auto& b : g_binds)
        if (b.src == S_LX || b.src == S_LY) g_stickIsDpad = false;
    std::string line;
    for (auto& b : g_binds) {
        if (b.kind == 1 && (b.player == 0 || (b.byteIdx == 0 && b.bit == 0x40) || !b.src)) continue;
        line += (line.empty() ? "" : ", ") + b.name.substr(0, b.name.find('|')) + " = " + SrcName(b.src);
    }
    Log("controls (from %ls): %s; coin = View, Test = View+Menu, Service = View+LB%s", xmlPath.c_str(), line.c_str(),
        g_stickIsDpad ? "; left stick = D-pad too" : "");
    for (auto& k : skipped) Log("controls: %s not fed yet", k.c_str());
    return true;
}
bool ControlsOn() { return g_on; }

static uint8_t Axis8(int16_t v, bool flip) {
    int x = ((int)v + 32768) >> 8;
    return (uint8_t)(flip ? 255 - x : x);
}
// the board's switch bytes for 'players' players of 'per' bytes, after the system byte the caller wrote
void ControlsSwitches(const PadIn& in, int players, int per, std::vector<uint8_t>& r) {
    PadIn p = in;
    if (g_stickIsDpad) {   // left stick past half way = the D-pad
        if (p.ly > 16384) p.buttons |= P_Up;
        if (p.ly < -16384) p.buttons |= P_Down;
        if (p.lx < -16384) p.buttons |= P_Left;
        if (p.lx > 16384) p.buttons |= P_Right;
    }
    for (int pl = 1; pl <= players; pl++) {
        uint8_t by[4] = {0, 0, 0, 0};
        for (auto& b : g_binds) {
            if (b.kind != 1 || b.player != pl || !b.src || b.byteIdx >= 4) continue;
            bool down = b.src == S_RT ? p.rt > 64 : b.src == S_LT ? p.lt > 64 : b.src < 0x10000 && (p.buttons & b.src);
            if (down) by[b.byteIdx] |= b.bit;
        }
        if (pl == 1 && p.service) by[0] |= 0x40;
        for (int i = 0; i < per; i++) r.push_back(i < 4 ? by[i] : 0);
    }
}
void ControlsAnalog(const PadIn& p, int channels, std::vector<uint8_t>& r) {
    for (int c = 0; c < channels; c++) {
        uint8_t v = 0x80;
        for (auto& b : g_binds) {
            if (b.kind != 2 || b.channel != c || !b.src) continue;
            switch (b.src) {
            case S_LT: v = b.reverse ? 255 - p.lt : p.lt; break;
            case S_RT: v = b.reverse ? 255 - p.rt : p.rt; break;
            case S_LX: v = Axis8(p.lx, b.reverse); break;
            case S_LY: v = Axis8(p.ly, b.reverse); break;
            case S_RX: v = Axis8(p.rx, b.reverse); break;
            case S_RY: v = Axis8(p.ry, b.reverse); break;
            }
        }
        r.push_back(v);
        r.push_back(0x00);
    }
}

}  // namespace arcade
