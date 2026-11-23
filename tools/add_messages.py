# one-off edit: window messages for 32-bit games (window_xbox.cpp): the game's window procedure gets the messages a
# desktop sends a new window, and its own posted ones
from pathlib import Path
src = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src")
def edit(name, pairs):
    p = src / name
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert s.count(a) == 1, (name, a[:60])
        s = s.replace(a, b)
    p.write_text(s, encoding="utf-8")
edit("peload32.cpp", [("static bool HasState32() {", "bool InProgram32();\nstatic bool HasState32() {")])
s = (src / "peload32.cpp").read_text(encoding="utf-8")
i = s.index("static bool HasState32() {")
j = s.index("\n}\n", i) + 3
s = s[:j] + "bool InProgram32() { return HasState32(); }   // this thread is running the 32-bit program\n" + s[j:]
(src / "peload32.cpp").write_text(s, encoding="utf-8")
edit("peload.h", [("void SetProfilesFolder(", "bool InProgram32();   // peload32.cpp: called from the 32-bit program right now\nvoid SetProfilesFolder(")])
edit("window_xbox.cpp", [
("""// window classes: accepted (the window procedure is not called: no window messages on an Xbox)
static ATOM WINAPI S_RegisterClassExW(const void*) { return 0xC001; }
static ATOM WINAPI S_RegisterClassExA(const void*) { return 0xC001; }
static ATOM WINAPI S_RegisterClassW(const void*) { return 0xC001; }
static ATOM WINAPI S_RegisterClassA(const void*) { return 0xC001; }
static HWND WINAPI S_CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int w, int h, HWND, HMENU, HINSTANCE, LPVOID) {
    Log("window made (%dx%d asked): it is the whole screen, %dx%d", w, h, g_width, g_height);
    return kWindow;
}
static HWND WINAPI S_CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int w, int h, HWND, HMENU, HINSTANCE, LPVOID) {
    Log("window made (%dx%d asked): it is the whole screen, %dx%d", w, h, g_width, g_height);
    return kWindow;
}""", """// --- window messages. A 64-bit game: none (its loop goes on without them). A 32-bit game: its window procedure (from
// its window class) gets what a desktop sends a new window - shown, activated, focused, sized - and the messages it
// posts itself (some games wait for these before they start: Ford Racing).
static uint64_t g_proc = 0;      // the game's window procedure
static bool g_proc32 = false;    // ... is 32-bit code
struct QMsg {
    UINT msg;
    uint64_t w, l;
};
static std::mutex g_qLock;
static std::deque<QMsg> g_q;
static void Post(UINT m, uint64_t w, uint64_t l) {
    std::lock_guard<std::mutex> lk(g_qLock);
    g_q.push_back({m, w, l});
}
static LRESULT CallProc(UINT m, uint64_t w, uint64_t l) {
    if (!g_proc) return 0;
    if (g_proc32) {
        uint32_t a[4] = {(uint32_t)(uintptr_t)kWindow, m, (uint32_t)w, (uint32_t)l};
        return (LRESULT)(int32_t)Call32((uint32_t)g_proc, a, 4);
    }
    return ((WNDPROC)g_proc)(kWindow, m, (WPARAM)w, (LPARAM)l);
}
// window classes: the procedure kept (32-bit WNDCLASS[EX]A: procedure at +4 / +8; 64-bit at +8 both)
static ATOM Class(const void* c, bool ex) {
    if (c && InProgram32()) {
        g_proc = *(const uint32_t*)((const uint8_t*)c + (ex ? 8 : 4));
        g_proc32 = true;
        Log("window class: 32-bit window procedure 0x%08x (messages on)", (uint32_t)g_proc);
    }
    return 0xC001;
}
static ATOM WINAPI S_RegisterClassExW(const void* c) { return Class(c, true); }
static ATOM WINAPI S_RegisterClassExA(const void* c) { return Class(c, true); }
static ATOM WINAPI S_RegisterClassW(const void* c) { return Class(c, false); }
static ATOM WINAPI S_RegisterClassA(const void* c) { return Class(c, false); }
static void Made(int w, int h) {
    Log("window made (%dx%d asked): it is the whole screen, %dx%d", w, h, g_width, g_height);
    if (!g_proc32) return;
    Post(WM_SHOWWINDOW, 1, 0);
    Post(WM_ACTIVATEAPP, 1, 0);
    Post(WM_ACTIVATE, WA_ACTIVE, 0);
    Post(WM_SETFOCUS, 0, 0);
    Post(WM_SIZE, SIZE_RESTORED, (uint64_t)(uint32_t)MAKELPARAM(g_width, g_height));
}
static HWND WINAPI S_CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int w, int h, HWND, HMENU, HINSTANCE, LPVOID) {
    Made(w, h);
    return kWindow;
}
static HWND WINAPI S_CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int w, int h, HWND, HMENU, HINSTANCE, LPVOID) {
    Made(w, h);
    return kWindow;
}
// a message into the game's MSG (32-bit: 28 bytes, 64-bit: 48)
static void FillMsg(void* out, const QMsg& q) {
    if (!out) return;
    if (InProgram32()) {
        uint32_t* m = (uint32_t*)out;
        m[0] = (uint32_t)(uintptr_t)kWindow, m[1] = q.msg, m[2] = (uint32_t)q.w, m[3] = (uint32_t)q.l;
        m[4] = GetTickCount(), m[5] = m[6] = 0;
    } else {
        MSG* m = (MSG*)out;
        memset(m, 0, sizeof *m);
        m->hwnd = kWindow, m->message = q.msg, m->wParam = (WPARAM)q.w, m->lParam = (LPARAM)q.l, m->time = GetTickCount();
    }
}
static bool TakeMsg(QMsg& q, bool remove) {
    std::lock_guard<std::mutex> lk(g_qLock);
    if (g_q.empty()) return false;
    q = g_q.front();
    if (remove) g_q.pop_front();
    return true;
}"""),
("""static BOOL WINAPI S_PeekMessage(void*, HWND, UINT, UINT, UINT) { return FALSE; }
static BOOL WINAPI S_GetMessage(void* m, HWND, UINT, UINT) {
    Sleep(16);   // no messages: the game's loop goes on
    if (m) memset(m, 0, 48);   // sizeof(MSG) on x64
    return TRUE;
}
static BOOL WINAPI S_TranslateMessage(const void*) { return FALSE; }
static LRESULT WINAPI S_DispatchMessage(const void*) { return 0; }""", """static BOOL WINAPI S_PeekMessage(void* m, HWND, UINT, UINT, UINT flags) {
    QMsg q;
    if (!TakeMsg(q, (flags & PM_REMOVE) != 0)) return FALSE;
    FillMsg(m, q);
    return TRUE;
}
static BOOL WINAPI S_GetMessage(void* m, HWND, UINT, UINT) {
    QMsg q;
    for (int i = 0; !TakeMsg(q, true); i++) {
        if (!g_proc32) {   // (64-bit games: as before, an empty message now and then)
            Sleep(16);
            q = {0, 0, 0};
            break;
        }
        Sleep(i < 10 ? 1 : 10);
    }
    FillMsg(m, q);
    return q.msg != WM_QUIT;
}
static BOOL WINAPI S_TranslateMessage(const void*) { return FALSE; }
static LRESULT WINAPI S_DispatchMessage(const void* m) {
    if (!m || !g_proc32) return 0;
    const uint32_t* q = (const uint32_t*)m;   // a 32-bit game's MSG
    return CallProc(q[1], q[2], q[3]);
}"""),
("""static BOOL WINAPI S_PostMessage(HWND, UINT, WPARAM, LPARAM) { return TRUE; }
static void WINAPI S_PostQuitMessage(int code) { Log("the game asked to quit (%d)", code); }""", """static BOOL WINAPI S_PostMessage(HWND, UINT m, WPARAM w, LPARAM l) {
    if (g_proc32) Post(m, (uint64_t)(uint32_t)w, (uint64_t)(uint32_t)l);
    return TRUE;
}
static void WINAPI S_PostQuitMessage(int code) {
    Log("the game asked to quit (%d)", code);
    if (g_proc32) Post(WM_QUIT, (uint64_t)(uint32_t)code, 0);
}"""),
("""static LRESULT WINAPI S_SendMessage(HWND, UINT, WPARAM, LPARAM) { return 0; }""", """static LRESULT WINAPI S_SendMessage(HWND, UINT m, WPARAM w, LPARAM l) {
    return g_proc32 ? CallProc(m, (uint64_t)(uint32_t)w, (uint64_t)(uint32_t)l) : 0;
}"""),
])
print("ok")
