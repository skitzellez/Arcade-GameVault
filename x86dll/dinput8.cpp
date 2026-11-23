// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// dinput8.dll for 32-bit programs (built 32-bit, loaded by the 32-bit bridge from the package's x86 folder):
// DirectInput 8 with no game controllers, a mouse that reports nothing and a keyboard that is the Xbox pad (through
// gvio.dll, the loader's): stick/d-pad = W A S D and the arrows, A B X Y = J K U I (and Z X C V), Menu = Enter and 1,
// View + Menu = F1 (test), View + LB = Space (service). Konami's Contra: Evolution reads these keys without its I/O
// board (player 1: W S A D, U J K I, Enter).
#include <windows.h>
#include <string.h>
#include <cstring>

namespace {
struct Obj {
    void** vtbl;
    LONG refs;
    int kind;   // 0 the DirectInput object / the mouse, 1 the keyboard
};
struct PadState {
    unsigned short buttons;
    unsigned char lt, rt;
    short lx, ly, rx, ry;
};
typedef DWORD(WINAPI* padread_t)(PadState*);
padread_t PadRead() {
    static padread_t f = [] {
        HMODULE m = LoadLibraryA("gvio.dll");
        return m ? (padread_t)GetProcAddress(m, "GvPadRead") : (padread_t)nullptr;
    }();
    return f;
}
// the keyboard as the pad makes it: 256 keys, 0x80 = down
void PadKeys(unsigned char* k) {
    memset(k, 0, 256);
    PadState p = {};
    if (!PadRead() || !PadRead()(&p)) return;
    unsigned b = p.buttons;
    bool view = (b & 0x20) != 0;
    auto set = [&](bool on, unsigned char a, unsigned char c) {
        if (on) k[a] = 0x80, k[c] = 0x80;
    };
    set((b & 1) || p.ly > 16000, 0x11, 0xC8);    // up: W, Up
    set((b & 2) || p.ly < -16000, 0x1F, 0xD0);   // down: S, Down
    set((b & 4) || p.lx < -16000, 0x1E, 0xCB);   // left: A, Left
    set((b & 8) || p.lx > 16000, 0x20, 0xCD);    // right: D, Right
    set((b & 0x1000) != 0, 0x24, 0x2C);          // A: J, Z
    set((b & 0x2000) != 0, 0x25, 0x2D);          // B: K, X
    set((b & 0x4000) != 0, 0x16, 0x2E);          // X: U, C
    set((b & 0x8000) != 0, 0x17, 0x2F);          // Y: I, V
    set((b & 0x10) && !view, 0x1C, 0x02);        // Menu: Enter, 1
    set(view && (b & 0x10), 0x3B, 0x3B);         // View + Menu: F1
    set(view && (b & 0x100), 0x39, 0x39);        // View + LB: Space
}
HRESULT STDMETHODCALLTYPE QI(Obj* self, REFIID, void** out) {
    if (!out) return E_POINTER;
    InterlockedIncrement(&self->refs);
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE AddRef(Obj* self) { return (ULONG)InterlockedIncrement(&self->refs); }
ULONG STDMETHODCALLTYPE Release(Obj* self) {
    LONG n = InterlockedDecrement(&self->refs);
    if (n <= 0) delete self;
    return n < 0 ? 0 : (ULONG)n;
}
// methods with 0..5 arguments that only answer OK / "not available" (stdcall: each its own argument count)
HRESULT STDMETHODCALLTYPE Ok0(Obj*) { return S_OK; }
HRESULT STDMETHODCALLTYPE Ok1(Obj*, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE Ok2(Obj*, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE Ok3(Obj*, DWORD, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE Ok4(Obj*, DWORD, DWORD, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE Ok5(Obj*, DWORD, DWORD, DWORD, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE No1(Obj*, DWORD) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE No2(Obj*, DWORD, DWORD) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE No3(Obj*, DWORD, DWORD, DWORD) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE No4(Obj*, DWORD, DWORD, DWORD, DWORD) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE Dev_GetCapabilities(Obj*, DWORD* caps) {
    if (!caps) return E_POINTER;
    DWORD size = caps[0];
    memset(caps + 1, 0, size > 4 ? size - 4 : 0);
    caps[1] = 0x1;   // DIDC_ATTACHED
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Dev_GetProperty(Obj*, DWORD, DWORD* prop) {
    if (prop && prop[0] > 16) memset((char*)prop + 16, 0, prop[0] - 16);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Dev_GetDeviceState(Obj* self, DWORD size, void* data) {
    if (!data) return E_POINTER;
    memset(data, 0, size);
    if (self->kind == 1 && size >= 256) PadKeys((unsigned char*)data);
    static bool told = false;
    if (!told && self->kind == 1) told = true, OutputDebugStringA("DirectInput 8: the game reads the keyboard");
    return S_OK;
}
// buffered: each key that changed since the last call (DIDEVICEOBJECTDATA, 20 bytes on 32-bit: offset, data, time,
// sequence, app data)
HRESULT STDMETHODCALLTYPE Dev_GetDeviceData(Obj* self, DWORD objSize, void* out, DWORD* inOut, DWORD flags) {
    if (!inOut) return E_POINTER;
    if (self->kind != 1) {
        *inOut = 0;
        return S_OK;
    }
    static unsigned char was[256];
    static DWORD seq = 0;
    static bool told = false;
    if (!told) told = true, OutputDebugStringA("DirectInput 8: the game reads keyboard events");
    unsigned char now[256];
    PadKeys(now);
    DWORD room = *inOut, n = 0;
    for (int i = 0; i < 256 && n < room; i++) {
        if (now[i] == was[i]) continue;
        if (out && objSize >= 16) {
            DWORD* d = (DWORD*)((char*)out + n * objSize);
            memset(d, 0, objSize);
            d[0] = (DWORD)i, d[1] = now[i], d[2] = GetTickCount(), d[3] = ++seq;
        }
        if (!(flags & 1 /*DIGDD_PEEK*/)) was[i] = now[i];
        n++;
    }
    *inOut = n;
    return S_OK;
}
// DIDEVICEINSTANCEA (dwSize first): a keyboard / mouse with a name
HRESULT STDMETHODCALLTYPE Dev_GetDeviceInfo(Obj*, DWORD* info) {
    if (!info) return E_POINTER;
    DWORD size = info[0];
    if (size < 16) return E_INVALIDARG;
    memset(info + 1, 0, size - 4);
    if (size >= 0x2C) info[0x24 / 4] = 0x13;            // dwDevType: DI8DEVTYPE_KEYBOARD (the mouse says so too)
    if (size >= 0x130) strcpy((char*)info + 0x28, "Keyboard"), strcpy((char*)info + 0x28 + 260, "Keyboard");
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Dev_GetObjectInfo(Obj*, DWORD* info, DWORD, DWORD) {
    if (!info) return E_POINTER;
    DWORD size = info[0];
    if (size > 4) memset(info + 1, 0, size - 4);
    return S_OK;
}
void* g_dev[32] = {
    (void*)QI, (void*)AddRef, (void*)Release, (void*)Dev_GetCapabilities,
    (void*)Ok3 /*EnumObjects*/, (void*)Dev_GetProperty, (void*)Ok2 /*SetProperty*/, (void*)Ok0 /*Acquire*/,
    (void*)Ok0 /*Unacquire*/, (void*)Dev_GetDeviceState, (void*)Dev_GetDeviceData, (void*)Ok1 /*SetDataFormat*/,
    (void*)Ok1 /*SetEventNotification*/, (void*)Ok2 /*SetCooperativeLevel*/, (void*)Dev_GetObjectInfo /*GetObjectInfo*/,
    (void*)Dev_GetDeviceInfo /*GetDeviceInfo*/, (void*)Ok2 /*RunControlPanel*/, (void*)Ok3 /*Initialize*/,
    (void*)No4 /*CreateEffect*/, (void*)Ok3 /*EnumEffects*/, (void*)No2 /*GetEffectInfo*/,
    (void*)No1 /*GetForceFeedbackState*/, (void*)No1 /*SendForceFeedbackCommand*/,
    (void*)Ok3 /*EnumCreatedEffectObjects*/, (void*)No1 /*Escape*/, (void*)Ok0 /*Poll*/,
    (void*)Ok4 /*SendDeviceData*/, (void*)Ok4 /*EnumEffectsInFile*/, (void*)No4 /*WriteEffectToFile*/,
    (void*)No3 /*BuildActionMap*/, (void*)No3 /*SetActionMap*/, (void*)No1 /*GetImageInfo*/,
};
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
HRESULT STDMETHODCALLTYPE Di_CreateDevice(Obj*, REFGUID guid, void** out, void*) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsEqualGUID(guid, kSysKeyboard) && !IsEqualGUID(guid, kSysMouse)) return 0x80040154;   // DIERR_DEVICENOTREG
    *out = new Obj{g_dev, 1, IsEqualGUID(guid, kSysKeyboard) ? 1 : 0};
    OutputDebugStringA(IsEqualGUID(guid, kSysKeyboard) ? "DirectInput 8: keyboard made (the pad)" : "DirectInput 8: mouse made");
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Di_EnumDevices(Obj*, DWORD type, void*, void*, DWORD) {   // none
    char m[64] = "DirectInput 8: the game lists devices of type 00 (none)";
    m[46] = (char)('0' + type / 10 % 10), m[47] = (char)('0' + type % 10);
    OutputDebugStringA(m);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Di_GetDeviceStatus(Obj*, REFGUID) { return 1; }                     // DI_NOTATTACHED
void* g_di[11] = {
    (void*)QI, (void*)AddRef, (void*)Release, (void*)Di_CreateDevice, (void*)Di_EnumDevices,
    (void*)Di_GetDeviceStatus, (void*)Ok2 /*RunControlPanel*/, (void*)Ok2 /*Initialize*/, (void*)No3 /*FindDevice*/,
    (void*)Ok5 /*EnumDevicesBySemantics*/, (void*)Ok4 /*ConfigureDevices*/,
};
}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD version, REFIID, void** out, void*) {
    if (!out) return E_POINTER;
    *out = new Obj{g_di, 1};
    OutputDebugStringA(version ? "DirectInput 8: stand-in (no controllers; keyboard/mouse idle)" : "DirectInput 8");
    return S_OK;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, void** out) {
    if (out) *out = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, void*) { return TRUE; }
