// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// DirectInput 8 stand-in (dinput8.dll!DirectInput8Create): an Xbox app has no DirectInput, and the PC one refuses
// a module handle that is not a real Windows module. The stand-in lists no game controllers yet and gives the
// keyboard and mouse as devices with nothing pressed; the game's own I/O (arcade controls) comes through other
// stand-ins. COM objects by hand: one vtable for the A and W interfaces (they differ only in strings we never pass).

#include "peload.h"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace arcade {

typedef HRESULT(STDMETHODCALLTYPE* Any_t)(void*);
struct DiObject {
    void** vtbl;
    std::atomic<long> refs;
};

static HRESULT STDMETHODCALLTYPE Ok(void*) { return S_OK; }
static HRESULT STDMETHODCALLTYPE NotThere(void*) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE QI(DiObject* self, REFIID, void** out) {
    if (!out) return E_POINTER;
    self->refs++;
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE AddRef(DiObject* self) { return (ULONG)++self->refs; }
static ULONG STDMETHODCALLTYPE Release(DiObject* self) {
    long n = --self->refs;
    if (n <= 0) delete self;
    return (ULONG)(n < 0 ? 0 : n);
}

// --- IDirectInputDevice8: 32 methods ------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Dev_GetCapabilities(DiObject*, DWORD* caps) {
    // DIDEVCAPS: dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, ...
    if (!caps) return E_POINTER;
    DWORD size = caps[0];
    memset(caps + 1, 0, size > 4 ? size - 4 : 0);
    caps[1] = 0x1;   // DIDC_ATTACHED
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Dev_GetDeviceState(DiObject*, DWORD size, void* data) {
    if (data) memset(data, 0, size);   // nothing pressed, no movement
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Dev_GetDeviceData(DiObject*, DWORD, void*, DWORD* inOut, DWORD) {
    if (inOut) *inOut = 0;             // no buffered events
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Dev_GetProperty(DiObject*, REFGUID, DWORD* prop) {
    // DIPROPHEADER (dwSize, dwHeaderSize, dwObj, dwHow) + value: zero values
    if (prop && prop[0] > 16) memset((char*)prop + 16, 0, prop[0] - 16);
    return S_OK;
}
static void* g_devVtbl[32] = {
    (void*)QI, (void*)AddRef, (void*)Release, (void*)Dev_GetCapabilities,
    (void*)Ok /*EnumObjects*/, (void*)Dev_GetProperty, (void*)Ok /*SetProperty*/, (void*)Ok /*Acquire*/,
    (void*)Ok /*Unacquire*/, (void*)Dev_GetDeviceState, (void*)Dev_GetDeviceData, (void*)Ok /*SetDataFormat*/,
    (void*)Ok /*SetEventNotification*/, (void*)Ok /*SetCooperativeLevel*/, (void*)NotThere /*GetObjectInfo*/,
    (void*)NotThere /*GetDeviceInfo*/, (void*)Ok /*RunControlPanel*/, (void*)Ok /*Initialize*/,
    (void*)NotThere /*CreateEffect*/, (void*)Ok /*EnumEffects*/, (void*)NotThere /*GetEffectInfo*/,
    (void*)NotThere /*GetForceFeedbackState*/, (void*)NotThere /*SendForceFeedbackCommand*/,
    (void*)Ok /*EnumCreatedEffectObjects*/, (void*)NotThere /*Escape*/, (void*)Ok /*Poll*/,
    (void*)Ok /*SendDeviceData*/, (void*)Ok /*EnumEffectsInFile*/, (void*)NotThere /*WriteEffectToFile*/,
    (void*)NotThere /*BuildActionMap*/, (void*)NotThere /*SetActionMap*/, (void*)NotThere /*GetImageInfo*/,
};

// --- IDirectInput8: 11 methods --------------------------------------------------------------------------------
static const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static HRESULT STDMETHODCALLTYPE Di_CreateDevice(DiObject*, REFGUID guid, void** out, void*) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsEqualGUID(guid, kSysKeyboard) && !IsEqualGUID(guid, kSysMouse)) {
        Log("DirectInput: device asked for that is not there");
        return 0x80040154;   // DIERR_DEVICENOTREG
    }
    *out = new DiObject{g_devVtbl, {1}};
    Log("DirectInput: %s (stand-in, nothing pressed)", IsEqualGUID(guid, kSysKeyboard) ? "keyboard" : "mouse");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Di_EnumDevices(DiObject*, DWORD type, void*, void*, DWORD) {
    static bool said = false;
    if (!said) Log("DirectInput: devices of type %lu listed (none yet)", type);
    said = true;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Di_GetDeviceStatus(DiObject*, REFGUID) { return 1; /*DI_NOTATTACHED*/ }
static void* g_diVtbl[11] = {
    (void*)QI, (void*)AddRef, (void*)Release, (void*)Di_CreateDevice, (void*)Di_EnumDevices,
    (void*)Di_GetDeviceStatus, (void*)Ok /*RunControlPanel*/, (void*)Ok /*Initialize*/,
    (void*)NotThere /*FindDevice*/, (void*)Ok /*EnumDevicesBySemantics*/, (void*)Ok /*ConfigureDevices*/,
};

HRESULT WINAPI S_DirectInput8Create(HINSTANCE, DWORD version, REFIID, void** out, void*) {
    if (!out) return E_POINTER;
    *out = new DiObject{g_diVtbl, {1}};
    Log("DirectInput 8 (version 0x%lx): stand-in", version);
    return S_OK;
}

}  // namespace arcade
