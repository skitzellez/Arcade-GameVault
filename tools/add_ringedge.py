# one-off edit: the RingEdge platform (ringedge.cpp) for Caladrius AC: profile, driver opens, DeviceIoControl,
# read/write/seek on the driver handles, the services started with the profile
from pathlib import Path

root = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader")
p = root / "src" / "arcade_io.cpp"
s = p.read_text(encoding="utf-8")


def rep(a, b, count=1):
    global s
    assert s.count(a) == count, a[:70]
    s = s.replace(a, b)


rep("    bool driveD = false;", "    bool driveD = false;\n    bool ringEdge = false;     // Sega RingEdge: its platform services and drivers (ringedge.cpp)")
rep('''    // Ford Racing Full Blown (Sega Europa-R, 2006)''', '''    // Caladrius AC (Sega RingEdge 2, 2013): amLib's services by PCP, its drivers; JVS board on COM4
    {L"SILVER_AMP_R_ROM.exe", "Caladrius AC", "COM4", false, "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-14572;Ver1.00;2005/10", 0x30, 0x10,
     0x13, 0x18, false, false, 0, false, 0, 0, false, L"CaladriusAC", false, false, false, true},
    // Ford Racing Full Blown (Sega Europa-R, 2006)''')
s = s.replace("namespace arcade {", '''namespace arcade {
HANDLE RingEdgeOpen(const char* name);   // ringedge.cpp
bool RingEdgeHandle(HANDLE h);
bool RingEdgeIoctl(HANDLE h, DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize, DWORD* ret);
bool RingEdgeRead(HANDLE h, void* b, DWORD n, DWORD* done);
bool RingEdgeWrite(HANDLE h, const void* b, DWORD n, DWORD* done);
bool RingEdgeSeek(HANDLE h, int64_t to, DWORD how, int64_t* now);
void RingEdgeStart(const std::wstring& gameDir, const char* gameId);''', 1)
# start with the profile
rep('''    if (g_profile)
        Log("game profile: %s (JVS %s, dongle %s)",''', '''    if (g_profile && g_profile->ringEdge) RingEdgeStart(exePath.substr(0, exePath.find_last_of(L"\\\\/") + 1), "SBWU");
    if (g_profile)
        Log("game profile: %s (JVS %s, dongle %s)",''')
# opens
i = s.index("static HANDLE WINAPI S_CreateFileA(")
j = s.index("{", i) + 1
s = s[:j] + '''
    if (HANDLE d = RingEdgeOpen(name)) return d;''' + s[j:]
i = s.index("static HANDLE WINAPI S_CreateFileW(")
j = s.index("{", i) + 1
s = s[:j] + '''
    if (name) {
        char a[64] = "";
        for (int k = 0; k < 63 && name[k]; k++) a[k] = (char)name[k], a[k + 1] = 0;
        if (HANDLE d = RingEdgeOpen(a)) return d;
    }''' + s[j:]
# read / write / close
rep('''static BOOL WINAPI S_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {''',
    '''static BOOL WINAPI S_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (RingEdgeWrite(h, b, n, done)) return TRUE;''')
rep('''static BOOL WINAPI S_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {''',
    '''static BOOL WINAPI S_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (RingEdgeRead(h, b, n, done)) return TRUE;''')
rep("    if (IsOurs(h)) return TRUE;\n", "    if (IsOurs(h) || RingEdgeHandle(h)) return TRUE;\n")
# DeviceIoControl, SetFilePointer(Ex)
rep('''static const IoShim kSerial[] = {''', '''static BOOL WINAPI S_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD ret,
                                     LPOVERLAPPED ov) {
    if (RingEdgeIoctl(h, code, in, inSize, out, outSize, ret)) return TRUE;
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
    {"SetFilePointerEx", (void*)S_SetFilePointerEx},''')
p.write_text(s, encoding="utf-8")

for f in ("build_pc.bat", "build_xbox.bat"):
    b = (root / f).read_text(encoding="utf-8")
    b = b.replace(" src\\gdi32.cpp ", " src\\gdi32.cpp src\\ringedge.cpp ", 1)
    (root / f).write_text(b, encoding="utf-8")
print("ok")
