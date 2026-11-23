// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// dsound.dll for 32-bit programs (built 32-bit, loaded by the 32-bit bridge from the package's x86 folder): DirectSound 8
// with a mixer. A thread mixes every playing buffer (8/16-bit PCM, mono/stereo; volume, pan, frequency, looping) into
// 16-bit stereo 48 kHz, 10 ms at a time, and hands it to gvaudio.dll (the loader's: an XAudio2 voice). Play positions
// are where the mixer is, so a game streaming music writes in step.
#include <windows.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
struct Buffer {
    void** vtbl;
    LONG refs;
    BYTE* data;
    DWORD size, flags;
    WAVEFORMATEX fmt;
    bool playing, looping;
    DWORD startTick, startPos, pos;
    LONG volume, pan;
    DWORD freq;
    double frame;   // the mixer's place (frames)
};
struct Sound {
    void** vtbl;
    LONG refs;
};
const DWORD kIUnknown = 0x00000000, kDSBuffer = 0x279AFA85, kDSBuffer8 = 0x6825A449, kDS = 0x279AFA83,
            kDS8 = 0xC50A7E93;

// --- the mixer --------------------------------------------------------------------------------------------------
CRITICAL_SECTION g_mix;
std::vector<Buffer*> g_playing;   // every secondary buffer (mixed while playing)
typedef BOOL(WINAPI* write_t)(const void*, DWORD);
typedef DWORD(WINAPI* queued_t)();
write_t g_write = nullptr;
queued_t g_queued = nullptr;
DWORD Align(const Buffer* b) { return b->fmt.nBlockAlign ? b->fmt.nBlockAlign : 1; }
DWORD Frames(const Buffer* b) { return b->size / Align(b); }
DWORD Position(Buffer* b) {
    EnterCriticalSection(&g_mix);
    DWORD p = b->playing ? (DWORD)b->frame * Align(b) : b->pos;
    LeaveCriticalSection(&g_mix);
    return b->size ? p % b->size : 0;
}
float Gain(LONG hundredthsDb) { return hundredthsDb <= -10000 ? 0.f : powf(10.f, hundredthsDb / 2000.f); }
void MixInto(float* lr, int n) {
    for (Buffer* b : g_playing) {
        if (!b->playing || !b->size || (b->fmt.wBitsPerSample != 8 && b->fmt.wBitsPerSample != 16)) continue;
        DWORD frames = Frames(b);
        if (!frames) continue;
        int ch = b->fmt.nChannels >= 2 ? 2 : 1;
        double step = (double)(b->freq ? b->freq : b->fmt.nSamplesPerSec) / 48000.0;
        float g = Gain(b->volume);
        float gl = g * (b->pan > 0 ? Gain(-b->pan) : 1.f), gr = g * (b->pan < 0 ? Gain(b->pan) : 1.f);
        for (int i = 0; i < n; i++) {
            DWORD f = (DWORD)b->frame;
            if (f >= frames) {
                if (!b->looping) {
                    b->playing = false;
                    b->pos = 0;
                    b->frame = 0;
                    break;
                }
                b->frame -= frames;
                f = (DWORD)b->frame;
            }
            const BYTE* at = b->data + f * Align(b);
            float l, r;
            if (b->fmt.wBitsPerSample == 16) {
                l = ((const short*)at)[0] / 32768.f;
                r = ch == 2 ? ((const short*)at)[1] / 32768.f : l;
            } else {
                l = (at[0] - 128) / 128.f;
                r = ch == 2 ? (at[1] - 128) / 128.f : l;
            }
            lr[i * 2] += l * gl;
            lr[i * 2 + 1] += r * gr;
            b->frame += step;
        }
    }
}
void NotifyTick();   // (below) notification events of playing buffers
void NotifyInit();
DWORD WINAPI MixThread(void*) {
    static float lr[480 * 2];
    static short out[480 * 2];
    for (;;) {
        if (g_queued && g_queued() >= 60) {   // ~60 ms waiting to be heard: enough
            Sleep(4);
            continue;
        }
        memset(lr, 0, sizeof lr);
        EnterCriticalSection(&g_mix);
        MixInto(lr, 480);   // 10 ms
        LeaveCriticalSection(&g_mix);
        NotifyTick();
        for (int i = 0; i < 960; i++) {
            float v = lr[i] * 32767.f;
            out[i] = (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
        }
        if (!g_write || !g_write(out, sizeof out)) Sleep(10);
    }
}
void StartMixer() {
    static bool started = false;
    if (started) return;
    started = true;
    InitializeCriticalSection(&g_mix);
    NotifyInit();
    if (HMODULE m = LoadLibraryA("gvaudio.dll")) {
        g_write = (write_t)GetProcAddress(m, "GvAudioWrite");
        g_queued = (queued_t)GetProcAddress(m, "GvAudioQueuedMs");
    }
    OutputDebugStringA(g_write ? "DirectSound 8: mixer -> XAudio2" : "DirectSound 8: no sound out (gvaudio.dll not there)");
    CreateThread(nullptr, 0, MixThread, nullptr, 0, nullptr);
}
template <typename T> ULONG STDMETHODCALLTYPE AddRefT(T* s) { return (ULONG)InterlockedIncrement(&s->refs); }
ULONG STDMETHODCALLTYPE B_Release(Buffer* b) {
    LONG n = InterlockedDecrement(&b->refs);
    if (n <= 0) {
        EnterCriticalSection(&g_mix);
        for (size_t i = 0; i < g_playing.size(); i++)
            if (g_playing[i] == b) {
                g_playing.erase(g_playing.begin() + i);
                break;
            }
        LeaveCriticalSection(&g_mix);
        delete[] b->data;
        delete b;
    }
    return n < 0 ? 0 : (ULONG)n;
}
// IDirectSound3DBuffer / IDirectSound3DListener / IDirectSoundNotify on a buffer: accepted, nothing done (no 3D
// placement; notification events set by the mixer while the buffer plays). Battle Gear 4's sound start asks for them.
struct Side {
    void** vtbl;
    LONG refs;
    Buffer* owner;
};
static void SayIid(const char* what, DWORD id) {   // "<what> 0123ABCD not there" to the loader's log
    char t[96];
    int n = 0;
    while (what[n] && n < 60) t[n] = what[n], n++;
    t[n++] = ' ';
    for (int i = 7; i >= 0; i--) t[n++] = "0123456789ABCDEF"[(id >> (i * 4)) & 15];
    const char* tail = " not there";
    for (int i = 0; tail[i]; i++) t[n++] = tail[i];
    t[n] = 0;
    OutputDebugStringA(t);
}
HRESULT STDMETHODCALLTYPE B_QI(Buffer* b, REFIID iid, void** out);
HRESULT STDMETHODCALLTYPE X_QI(Side* x, REFIID iid, void** out) { return B_QI(x->owner, iid, out); }
ULONG STDMETHODCALLTYPE X_AddRef(Side* x) { return (ULONG)InterlockedIncrement(&x->refs); }
ULONG STDMETHODCALLTYPE X_Release(Side* x) {
    LONG n = InterlockedDecrement(&x->refs);
    if (n == 0) delete x;
    return n < 0 ? 0 : (ULONG)n;
}
HRESULT STDMETHODCALLTYPE X0(Side*) { return S_OK; }
HRESULT STDMETHODCALLTYPE X1(Side*, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE X2(Side*, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE X3(Side*, DWORD, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE X4(Side*, DWORD, DWORD, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE X7(Side*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE X_Get1(Side*, DWORD* p) { if (p) *p = 0; return S_OK; }   // a DWORD / float out
HRESULT STDMETHODCALLTYPE X_GetVec(Side*, float* v) { if (v) v[0] = v[1] = v[2] = 0; return S_OK; }
HRESULT STDMETHODCALLTYPE X_GetFloat(Side*, float* f) { if (f) *f = 1.0f; return S_OK; }
HRESULT STDMETHODCALLTYPE X_GetAll(Side*, DWORD* p) { if (p && p[0] >= 4) memset(p + 1, 0, p[0] - 4); return S_OK; }
void* g_3dbuf[21] = {(void*)X_QI, (void*)X_AddRef, (void*)X_Release,
    (void*)X_GetAll, (void*)X2 /*GetConeAngles*/, (void*)X_GetVec /*GetConeOrientation*/, (void*)X_Get1 /*GetConeOutsideVolume*/,
    (void*)X_GetFloat /*GetMaxDistance*/, (void*)X_GetFloat /*GetMinDistance*/, (void*)X_Get1 /*GetMode*/, (void*)X_GetVec /*GetPosition*/,
    (void*)X_GetVec /*GetVelocity*/, (void*)X2 /*SetAllParameters*/, (void*)X3 /*SetConeAngles*/, (void*)X4 /*SetConeOrientation*/,
    (void*)X2 /*SetConeOutsideVolume*/, (void*)X2 /*SetMaxDistance*/, (void*)X2 /*SetMinDistance*/, (void*)X2 /*SetMode*/,
    (void*)X4 /*SetPosition*/, (void*)X4 /*SetVelocity*/};
void* g_3dlis[18] = {(void*)X_QI, (void*)X_AddRef, (void*)X_Release,
    (void*)X_GetAll, (void*)X_GetFloat /*GetDistanceFactor*/, (void*)X_GetFloat /*GetDopplerFactor*/, (void*)X2 /*GetOrientation*/,
    (void*)X_GetVec /*GetPosition*/, (void*)X_GetFloat /*GetRolloffFactor*/, (void*)X_GetVec /*GetVelocity*/,
    (void*)X2 /*SetAllParameters*/, (void*)X2 /*SetDistanceFactor*/, (void*)X2 /*SetDopplerFactor*/, (void*)X7 /*SetOrientation*/,
    (void*)X4 /*SetPosition*/, (void*)X2 /*SetRolloffFactor*/, (void*)X4 /*SetVelocity*/, (void*)X0 /*CommitDeferredSettings*/};
// IDirectSoundNotify: the events kept; the mixer sets them while their buffer plays
CRITICAL_SECTION g_notifyLock;
std::vector<std::pair<Buffer*, HANDLE>> g_notify;
HRESULT STDMETHODCALLTYPE N_SetPositions(Side* x, DWORD n, const DWORD* pos) {   // DSBPOSITIONNOTIFY: offset, event
    EnterCriticalSection(&g_notifyLock);
    for (DWORD i = 0; i < n && pos; i++) g_notify.push_back({x->owner, (HANDLE)pos[i * 2 + 1]});
    LeaveCriticalSection(&g_notifyLock);
    return S_OK;
}
void NotifyInit() { InitializeCriticalSection(&g_notifyLock); }
void NotifyTick() {   // every 10 ms: the events of buffers that play (a streaming game refills; no exact positions)
    EnterCriticalSection(&g_notifyLock);
    for (auto& n : g_notify)
        if (n.first->playing && n.second) SetEvent(n.second);
    LeaveCriticalSection(&g_notifyLock);
}
void* g_notifyVt[4] = {(void*)X_QI, (void*)X_AddRef, (void*)X_Release, (void*)N_SetPositions};
HRESULT STDMETHODCALLTYPE B_QI(Buffer* b, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (iid.Data1 == kIUnknown || iid.Data1 == kDSBuffer || iid.Data1 == kDSBuffer8) {
        InterlockedIncrement(&b->refs);
        *out = b;
        return S_OK;
    }
    void** vt = iid.Data1 == 0x279AFA86 ? g_3dbuf : iid.Data1 == 0x279AFA84 ? g_3dlis : iid.Data1 == 0xB0210783 ? g_notifyVt : nullptr;
    if (vt) {
        *out = new Side{vt, 1, b};
        return S_OK;
    }
    SayIid("DirectSound buffer: interface", iid.Data1);
    *out = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE B_GetCaps(Buffer* b, DWORD* caps) {   // DSBCAPS: dwSize dwFlags dwBufferBytes ...
    if (!caps) return E_POINTER;
    DWORD size = caps[0];
    memset(caps + 1, 0, size > 4 ? size - 4 : 0);
    caps[1] = b->flags;
    caps[2] = b->size;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_GetCurrentPosition(Buffer* b, DWORD* play, DWORD* write) {
    DWORD p = Position(b);
    if (play) *play = p;
    if (write) *write = b->size ? (p + b->fmt.nAvgBytesPerSec / 100) % b->size : 0;   // ~10 ms ahead
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_GetFormat(Buffer* b, WAVEFORMATEX* f, DWORD size, DWORD* written) {
    DWORD n = sizeof(WAVEFORMATEX) < size ? (DWORD)sizeof(WAVEFORMATEX) : size;
    if (f) memcpy(f, &b->fmt, n);
    if (written) *written = f ? n : (DWORD)sizeof(WAVEFORMATEX);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_GetVolume(Buffer* b, LONG* v) { return *v = b->volume, S_OK; }
HRESULT STDMETHODCALLTYPE B_GetPan(Buffer* b, LONG* v) { return *v = b->pan, S_OK; }
HRESULT STDMETHODCALLTYPE B_GetFrequency(Buffer* b, DWORD* v) { return *v = b->freq, S_OK; }
HRESULT STDMETHODCALLTYPE B_GetStatus(Buffer* b, DWORD* s) {
    Position(b);
    *s = (b->playing ? 1 : 0) | (b->playing && b->looping ? 4 : 0);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_Initialize(Buffer*, void*, void*) { return 0x88780082; }   // DSERR_ALREADYINITIALIZED
HRESULT STDMETHODCALLTYPE B_Lock(Buffer* b, DWORD offset, DWORD bytes, void** p1, DWORD* n1, void** p2, DWORD* n2,
                                 DWORD flags) {
    if (!b->size) return 0x80070057;
    if (flags & 0x1) offset = Position(b);    // DSBLOCK_FROMWRITECURSOR
    if (flags & 0x2) bytes = b->size;         // DSBLOCK_ENTIREBUFFER
    offset %= b->size;
    if (bytes > b->size) bytes = b->size;
    DWORD first = bytes < b->size - offset ? bytes : b->size - offset;
    *p1 = b->data + offset;
    *n1 = first;
    if (p2) *p2 = bytes > first ? b->data : nullptr;
    if (n2) *n2 = bytes - first;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_Play(Buffer* b, DWORD, DWORD, DWORD flags) {
    EnterCriticalSection(&g_mix);
    if (!b->playing) b->frame = (double)(b->pos / Align(b));
    b->playing = true;
    b->looping = (flags & 1) != 0;   // DSBPLAY_LOOPING
    LeaveCriticalSection(&g_mix);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_SetCurrentPosition(Buffer* b, DWORD p) {
    EnterCriticalSection(&g_mix);
    b->pos = b->size ? p % b->size : 0;
    b->frame = (double)(b->pos / Align(b));
    LeaveCriticalSection(&g_mix);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_SetFormat(Buffer* b, const WAVEFORMATEX* f) {
    if (f) memcpy(&b->fmt, f, sizeof(WAVEFORMATEX)), b->fmt.cbSize = 0;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_SetVolume(Buffer* b, LONG v) { return b->volume = v, S_OK; }
HRESULT STDMETHODCALLTYPE B_SetPan(Buffer* b, LONG v) { return b->pan = v, S_OK; }
HRESULT STDMETHODCALLTYPE B_SetFrequency(Buffer* b, DWORD v) { return b->freq = v, S_OK; }
HRESULT STDMETHODCALLTYPE B_Stop(Buffer* b) {
    EnterCriticalSection(&g_mix);
    if (b->playing) b->pos = b->size ? ((DWORD)b->frame * Align(b)) % b->size : 0;
    b->playing = false;
    LeaveCriticalSection(&g_mix);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE B_Unlock(Buffer*, void*, DWORD, void*, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE B_Restore(Buffer*) { return S_OK; }
HRESULT STDMETHODCALLTYPE B_SetFX(Buffer*, DWORD, void*, DWORD*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE B_AcquireResources(Buffer*, DWORD, DWORD, DWORD*) { return S_OK; }
HRESULT STDMETHODCALLTYPE B_GetObjectInPath(Buffer*, REFGUID, DWORD, REFGUID, void** out) {
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
void* g_buf[24] = {
    (void*)B_QI, (void*)AddRefT<Buffer>, (void*)B_Release, (void*)B_GetCaps, (void*)B_GetCurrentPosition,
    (void*)B_GetFormat, (void*)B_GetVolume, (void*)B_GetPan, (void*)B_GetFrequency, (void*)B_GetStatus,
    (void*)B_Initialize, (void*)B_Lock, (void*)B_Play, (void*)B_SetCurrentPosition, (void*)B_SetFormat,
    (void*)B_SetVolume, (void*)B_SetPan, (void*)B_SetFrequency, (void*)B_Stop, (void*)B_Unlock, (void*)B_Restore,
    (void*)B_SetFX, (void*)B_AcquireResources, (void*)B_GetObjectInPath,
};

ULONG STDMETHODCALLTYPE S_Release(Sound* s) {
    LONG n = InterlockedDecrement(&s->refs);
    if (n <= 0) delete s;
    return n < 0 ? 0 : (ULONG)n;
}
HRESULT STDMETHODCALLTYPE S_QI(Sound* s, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (iid.Data1 == kIUnknown || iid.Data1 == kDS || iid.Data1 == kDS8) {
        InterlockedIncrement(&s->refs);
        *out = s;
        return S_OK;
    }
    SayIid("DirectSound: interface", iid.Data1);
    *out = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE S_CreateSoundBuffer(Sound*, const DWORD* desc, void** out, void*) {
    // DSBUFFERDESC: dwSize dwFlags dwBufferBytes dwReserved lpwfxFormat (guid3DAlgorithm)
    if (!desc || !out) return E_POINTER;
    Buffer* b = new Buffer{};
    b->vtbl = g_buf;
    b->refs = 1;
    b->flags = desc[1];
    b->size = (desc[1] & 1) /*DSBCAPS_PRIMARYBUFFER*/ ? 65536 : desc[2];
    const WAVEFORMATEX* f = (const WAVEFORMATEX*)desc[4];
    if (f) memcpy(&b->fmt, f, sizeof(WAVEFORMATEX)), b->fmt.cbSize = 0;
    else b->fmt = {WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0};
    b->freq = b->fmt.nSamplesPerSec;
    b->data = new BYTE[b->size ? b->size : 1]();
    if (b->fmt.wBitsPerSample == 8) memset(b->data, 0x80, b->size);   // 8-bit silence
    if (!(b->flags & 1)) {   // secondary buffers are mixed (the primary one is not)
        StartMixer();
        EnterCriticalSection(&g_mix);
        g_playing.push_back(b);
        LeaveCriticalSection(&g_mix);
    }
    *out = b;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE S_GetCaps(Sound*, DWORD* caps) {
    if (!caps) return E_POINTER;
    DWORD size = caps[0];
    memset(caps + 1, 0, size > 4 ? size - 4 : 0);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE S_Duplicate(Sound* s, Buffer* orig, void** out) {
    DWORD desc[6] = {sizeof desc, orig->flags, orig->size, 0, (DWORD)&orig->fmt, 0};
    HRESULT hr = S_CreateSoundBuffer(s, desc, out, nullptr);
    if (SUCCEEDED(hr)) memcpy(((Buffer*)*out)->data, orig->data, orig->size);
    return hr;
}
HRESULT STDMETHODCALLTYPE S_Ok2(Sound*, DWORD, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE S_Ok0(Sound*) { return S_OK; }
HRESULT STDMETHODCALLTYPE S_GetSpeaker(Sound*, DWORD* c) { return *c = 4 /*stereo*/, S_OK; }
HRESULT STDMETHODCALLTYPE S_Ok1(Sound*, DWORD) { return S_OK; }
HRESULT STDMETHODCALLTYPE S_Verify(Sound*, DWORD* c) { return *c = 0, S_OK; }
void* g_ds[12] = {
    (void*)S_QI, (void*)AddRefT<Sound>, (void*)S_Release, (void*)S_CreateSoundBuffer, (void*)S_GetCaps,
    (void*)S_Duplicate, (void*)S_Ok2 /*SetCooperativeLevel*/, (void*)S_Ok0 /*Compact*/, (void*)S_GetSpeaker,
    (void*)S_Ok1 /*SetSpeakerConfig*/, (void*)S_Ok1 /*Initialize*/, (void*)S_Verify,
};
}  // namespace

extern "C" HRESULT WINAPI DirectSoundCreate8(const GUID*, void** out, void*) {
    if (!out) return E_POINTER;
    *out = new Sound{g_ds, 1};
    StartMixer();
    return S_OK;
}
extern "C" HRESULT WINAPI DirectSoundCreate(const GUID* g, void** out, void* outer) { return DirectSoundCreate8(g, out, outer); }
typedef BOOL(CALLBACK* DSEnumA)(GUID*, LPCSTR, LPCSTR, void*);
typedef BOOL(CALLBACK* DSEnumW)(GUID*, LPCWSTR, LPCWSTR, void*);
extern "C" HRESULT WINAPI DirectSoundEnumerateA(DSEnumA cb, void* ctx) {
    if (cb) cb(nullptr, "Primary Sound Driver", "", ctx);
    return S_OK;
}
extern "C" HRESULT WINAPI DirectSoundEnumerateW(DSEnumW cb, void* ctx) {
    if (cb) cb(nullptr, L"Primary Sound Driver", L"", ctx);
    return S_OK;
}
extern "C" HRESULT WINAPI DirectSoundCaptureCreate(const GUID*, void** out, void*) {
    if (out) *out = nullptr;
    return 0x88780078;   // DSERR_NODRIVER
}
extern "C" HRESULT WINAPI DirectSoundCaptureCreate8(const GUID* g, void** out, void* o) { return DirectSoundCaptureCreate(g, out, o); }
extern "C" HRESULT WINAPI DirectSoundCaptureEnumerateA(void*, void*) { return S_OK; }
extern "C" HRESULT WINAPI DirectSoundCaptureEnumerateW(void*, void*) { return S_OK; }
extern "C" HRESULT WINAPI DirectSoundFullDuplexCreate(void*, void*, void*, void*, HWND, DWORD, void*, void*, void*, void*) {
    return 0x88780078;
}
extern "C" HRESULT WINAPI GetDeviceID(const GUID* in, GUID* out) {
    if (out) memset(out, 0, sizeof *out);
    return S_OK;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, void** out) {
    if (out) *out = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, void*) { return TRUE; }
