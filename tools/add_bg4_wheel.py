# one-off edit: Battle Gear 4's steering drive board on COM1 (2-byte commands, each answered by 2 bytes; protocol as
# njz3/windowsloader taito.cpp and OpenParrot TypeX2Generic.cpp give it, GPL-3.0) and the Tuned pause flag
# (OpenParrot BG4General: once screen +0x4e3668 is 8, +0x42d29c held at 1)
from pathlib import Path
import re

p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\arcade_io.cpp")
s = p.read_text(encoding="utf-8")

# the handle and the device
a = "static const HANDLE kJvsHandle = (HANDLE)(uintptr_t)0x0A5C3A00;"
assert s.count(a) == 1
s = s.replace(a, a + r'''
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
''')

# generic comm functions: ours = either device
s = s.replace("return h == kJvsHandle ? TRUE :", "return IsOurs(h) ? TRUE :")
s = s.replace("return h == kJvsHandle ? FILE_TYPE_CHAR :", "return IsOurs(h) ? FILE_TYPE_CHAR :")
for fn in ("GetCommState", "GetCommTimeouts", "PurgeComm", "GetCommMask", "GetOverlappedResult"):
    pat = 'if (h != kJvsHandle) return Real<'
    i = s.index("static BOOL WINAPI S_%s(" % fn)
    j = s.index(pat, i)
    s = s[:j] + 'if (!IsOurs(h)) return Real<' + s[j + len(pat):]
s = s.replace("    if (h == kJvsHandle) return TRUE;\n", "    if (IsOurs(h)) return TRUE;\n", 1)   # CloseHandle

# WriteFile / ReadFile / ClearCommError / GetCommModemStatus / WaitCommEvent: the wheel's own
s = s.replace('''static BOOL WINAPI S_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (h != kJvsHandle)''', '''static BOOL WINAPI S_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
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
    if (h != kJvsHandle)''', 1)
s = s.replace('''static BOOL WINAPI S_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {''',
              '''static BOOL WINAPI S_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (h == kWheelHandle) {
        std::lock_guard<std::mutex> l(g_wheelLock);
        DWORD k = 0;
        while (k < n && !g_wheelOut.empty()) ((uint8_t*)b)[k++] = g_wheelOut.front(), g_wheelOut.pop_front();
        if (done) *done = k;
        if (ov) ov->InternalHigh = k, ov->Internal = 0;
        return TRUE;
    }''', 1)
s = s.replace('''static BOOL WINAPI S_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st) {''',
              '''static BOOL WINAPI S_ClearCommError(HANDLE h, LPDWORD err, LPCOMSTAT st) {
    if (h == kWheelHandle) {
        if (err) *err = 0;
        if (st) {
            memset(st, 0, sizeof *st);
            std::lock_guard<std::mutex> l(g_wheelLock);
            st->cbInQue = (DWORD)g_wheelOut.size();
        }
        return TRUE;
    }''', 1)
s = s.replace('''static BOOL WINAPI S_GetCommModemStatus(HANDLE h, LPDWORD s) {''',
              '''static BOOL WINAPI S_GetCommModemStatus(HANDLE h, LPDWORD s) {
    if (h == kWheelHandle) {
        *s = MS_CTS_ON | MS_DSR_ON;
        return TRUE;
    }''', 1)
s = s.replace('''static BOOL WINAPI S_WaitCommEvent(HANDLE h, LPDWORD m, LPOVERLAPPED ov) {''',
              '''static BOOL WINAPI S_WaitCommEvent(HANDLE h, LPDWORD m, LPOVERLAPPED ov) {
    if (h == kWheelHandle) {   // (an answer is always there right after a command)
        if (m) *m = EV_RXCHAR;
        return TRUE;
    }''', 1)

# opening COM1 on a Battle Gear cabinet
for W, T in (("A", "LPCSTR"), ("W", "LPCWSTR")):
    i = s.index("static HANDLE WINAPI S_CreateFile%s(" % W)
    j = s.index("{", i) + 1
    if W == "A":
        add = '''
    if (g_profile && g_profile->battleGear && name && (!_stricmp(name, "COM1") || !_stricmp(name, "\\\\\\\\.\\\\COM1"))) {
        Log("steering board: the game opened %s (emulated drive board)", name);
        return kWheelHandle;
    }'''
    else:
        add = '''
    if (g_profile && g_profile->battleGear && name && (!_wcsicmp(name, L"COM1") || !_wcsicmp(name, L"\\\\\\\\.\\\\COM1"))) {
        Log("steering board: the game opened COM1 (emulated drive board)");
        return kWheelHandle;
    }'''
    s = s[:j] + add + s[j:]

# steering from the pad, and the pause flag each JVS poll
a = "// coin button presses so far (View alone)"
assert s.count(a) == 1
s = s.replace(a, '''static int WheelSteering() {   // the left stick (or the triggers' profile wheel): 0 left .. 255 right
    int v = (Pad().lx + 32768) >> 8;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}
static void Bg4TunedTick() {
    __try {
        if (*(volatile uint8_t*)0x8E3668 == 8) *(volatile uint8_t*)0x82D29C = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
''' + a)
a = '''    case 0x22: {   // analog
        report();
        if (p.battleGear) {'''
assert s.count(a) == 1
s = s.replace(a, '''    case 0x22: {   // analog
        report();
        if (p.battleGear && p.crc == 0x93c2bf69) {   // (Tuned: once its screen +0x4e3668 is 8, its pause flag held at 1)
            Bg4TunedTick();
        }
        if (p.battleGear) {''')
p.write_text(s, encoding="utf-8")
print("ok", s.count("IsOurs(h)"))
