# one-off edit: JVS switches/analog from the Xbox pad (XInput pad 1)
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "arcade_io.cpp"
s = p.read_text(encoding="utf-8")


def rep(a, b):
    global s
    assert a in s, a[:60]
    s = s.replace(a, b, 1)


rep('''// one command of a package: its reply bytes''', r'''// --- the player's controls: Xbox pad 1 (XInput) -> JVS --------------------------------------------------------
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
static PadState Pad() {
    struct {
        DWORD packet;
        PadState g;
    } st = {};
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
    r.push_back(test ? 0x80 : 0x00);
    for (int pl = 0; pl < players; pl++) {
        uint8_t s1 = 0, s2 = 0, s3 = 0;
        if (pl == 0) {
            if ((b & kStart) && !test) s1 |= 0x80;
            if ((b & kBack) && !test) s1 |= 0x40;   // Service
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
        const uint8_t bytes[3] = {s1, s2, s3};
        for (int i = 0; i < per; i++) r.push_back(i < 3 ? bytes[i] : 0);
    }
}
static void JvsAnalog(int channels, std::vector<uint8_t>& r) {
    PadState p = Pad();
    const uint8_t v[8] = {Stick8(p.lx, false), Stick8(p.ly, true), Stick8(p.rx, false), Stick8(p.ry, true), p.lt, p.rt,
                          0x80, 0x80};
    for (int i = 0; i < channels; i++) {
        r.push_back(i < 8 ? v[i] : 0x80);
        r.push_back(0x00);
    }
}

// one command of a package: its reply bytes''')
rep('''        r.push_back(0x00);   // test/tilt
        int players = left > 1 ? c[1] : 2, per = left > 2 ? c[2] : 2;
        for (int i = 0; i < players * per; i++) r.push_back(0x00);
        return 3;''', '''        JvsSwitches(left > 1 ? c[1] : 2, left > 2 ? c[2] : 2, r);
        return 3;''')
rep('''    case 0x22: {   // analog: centred
        report();
        int ch = left > 1 ? c[1] : 8;
        for (int i = 0; i < ch; i++) {
            r.push_back(0x80);
            r.push_back(0x00);
        }
        return 2;''', '''    case 0x22: {   // analog
        report();
        JvsAnalog(left > 1 ? c[1] : 8, r);
        return 2;''')
# the XInput stand-in uses the same real function
rep('''    static XGet_t real = [] {
        HMODULE m = GetModuleHandleW(L"xinputuap.dll");
        if (!m) m = GetModuleHandleW(L"xinput1_4.dll");
#if !(defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP)
        if (!m) m = LoadLibraryW(L"xinput1_4.dll");
#endif
        return m ? (XGet_t)GetProcAddress(m, "XInputGetState") : nullptr;
    }();''', '''    XGet_t real = (XGet_t)RealXInputGetState();''')
p.write_text(s, encoding="utf-8")
print("ok")
