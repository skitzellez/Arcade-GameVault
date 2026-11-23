// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// Xbox only: an app has no desktop windows. A game's window becomes our one CoreWindow (the whole TV): window calls
// answer as if the window were there and full screen, and a Direct3D 11 swap chain made for that window is made for
// the CoreWindow instead (flip model: at least 2 buffers).

#include "peload.h"
#include <deque>
#include <mutex>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace arcade {

static IUnknown* g_coreWindow = nullptr;
static int g_width = 1920, g_height = 1080;
static const HWND kWindow = (HWND)(uintptr_t)0x0A5CADE0;   // the game's one window (below 2 GB: also a 32-bit program's handle)

void SetCoreWindow(IUnknown* w, int width, int height) {
    if (w) w->AddRef();
    g_coreWindow = w;
    if (width > 0) g_width = width;
    if (height > 0) g_height = height;
}

void SetGameScreen(int width, int height) {
    if (width > 0 && height > 0) g_width = width, g_height = height;
}

// --- window messages. A 64-bit game: none (its loop goes on without them). A 32-bit game: its window procedure (from
// its window class) gets what a desktop sends a new window - shown, activated, focused, sized - and the messages it
// posts itself (some games wait for these before they start: Ford Racing).
#ifndef WM_ACTIVATEAPP   // (an app's headers leave out the desktop window messages)
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WA_ACTIVE 1
#define SIZE_RESTORED 0
#define PM_REMOVE 1
#endif
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
    return ((LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM))g_proc)(kWindow, m, (WPARAM)w, (LPARAM)l);
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
        uint8_t* m = (uint8_t*)out;   // MSG: hwnd, message, wParam, lParam, time, pt (x64 layout)
        memset(m, 0, 48);
        *(HWND*)m = kWindow;
        *(UINT*)(m + 8) = q.msg;
        *(uint64_t*)(m + 16) = q.w;
        *(uint64_t*)(m + 24) = q.l;
        *(DWORD*)(m + 32) = GetTickCount();
    }
}
static bool TakeMsg(QMsg& q, bool remove) {
    std::lock_guard<std::mutex> lk(g_qLock);
    if (g_q.empty()) return false;
    q = g_q.front();
    if (remove) g_q.pop_front();
    return true;
}
static BOOL WINAPI S_True1(HWND) { return TRUE; }
static BOOL WINAPI S_ShowWindow(HWND, int) { return TRUE; }
static BOOL WINAPI S_PeekMessage(void* m, HWND, UINT, UINT, UINT flags) {
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
}
static LRESULT WINAPI S_DefWindowProc(HWND, UINT, WPARAM, LPARAM) { return 0; }
static BOOL WINAPI S_GetClientRect(HWND, LPRECT r) {
    if (r) *r = {0, 0, g_width, g_height};
    return TRUE;
}
static BOOL WINAPI S_GetWindowRect(HWND, LPRECT r) {
    if (r) *r = {0, 0, g_width, g_height};
    return TRUE;
}
static BOOL WINAPI S_AdjustWindowRect(LPRECT, DWORD, BOOL) { return TRUE; }
static BOOL WINAPI S_AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }
static BOOL WINAPI S_SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
static BOOL WINAPI S_MoveWindow(HWND, int, int, int, int, BOOL) { return TRUE; }
static HANDLE WINAPI S_LoadNull2(HINSTANCE, LPCVOID) { return nullptr; }
static HCURSOR WINAPI S_SetCursor(HCURSOR) { return nullptr; }
static int WINAPI S_ShowCursor(BOOL show) { return show ? 0 : -1; }
static int WINAPI S_GetSystemMetrics(int i) {
    switch (i) {
        case 0: case 16: case 78: return g_width;    // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
        case 1: case 17: case 79: return g_height;   // SM_CYSCREEN, SM_CYFULLSCREEN, SM_CYVIRTUALSCREEN
        default: return 0;
    }
}
static BOOL WINAPI S_SetWindowTextW(HWND, LPCWSTR) { return TRUE; }
static BOOL WINAPI S_SetWindowTextA(HWND, LPCSTR) { return TRUE; }
static HWND WINAPI S_GetWindow0() { return kWindow; }
static HWND WINAPI S_SetFocus(HWND) { return kWindow; }
static BOOL WINAPI S_ClipCursor(const RECT*) { return TRUE; }
static BOOL WINAPI S_GetCursorPos(LPPOINT p) {
    if (p) *p = {g_width / 2, g_height / 2};
    return TRUE;
}
static BOOL WINAPI S_SetCursorPos(int, int) { return TRUE; }
static BOOL WINAPI S_ScreenToClient(HWND, LPPOINT) { return TRUE; }
static BOOL WINAPI S_ClientToScreen(HWND, LPPOINT) { return TRUE; }
static LONG_PTR WINAPI S_SetWindowLongPtr(HWND, int, LONG_PTR) { return 0; }
static LONG_PTR WINAPI S_GetWindowLongPtr(HWND, int) { return 0; }
static BOOL WINAPI S_IsWindow(HWND h) { return h == kWindow; }
static BOOL WINAPI S_PostMessage(HWND, UINT m, WPARAM w, LPARAM l) {
    if (g_proc32) Post(m, (uint64_t)(uint32_t)w, (uint64_t)(uint32_t)l);
    return TRUE;
}
static void WINAPI S_PostQuitMessage(int code) {
    Log("the game asked to quit (%d)", code);
    if (g_proc32) Post(WM_QUIT, (uint64_t)(uint32_t)code, 0);
}
static HDC WINAPI S_GetDC(HWND) { return nullptr; }
static int WINAPI S_ReleaseDC(HWND, HDC) { return 1; }


// more window calls a game makes (pacman.c / sokol_app, 2026-11-20): answered as for one full-screen window
static const HMONITOR kMonitor = (HMONITOR)(uintptr_t)0x0A5C0FE0;
static BOOL WINAPI S_IsIconic(HWND) { return FALSE; }
static SHORT WINAPI S_GetKeyState(int) { return 0; }
static UINT_PTR WINAPI S_SetTimer(HWND, UINT_PTR id, UINT, void*) { return id ? id : 1; }
static BOOL WINAPI S_KillTimer(HWND, UINT_PTR) { return TRUE; }
static HWND WINAPI S_SetCapture(HWND) { return nullptr; }
static BOOL WINAPI S_ReleaseCapture() { return TRUE; }
static HWND WINAPI S_WindowFromPoint(POINT) { return kWindow; }
static BOOL WINAPI S_PtInRect(const RECT* r, POINT p) {
    return r && p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}
static BOOL WINAPI S_DestroyIcon(HICON) { return TRUE; }
static HANDLE WINAPI S_LoadImage(HINSTANCE, LPCVOID, UINT, int, int, UINT) { return nullptr; }
static HICON WINAPI S_CreateIconIndirect(void*) { return nullptr; }
static HMONITOR WINAPI S_MonitorFromPoint(POINT, DWORD) { return kMonitor; }
static HMONITOR WINAPI S_MonitorFromWindow(HWND, DWORD) { return kMonitor; }
// MONITORINFO: cbSize, rcMonitor, rcWork, dwFlags (MONITORINFOEX adds the device name, the one EnumDisplaySettings
// takes)
static void MonitorInfo(void* mi) {
    DWORD size = *(DWORD*)mi;
    RECT* r = (RECT*)((char*)mi + 4);
    r[0] = {0, 0, g_width, g_height};
    r[1] = {0, 0, g_width, g_height};
    *(DWORD*)((char*)mi + 36) = 1;   // MONITORINFOF_PRIMARY
    if (size > 40) memset((char*)mi + 40, 0, size - 40);
}
static BOOL WINAPI S_GetMonitorInfoW(HMONITOR, void* mi) {
    if (!mi) return FALSE;
    MonitorInfo(mi);
    if (*(DWORD*)mi >= 40 + 32 * sizeof(wchar_t)) wcscpy_s((wchar_t*)((char*)mi + 40), 32, L"\\\\.\\DISPLAY1");
    return TRUE;
}
static BOOL WINAPI S_GetMonitorInfoA(HMONITOR, void* mi) {
    if (!mi) return FALSE;
    MonitorInfo(mi);
    if (*(DWORD*)mi >= 40 + 32) strcpy_s((char*)mi + 40, 32, "\\\\.\\DISPLAY1");
    return TRUE;
}
typedef BOOL(CALLBACK* MonitorEnum_t)(HMONITOR, HDC, LPRECT, LPARAM);
static BOOL WINAPI S_EnumDisplayMonitors(HDC, LPCRECT, MonitorEnum_t fn, LPARAM data) {
    RECT r = {0, 0, g_width, g_height};
    if (fn) fn(kMonitor, nullptr, &r, data);
    return TRUE;
}
// display modes: the screen's own size first, then the usual PC sizes a game may look for (the swap chain is
// stretched to the TV whatever the game picks)
#ifndef ENUM_CURRENT_SETTINGS   // (desktop-only in the app headers)
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif
static const int kModes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 768}, {1280, 1024},
                                {1360, 768}, {1366, 768}, {1600, 900}, {1920, 1080}};
static bool ModeAt(DWORD i, int& w, int& h) {
    if (i == ENUM_CURRENT_SETTINGS || i == ENUM_REGISTRY_SETTINGS) {
        w = g_width, h = g_height;
        return true;
    }
    if (i >= sizeof kModes / sizeof kModes[0]) return false;
    w = kModes[i][0], h = kModes[i][1];
    return true;
}
static void FillMode(DEVMODEW* m, int w, int h) {
    WORD size = m->dmSize ? m->dmSize : sizeof(DEVMODEW);
    memset((char*)m + offsetof(DEVMODEW, dmSpecVersion), 0, size - offsetof(DEVMODEW, dmSpecVersion));
    m->dmSize = size;
    m->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
    m->dmBitsPerPel = 32;
    m->dmPelsWidth = w;
    m->dmPelsHeight = h;
    m->dmDisplayFrequency = 60;
}
static BOOL WINAPI S_EnumDisplaySettingsW(LPCWSTR, DWORD i, DEVMODEW* m) {
    int w, h;
    if (!m || !ModeAt(i, w, h)) return FALSE;
    FillMode(m, w, h);
    return TRUE;
}
static BOOL WINAPI S_EnumDisplaySettingsExW(LPCWSTR dev, DWORD i, DEVMODEW* m, DWORD) { return S_EnumDisplaySettingsW(dev, i, m); }
static BOOL WINAPI S_EnumDisplaySettingsA(LPCSTR, DWORD i, DEVMODEA* m) {
    int w, h;
    if (!m || !ModeAt(i, w, h)) return FALSE;
    WORD size = m->dmSize ? m->dmSize : sizeof(DEVMODEA);
    memset((char*)m + offsetof(DEVMODEA, dmSpecVersion), 0, size - offsetof(DEVMODEA, dmSpecVersion));
    m->dmSize = size;
    m->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    m->dmBitsPerPel = 32;
    m->dmPelsWidth = w;
    m->dmPelsHeight = h;
    m->dmDisplayFrequency = 60;
    return TRUE;
}
static LONG WINAPI S_ChangeDisplaySettings(const void*, DWORD) { return DISP_CHANGE_SUCCESSFUL; }
static LONG WINAPI S_ChangeDisplaySettingsEx(const void*, const void*, HWND, DWORD, void*) { return DISP_CHANGE_SUCCESSFUL; }
// one display adapter, one screen
static BOOL WINAPI S_EnumDisplayDevicesA(LPCSTR dev, DWORD i, DISPLAY_DEVICEA* d, DWORD) {
    if (i != 0 || !d) return FALSE;
    DWORD cb = d->cb ? d->cb : sizeof(DISPLAY_DEVICEA);
    memset(d, 0, cb);
    d->cb = cb;
    strcpy_s(d->DeviceName, dev ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1");
    strcpy_s(d->DeviceString, "Xbox");
    d->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE | 1 /*ACTIVE*/;
    return TRUE;
}
static BOOL WINAPI S_EnumDisplayDevicesW(LPCWSTR dev, DWORD i, DISPLAY_DEVICEW* d, DWORD) {
    if (i != 0 || !d) return FALSE;
    DWORD cb = d->cb ? d->cb : sizeof(DISPLAY_DEVICEW);
    memset(d, 0, cb);
    d->cb = cb;
    wcscpy_s(d->DeviceName, dev ? L"\\\\.\\DISPLAY1\\Monitor0" : L"\\\\.\\DISPLAY1");
    wcscpy_s(d->DeviceString, L"Xbox");
    d->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE | 1;
    return TRUE;
}
// window procedures are called directly (there are no window messages; a library hooking the window proc calls on)
typedef LRESULT(CALLBACK* WndProc_t)(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI S_CallWindowProc(WndProc_t fn, HWND w, UINT m, WPARAM a, LPARAM b) { return fn ? fn(w, m, a, b) : 0; }
static BOOL WINAPI S_IsWindowUnicode(HWND) { return TRUE; }
static BOOL WINAPI S_IsWindowVisible(HWND h) { return h == kWindow; }
static BOOL WINAPI S_DestroyCursor(HCURSOR) { return TRUE; }
// GDI: a few calls for cursor pictures (no desktop to draw on)
static HANDLE WINAPI S_GdiHandle0() { return (HANDLE)(uintptr_t)0xA5D00001; }
static BOOL WINAPI S_GdiTrue1(HANDLE) { return TRUE; }

// Mesa's Vulkan driver (Dozen) gives the host every presented frame (MesaUwpHostPresent): a swap chain Dozen makes on
// the CoreWindow with Direct3D 12 stays black on the Xbox, so the frame goes to the screen through a Direct3D 11 swap
// chain of ours (the way that shows). One copy per frame, sized as the game's swap chain (stretched to the TV).
static ID3D11Device* g_hostDev = nullptr;
static ID3D11DeviceContext* g_hostCtx = nullptr;
static IDXGISwapChain1* g_hostSc = nullptr;
static int g_hostW = 0, g_hostH = 0;
static DXGI_FORMAT g_hostFmt = DXGI_FORMAT_UNKNOWN;
extern "C" __declspec(dllexport) void MesaUwpHostPresent(const void* pixels, int pitch, int width, int height, int vkFormat) {
    if (!pixels || width <= 0 || height <= 0 || !g_coreWindow) return;
    {   // (testing) frame rate and the slowest frames, every 5 s
        static LARGE_INTEGER freq, last, start;
        static int frames = 0, slow = 0;
        static double worst = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) {
            QueryPerformanceFrequency(&freq);
            start = last = now;
        }
        double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        last = now;
        frames++;
        if (ms > worst) worst = ms;
        if (ms > 50) slow++;
        double span = (now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        if (span >= 5) {
            Log("frames: %.1f fps, slowest %.0f ms, %d frames over 50 ms", frames / span, worst, slow);
            frames = slow = 0;
            worst = 0;
            start = now;
        }
    }
    DXGI_FORMAT fmt = vkFormat == 44 /*VK_FORMAT_B8G8R8A8_UNORM*/ ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!g_hostDev) {
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                       0, D3D11_SDK_VERSION, &g_hostDev, &fl, &g_hostCtx);
        Log("screen output for Vulkan frames: Direct3D 11 device 0x%08lx", (unsigned long)hr);
        if (FAILED(hr)) return;
    }
    if (!g_hostSc || width != g_hostW || height != g_hostH || fmt != g_hostFmt) {
        if (g_hostSc) {
            g_hostSc->Release();
            g_hostSc = nullptr;
            g_hostCtx->ClearState();
            g_hostCtx->Flush();
        }
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        g_hostDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
        if (dxgiDev) dxgiDev->GetAdapter(&adapter);
        if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = width;
        d.Height = height;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        HRESULT hr = factory ? factory->CreateSwapChainForCoreWindow(g_hostDev, g_coreWindow, &d, nullptr, &g_hostSc) : E_FAIL;
        Log("screen output for Vulkan frames: swap chain %dx%d format %d: 0x%08lx", width, height, (int)fmt, (unsigned long)hr);
        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();
        if (FAILED(hr)) {
            g_hostSc = nullptr;
            return;
        }
        g_hostW = width, g_hostH = height, g_hostFmt = fmt;
    }
    {   // (testing) what arrives: the frame's centre pixel and how many of a row of samples are not black
        static int n = 0;
        if (n++ % 120 == 5) {
            const uint8_t* p = (const uint8_t*)pixels;
            uint32_t centre = *(const uint32_t*)(p + (size_t)(height / 2) * pitch + (width / 2) * 4);
            int lit = 0;
            for (int x = 0; x < width; x += 16) lit += *(const uint32_t*)(p + (size_t)(height / 2) * pitch + x * 4) & 0xFFFFFF ? 1 : 0;
            Log("screen output: frame %d centre 0x%08x, %d of %d samples lit, pitch %d", n, centre, lit, width / 16, pitch);
        }
    }
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_hostSc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        g_hostCtx->UpdateSubresource(back, 0, nullptr, pixels, (UINT)pitch, 0);
        back->Release();
    }
    g_hostSc->Present(1, 0);
}

// Mesa's Vulkan driver (Dozen) asks the host for its window and screen size: our UI thread waits while a game runs,
// so Mesa cannot look them up there itself (deps/mesa-arcade, util/uwp_util.cpp)
extern "C" __declspec(dllexport) void* MesaUwpHostWindow(int* width, int* height) {
    if (width) *width = g_width;
    if (height) *height = g_height;
    return g_coreWindow;
}
static UINT WINAPI S_GetRawInputData(void*, UINT, void*, UINT*, UINT) { return (UINT)-1; }
static BOOL WINAPI S_RegisterRawInputDevices(const void*, UINT, UINT) { return TRUE; }
static LRESULT WINAPI S_SendMessage(HWND, UINT m, WPARAM w, LPARAM l) {
    return g_proc32 ? CallProc(m, (uint64_t)(uint32_t)w, (uint64_t)(uint32_t)l) : 0;
}
static BOOL WINAPI S_TrackMouseEvent(void*) { return TRUE; }
static BOOL WINAPI S_UnregisterClass(LPCWSTR, HINSTANCE) { return TRUE; }
// shell32 (not in an app)
static void WINAPI S_DragFinish(void*) {}
static UINT WINAPI S_DragQueryFileW(void*, UINT, LPWSTR, UINT) { return 0; }
static void WINAPI S_DragAcceptFiles(HWND, BOOL) {}
static LPWSTR* WINAPI S_CommandLineToArgvW(LPCWSTR cmd, int* argc) {
    // the usual rules: spaces split, double quotes group (enough for game command lines)
    std::vector<std::wstring> args;
    std::wstring cur;
    bool q = false, any = false;
    for (const wchar_t* c = cmd ? cmd : L""; *c; c++) {
        if (*c == L'"') { q = !q; any = true; continue; }
        if (!q && (*c == L' ' || *c == L'\t')) {
            if (any || !cur.empty()) args.push_back(cur);
            cur.clear(); any = false;
            continue;
        }
        cur += *c;
    }
    if (any || !cur.empty()) args.push_back(cur);
    size_t bytes = (args.size() + 1) * sizeof(LPWSTR);
    for (auto& a : args) bytes += (a.size() + 1) * sizeof(wchar_t);
    LPWSTR* out = (LPWSTR*)LocalAlloc(LMEM_FIXED, bytes);   // freed by the game with LocalFree, as the real one
    if (!out) return nullptr;
    wchar_t* text = (wchar_t*)(out + args.size() + 1);
    for (size_t i = 0; i < args.size(); i++) {
        out[i] = text;
        memcpy(text, args[i].c_str(), (args[i].size() + 1) * sizeof(wchar_t));
        text += args[i].size() + 1;
    }
    out[args.size()] = nullptr;
    if (argc) *argc = (int)args.size();
    return out;
}

// --- swap chains: every way a game makes one ends at CreateSwapChainForCoreWindow -----------------------------
static void HookSwapChain(IDXGISwapChain1* sc);
static HRESULT MakeCoreSwapChain(IDXGIFactory2* fac, IUnknown* dev, DXGI_SWAP_CHAIN_DESC1 d, IDXGISwapChain1** out) {
    if (!d.Width) d.Width = g_width;
    if (!d.Height) d.Height = g_height;
    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // (B8G8R8A8 stays: sokol asks for that format for its render target view; the console screenshot fails on it)
    d.SampleDesc.Count = 1;   // flip model: no multisampled swap chains
    d.SampleDesc.Quality = 0;
    d.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
    if (d.BufferCount < 2) d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    d.Flags &= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    HRESULT hr = fac && g_coreWindow ? fac->CreateSwapChainForCoreWindow(dev, g_coreWindow, &d, nullptr, out) : E_FAIL;
    Log("swap chain for the screen (%ux%u, %u buffers, format %d): 0x%08lx", d.Width, d.Height, d.BufferCount,
        (int)d.Format, (unsigned long)hr);
    if (SUCCEEDED(hr)) HookSwapChain(*out);
    return hr;
}
static DXGI_SWAP_CHAIN_DESC1 Desc1From(const DXGI_SWAP_CHAIN_DESC* desc) {
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = desc->BufferDesc.Width;
    d.Height = desc->BufferDesc.Height;
    d.Format = desc->BufferDesc.Format;
    d.BufferUsage = desc->BufferUsage;
    d.BufferCount = desc->BufferCount;
    return d;
}

// vtable slots (dxgi.h order): IDXGIFactory 8 MakeWindowAssociation, 10 CreateSwapChain; IDXGIFactory2 15
// CreateSwapChainForHwnd. IDXGISwapChain 10 SetFullscreenState, 11 GetFullscreenState, 14 ResizeTarget.
typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static HRESULT STDMETHODCALLTYPE H_MakeWindowAssociation(IDXGIFactory*, HWND, UINT) { return S_OK; }
static HRESULT STDMETHODCALLTYPE H_CreateSwapChain(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc,
                                                   IDXGISwapChain** out) {
    if (!desc || !out) return E_INVALIDARG;
    IDXGIFactory2* f2 = nullptr;
    self->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2);
    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = MakeCoreSwapChain(f2, dev, Desc1From(desc), &sc);
    if (f2) f2->Release();
    *out = sc;
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                          IDXGISwapChain1** out) {
    if (!desc || !out) return E_INVALIDARG;
    return MakeCoreSwapChain(self, dev, *desc, out);
}
static HRESULT STDMETHODCALLTYPE H_SetFullscreenState(IDXGISwapChain*, BOOL, IDXGIOutput*) { return S_OK; }
static HRESULT STDMETHODCALLTYPE H_GetFullscreenState(IDXGISwapChain*, BOOL* fs, IDXGIOutput** out) {
    if (fs) *fs = TRUE;   // the game is the whole screen
    if (out) *out = nullptr;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE H_ResizeTarget(IDXGISwapChain*, const DXGI_MODE_DESC*) { return S_OK; }
// testing: frame 600 of the game saved as a picture (LocalState\frame.bmp), for when the console's own screenshot
// cannot read the swap chain (B8G8R8A8)
static std::wstring g_shotPath;
void SetShotPath(const std::wstring& path) { g_shotPath = path; }
static void SaveFrame(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* stage = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &stage))) {
        ctx->CopyResource(stage, back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
            HANDLE f = CreateFile2FromAppW(g_shotPath.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
                uint32_t w = d.Width, h = d.Height, row = w * 4, size = 54 + row * h;
                uint8_t hdr[54] = {'B', 'M'};
                memcpy(hdr + 2, &size, 4);
                uint32_t off = 54, dib = 40, planesBpp = 1 | (32 << 16), img = row * h;
                int32_t hh = -(int32_t)h;
                memcpy(hdr + 10, &off, 4);
                memcpy(hdr + 14, &dib, 4);
                memcpy(hdr + 18, &w, 4);
                memcpy(hdr + 22, &hh, 4);
                memcpy(hdr + 26, &planesBpp, 4);
                memcpy(hdr + 34, &img, 4);
                DWORD n;
                WriteFile(f, hdr, 54, &n, nullptr);
                std::vector<uint8_t> line(row);
                for (uint32_t y = 0; y < h; y++) {
                    memcpy(line.data(), (uint8_t*)m.pData + y * m.RowPitch, row);
                    if (rgba)
                        for (uint32_t x = 0; x < w; x++) std::swap(line[x * 4], line[x * 4 + 2]);
                    WriteFile(f, line.data(), row, &n, nullptr);
                }
                CloseHandle(f);
                Log("frame saved: %ux%u format %d", w, h, (int)d.Format);
            }
            ctx->Unmap(stage, 0);
        }
        stage->Release();
    }
    ctx->Release();
    dev->Release();
    back->Release();
}
// frames shown: the game's Present, counted (logged at 1, 60 and every 600 frames)
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
static Present_t g_present = nullptr;
static std::atomic<uint64_t> g_frames{0};
static HRESULT STDMETHODCALLTYPE H_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    uint64_t n = ++g_frames;
    if (n == 600 && !g_shotPath.empty()) SaveFrame(sc);
    HRESULT hr = g_present(sc, sync, flags);
    if (n <= 3 || n % 120 == 0) Log("frames shown: %llu (last Present 0x%08lx)", (unsigned long long)n, (unsigned long)hr);
    return hr;
}

static bool PatchSlot(void** vtbl, int slot, void* fn) {
    ULONG old;
    if (!VirtualProtectFromApp(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) {
        Log("vtable slot %d: no write access (%lu)", slot, GetLastError());
        return false;
    }
    vtbl[slot] = fn;
    VirtualProtectFromApp(&vtbl[slot], sizeof(void*), old, &old);
    return true;
}
static void HookSwapChain(IDXGISwapChain1* sc) {
    static void** done = nullptr;
    void** vtbl = *(void***)sc;
    if (vtbl == done) return;
    done = vtbl;
    g_present = (Present_t)vtbl[8];
    bool ok = PatchSlot(vtbl, 10, (void*)H_SetFullscreenState) & PatchSlot(vtbl, 11, (void*)H_GetFullscreenState) &
              PatchSlot(vtbl, 14, (void*)H_ResizeTarget) & PatchSlot(vtbl, 8, (void*)H_Present);
    Log("swap chain full screen calls %s", ok ? "answered by us" : "NOT hooked");
}
void InstallDxgiHooks() {
    IDXGIFactory2* fac = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&fac)) || !fac) {
        Log("no DXGI factory for the hooks");
        return;
    }
    void** vtbl = *(void***)fac;
    bool ok = PatchSlot(vtbl, 8, (void*)H_MakeWindowAssociation) & PatchSlot(vtbl, 10, (void*)H_CreateSwapChain) &
              PatchSlot(vtbl, 15, (void*)H_CreateSwapChainForHwnd);
    Log("DXGI factory swap chain calls %s", ok ? "go to the screen" : "NOT hooked");
    fac->Release();
}

// --- Direct3D 11: the swap chain goes to the CoreWindow ------------------------------------------------------
static HRESULT WINAPI S_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                                      const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk,
                                                      const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap,
                                                      ID3D11Device** devOut, D3D_FEATURE_LEVEL* got,
                                                      ID3D11DeviceContext** ctxOut) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter, type, sw, flags & ~D3D11_CREATE_DEVICE_DEBUG, levels, nlevels, sdk, &dev, got, &ctx);
    Log("D3D11 device: 0x%08lx", (unsigned long)hr);
    if (FAILED(hr)) return hr;
    if (swap && desc) {
        *swap = nullptr;
        IDXGIDevice* xdev = nullptr;
        IDXGIAdapter* xad = nullptr;
        IDXGIFactory2* fac = nullptr;
        dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xdev);
        if (xdev) xdev->GetAdapter(&xad);
        if (xad) xad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
        IDXGISwapChain1* sc = nullptr;
        hr = MakeCoreSwapChain(fac, dev, Desc1From(desc), &sc);
        if (fac) fac->Release();
        if (xad) xad->Release();
        if (xdev) xdev->Release();
        if (FAILED(hr)) {
            ctx->Release();
            dev->Release();
            return hr;
        }
        *swap = sc;
    }
    if (devOut) *devOut = dev; else dev->Release();
    if (ctxOut) *ctxOut = ctx; else ctx->Release();
    return S_OK;
}

struct WinShim {
    const char* name;
    void* fn;
};
static const WinShim kWin[] = {
    {"RegisterClassExW", (void*)S_RegisterClassExW}, {"RegisterClassExA", (void*)S_RegisterClassExA},
    {"RegisterClassW", (void*)S_RegisterClassW}, {"RegisterClassA", (void*)S_RegisterClassA},
    {"CreateWindowExW", (void*)S_CreateWindowExW}, {"CreateWindowExA", (void*)S_CreateWindowExA},
    {"ShowWindow", (void*)S_ShowWindow}, {"UpdateWindow", (void*)S_True1}, {"DestroyWindow", (void*)S_True1},
    {"SetForegroundWindow", (void*)S_True1}, {"PeekMessageW", (void*)S_PeekMessage},
    {"PeekMessageA", (void*)S_PeekMessage}, {"GetMessageW", (void*)S_GetMessage}, {"GetMessageA", (void*)S_GetMessage},
    {"TranslateMessage", (void*)S_TranslateMessage}, {"DispatchMessageW", (void*)S_DispatchMessage},
    {"DispatchMessageA", (void*)S_DispatchMessage}, {"DefWindowProcW", (void*)S_DefWindowProc},
    {"DefWindowProcA", (void*)S_DefWindowProc}, {"GetClientRect", (void*)S_GetClientRect},
    {"GetWindowRect", (void*)S_GetWindowRect}, {"AdjustWindowRect", (void*)S_AdjustWindowRect},
    {"AdjustWindowRectEx", (void*)S_AdjustWindowRectEx}, {"SetWindowPos", (void*)S_SetWindowPos},
    {"MoveWindow", (void*)S_MoveWindow}, {"LoadCursorW", (void*)S_LoadNull2}, {"LoadCursorA", (void*)S_LoadNull2},
    {"LoadIconW", (void*)S_LoadNull2}, {"LoadIconA", (void*)S_LoadNull2}, {"SetCursor", (void*)S_SetCursor},
    {"ShowCursor", (void*)S_ShowCursor}, {"GetSystemMetrics", (void*)S_GetSystemMetrics},
    {"SetWindowTextW", (void*)S_SetWindowTextW}, {"SetWindowTextA", (void*)S_SetWindowTextA},
    {"GetForegroundWindow", (void*)S_GetWindow0}, {"GetActiveWindow", (void*)S_GetWindow0},
    {"GetDesktopWindow", (void*)S_GetWindow0}, {"SetFocus", (void*)S_SetFocus}, {"ClipCursor", (void*)S_ClipCursor},
    {"GetCursorPos", (void*)S_GetCursorPos}, {"SetCursorPos", (void*)S_SetCursorPos},
    {"ScreenToClient", (void*)S_ScreenToClient}, {"ClientToScreen", (void*)S_ClientToScreen},
    {"SetWindowLongPtrW", (void*)S_SetWindowLongPtr}, {"SetWindowLongPtrA", (void*)S_SetWindowLongPtr},
    {"GetWindowLongPtrW", (void*)S_GetWindowLongPtr}, {"GetWindowLongPtrA", (void*)S_GetWindowLongPtr},
    {"SetWindowLongW", (void*)S_SetWindowLongPtr}, {"GetWindowLongW", (void*)S_GetWindowLongPtr},
    {"IsWindow", (void*)S_IsWindow}, {"PostMessageW", (void*)S_PostMessage}, {"PostMessageA", (void*)S_PostMessage},
    {"PostQuitMessage", (void*)S_PostQuitMessage}, {"GetDC", (void*)S_GetDC}, {"ReleaseDC", (void*)S_ReleaseDC},
    {"IsIconic", (void*)S_IsIconic}, {"GetKeyState", (void*)S_GetKeyState}, {"GetAsyncKeyState", (void*)S_GetKeyState},
    {"SetTimer", (void*)S_SetTimer}, {"KillTimer", (void*)S_KillTimer}, {"SetCapture", (void*)S_SetCapture},
    {"ReleaseCapture", (void*)S_ReleaseCapture}, {"WindowFromPoint", (void*)S_WindowFromPoint},
    {"PtInRect", (void*)S_PtInRect}, {"DestroyIcon", (void*)S_DestroyIcon}, {"LoadImageW", (void*)S_LoadImage},
    {"LoadImageA", (void*)S_LoadImage}, {"CreateIconIndirect", (void*)S_CreateIconIndirect},
    {"MonitorFromPoint", (void*)S_MonitorFromPoint}, {"MonitorFromWindow", (void*)S_MonitorFromWindow},
    {"GetMonitorInfoA", (void*)S_GetMonitorInfoA}, {"GetMonitorInfoW", (void*)S_GetMonitorInfoW},
    {"EnumDisplayMonitors", (void*)S_EnumDisplayMonitors}, {"EnumDisplaySettingsW", (void*)S_EnumDisplaySettingsW},
    {"EnumDisplaySettingsA", (void*)S_EnumDisplaySettingsA}, {"EnumDisplaySettingsExW", (void*)S_EnumDisplaySettingsExW},
    {"ChangeDisplaySettingsW", (void*)S_ChangeDisplaySettings}, {"ChangeDisplaySettingsA", (void*)S_ChangeDisplaySettings},
    {"ChangeDisplaySettingsExW", (void*)S_ChangeDisplaySettingsEx}, {"ChangeDisplaySettingsExA", (void*)S_ChangeDisplaySettingsEx},
    {"EnumDisplayDevicesA", (void*)S_EnumDisplayDevicesA}, {"EnumDisplayDevicesW", (void*)S_EnumDisplayDevicesW},
    {"CallWindowProcW", (void*)S_CallWindowProc}, {"CallWindowProcA", (void*)S_CallWindowProc},
    {"IsWindowUnicode", (void*)S_IsWindowUnicode}, {"IsWindowVisible", (void*)S_IsWindowVisible},
    {"DestroyCursor", (void*)S_DestroyCursor},
    {"GetRawInputData", (void*)S_GetRawInputData}, {"RegisterRawInputDevices", (void*)S_RegisterRawInputDevices},
    {"SendMessageA", (void*)S_SendMessage}, {"SendMessageW", (void*)S_SendMessage},
    {"TrackMouseEvent", (void*)S_TrackMouseEvent}, {"UnregisterClassW", (void*)S_UnregisterClass},
    {"UnregisterClassA", (void*)S_UnregisterClass},
};

void* FindWindowShim(const char* dll, const char* func) {
    if (!_stricmp(dll, "d3d11.dll") && !strcmp(func, "D3D11CreateDeviceAndSwapChain"))
        return (void*)S_D3D11CreateDeviceAndSwapChain;
    if (!_stricmp(dll, "shell32.dll")) {
        if (!strcmp(func, "CommandLineToArgvW")) return (void*)S_CommandLineToArgvW;
        if (!strcmp(func, "DragFinish")) return (void*)S_DragFinish;
        if (!strcmp(func, "DragQueryFileW")) return (void*)S_DragQueryFileW;
        if (!strcmp(func, "DragAcceptFiles")) return (void*)S_DragAcceptFiles;
        return nullptr;
    }
    if (!_stricmp(dll, "gdi32.dll")) {
        if (!strcmp(func, "CreateBitmap") || !strcmp(func, "CreateCompatibleDC")) return (void*)S_GdiHandle0;
        if (!strcmp(func, "DeleteDC") || !strcmp(func, "DeleteObject")) return (void*)S_GdiTrue1;
        return nullptr;
    }
    if (_stricmp(dll, "user32.dll")) return nullptr;
    for (const WinShim& s : kWin)
        if (!strcmp(s.name, func)) return s.fn;
    return nullptr;
}

}  // namespace arcade
