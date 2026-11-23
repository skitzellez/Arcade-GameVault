// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// XAudio2 2.7 (DirectX SDK June 2010, XAudio2_7.dll) for games made for it (Star Wars Battle Pod's Wwise), on the
// system's XAudio2 2.9. 2.7 is a COM class (CoCreateInstance + Initialize); its interfaces differ from 2.9 in a few
// places, so the engine and every voice the game gets are thin objects in 2.7's layout that call 2.9:
//   - the engine: GetDeviceCount / GetDeviceDetails / Initialize (2.7 only); CreateMasteringVoice by device index
//   - voices: GetVoiceDetails (2.9 adds ActiveFlags), source GetState (2.9 adds a flags argument), and every voice a
//     game names as a destination (sends, output matrix, output filter) is one of ours, unwrapped here
// Callbacks (voice and engine) have the same layout in both, so the game's own go straight to 2.9.

#include "peload.h"
#include <windows.h>
#include <xaudio2.h>
#include <xapo.h>
#include <new>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace arcade {

static const GUID kClsidXAudio2_27 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
static const GUID kClsidXAudio2Debug_27 = {0xdb05ea35, 0x0329, 0x4d4b, {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52}};
static const GUID kIidXAudio2_27 = {0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};

struct Details27 {
    UINT32 CreationFlags, InputChannels, InputSampleRate;
};
struct DeviceDetails27 {
    WCHAR DeviceID[256];
    WCHAR DisplayName[256];
    UINT32 Role;   // XAUDIO2_DEVICE_ROLE: 0xF = global default device
    WAVEFORMATEXTENSIBLE OutputFormat;
};

// (testing) what reaches the voices
static std::atomic<long> g_buffers{0}, g_starts{0}, g_engineStarts{0}, g_loud{0};
static std::atomic<long> g_peak{0};   // biggest sample seen x 1000
static void AudioReporter() {
    static bool on = false;
    if (on) return;
    on = true;
    std::thread([] {
        for (;;) {
            Sleep(5000);
            Log("XAudio2 2.7 (5 s): %ld buffers, %ld voice starts, %ld engine starts, %ld loud buffers, peak %.3f",
                g_buffers.exchange(0), g_starts.exchange(0), g_engineStarts.load(), g_loud.exchange(0), g_peak.exchange(0) / 1000.0);
        }
    }).detach();
}

// ---- effects: a game's XAPOs answer 2.7's interface IDs; the methods are the same, so 2.9 gets a go-between ----
static const GUID kIidXapo27 = {0xA90BC001, 0xE897, 0xE897, {0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00}};
static const GUID kIidXapoParams27 = {0xA90BC001, 0xE897, 0xE897, {0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x01}};
class Xapo29 : public IXAPO, public IXAPOParameters {
public:
    LONG refs = 1;
    IXAPO* x = nullptr;             // the game's (2.7 IID, same table)
    IXAPOParameters* prm = nullptr;
    static IUnknown* Wrap(IUnknown* game) {
        if (!game) return nullptr;
        IXAPO* x = nullptr;
        if (SUCCEEDED(game->QueryInterface(__uuidof(IXAPO), (void**)&x))) {   // made for 2.8+: as it is
            x->Release();
            game->AddRef();
            return game;
        }
        if (FAILED(game->QueryInterface(kIidXapo27, (void**)&x))) {
            Log("XAudio2 2.7: an effect that is no XAPO");
            game->AddRef();
            return game;
        }
        auto* w = new Xapo29;
        w->x = x;
        game->QueryInterface(kIidXapoParams27, (void**)&w->prm);
        return static_cast<IXAPO*>(w);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IXAPO)) *out = static_cast<IXAPO*>(this);
        else if (iid == __uuidof(IXAPOParameters) && prm) *out = static_cast<IXAPOParameters*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (!n) {
            if (prm) prm->Release();
            x->Release();
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(XAPO_REGISTRATION_PROPERTIES** p) override { return x->GetRegistrationProperties(p); }
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(const WAVEFORMATEX* o, const WAVEFORMATEX* r, WAVEFORMATEX** s) override {
        return x->IsInputFormatSupported(o, r, s);
    }
    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(const WAVEFORMATEX* i, const WAVEFORMATEX* r, WAVEFORMATEX** s) override {
        return x->IsOutputFormatSupported(i, r, s);
    }
    HRESULT STDMETHODCALLTYPE Initialize(const void* d, UINT32 n) override { return x->Initialize(d, n); }
    void STDMETHODCALLTYPE Reset() override { x->Reset(); }
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 ni, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* i, UINT32 no,
                                             const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* o) override {
        return x->LockForProcess(ni, i, no, o);
    }
    void STDMETHODCALLTYPE UnlockForProcess() override { x->UnlockForProcess(); }
    void STDMETHODCALLTYPE Process(UINT32 ni, const XAPO_PROCESS_BUFFER_PARAMETERS* i, UINT32 no,
                                   XAPO_PROCESS_BUFFER_PARAMETERS* o, BOOL on) override {
        x->Process(ni, i, no, o, on);
    }
    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 n) override { return x->CalcInputFrames(n); }
    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 n) override { return x->CalcOutputFrames(n); }
    void STDMETHODCALLTYPE SetParameters(const void* p, UINT32 n) override { prm->SetParameters(p, n); }
    void STDMETHODCALLTYPE GetParameters(void* p, UINT32 n) override { prm->GetParameters(p, n); }
};
// the game's effect chain, with go-betweens (released after the voice took them)
struct Chain29 {
    std::vector<XAUDIO2_EFFECT_DESCRIPTOR> d;
    XAUDIO2_EFFECT_CHAIN c = {};
    const XAUDIO2_EFFECT_CHAIN* From(const XAUDIO2_EFFECT_CHAIN* g) {
        if (!g) return nullptr;
        d.resize(g->EffectCount);
        for (UINT32 i = 0; i < g->EffectCount; i++) {
            d[i] = g->pEffectDescriptors[i];
            d[i].pEffect = Xapo29::Wrap(g->pEffectDescriptors[i].pEffect);
        }
        c.EffectCount = g->EffectCount;
        c.pEffectDescriptors = d.data();
        return &c;
    }
    ~Chain29() {
        for (auto& e : d)
            if (e.pEffect) e.pEffect->Release();
    }
};

// ---- voices (2.7 layout: no IUnknown) ----
class Voice27 {
public:
    IXAudio2Voice* real = nullptr;
    virtual void STDMETHODCALLTYPE GetVoiceDetails(Details27* d) {
        XAUDIO2_VOICE_DETAILS x = {};
        real->GetVoiceDetails(&x);
        d->CreationFlags = x.CreationFlags;
        d->InputChannels = x.InputChannels;
        d->InputSampleRate = x.InputSampleRate;
    }
    virtual HRESULT STDMETHODCALLTYPE SetOutputVoices(const XAUDIO2_VOICE_SENDS* s);
    virtual HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN* c) {
        Chain29 x;
        return real->SetEffectChain(x.From(c));
    }
    virtual HRESULT STDMETHODCALLTYPE EnableEffect(UINT32 i, UINT32 op) { return real->EnableEffect(i, op); }
    virtual HRESULT STDMETHODCALLTYPE DisableEffect(UINT32 i, UINT32 op) { return real->DisableEffect(i, op); }
    virtual void STDMETHODCALLTYPE GetEffectState(UINT32 i, BOOL* on) { real->GetEffectState(i, on); }
    virtual HRESULT STDMETHODCALLTYPE SetEffectParameters(UINT32 i, const void* p, UINT32 n, UINT32 op) {
        return real->SetEffectParameters(i, p, n, op);
    }
    virtual HRESULT STDMETHODCALLTYPE GetEffectParameters(UINT32 i, void* p, UINT32 n) { return real->GetEffectParameters(i, p, n); }
    virtual HRESULT STDMETHODCALLTYPE SetFilterParameters(const XAUDIO2_FILTER_PARAMETERS* f, UINT32 op) {
        return real->SetFilterParameters(f, op);
    }
    virtual void STDMETHODCALLTYPE GetFilterParameters(XAUDIO2_FILTER_PARAMETERS* f) { real->GetFilterParameters(f); }
    virtual HRESULT STDMETHODCALLTYPE SetOutputFilterParameters(Voice27* to, const XAUDIO2_FILTER_PARAMETERS* f, UINT32 op) {
        return real->SetOutputFilterParameters(to ? to->real : nullptr, f, op);
    }
    virtual void STDMETHODCALLTYPE GetOutputFilterParameters(Voice27* to, XAUDIO2_FILTER_PARAMETERS* f) {
        real->GetOutputFilterParameters(to ? to->real : nullptr, f);
    }
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float v, UINT32 op) { return real->SetVolume(v, op); }
    virtual void STDMETHODCALLTYPE GetVolume(float* v) { real->GetVolume(v); }
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumes(UINT32 n, const float* v, UINT32 op) { return real->SetChannelVolumes(n, v, op); }
    virtual void STDMETHODCALLTYPE GetChannelVolumes(UINT32 n, float* v) { real->GetChannelVolumes(n, v); }
    virtual HRESULT STDMETHODCALLTYPE SetOutputMatrix(Voice27* to, UINT32 src, UINT32 dst, const float* m, UINT32 op) {
        return real->SetOutputMatrix(to ? to->real : nullptr, src, dst, m, op);
    }
    virtual void STDMETHODCALLTYPE GetOutputMatrix(Voice27* to, UINT32 src, UINT32 dst, float* m) {
        real->GetOutputMatrix(to ? to->real : nullptr, src, dst, m);
    }
    virtual void STDMETHODCALLTYPE DestroyVoice() {   // (no virtual destructor: it would take a slot in 2.7's table)
        real->DestroyVoice();
        delete this;
    }
};

// the game's sends name our voices: 2.9 gets its own
struct Sends29 {
    std::vector<XAUDIO2_SEND_DESCRIPTOR> d;
    XAUDIO2_VOICE_SENDS s = {};
    const XAUDIO2_VOICE_SENDS* From(const XAUDIO2_VOICE_SENDS* g) {
        if (!g) return nullptr;
        d.resize(g->SendCount);
        for (UINT32 i = 0; i < g->SendCount; i++) {
            d[i].Flags = g->pSends[i].Flags;
            d[i].pOutputVoice = g->pSends[i].pOutputVoice ? ((Voice27*)g->pSends[i].pOutputVoice)->real : nullptr;
        }
        s.SendCount = g->SendCount;
        s.pSends = d.data();
        return &s;
    }
};
HRESULT STDMETHODCALLTYPE Voice27::SetOutputVoices(const XAUDIO2_VOICE_SENDS* s) {
    Sends29 x;
    return real->SetOutputVoices(x.From(s));
}

class SourceVoice27 : public Voice27 {
public:
    IXAudio2SourceVoice* Src() { return (IXAudio2SourceVoice*)real; }
    WORD tag = 0, bits = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(UINT32 f, UINT32 op) {
        g_starts++;
        return Src()->Start(f, op);
    }
    virtual HRESULT STDMETHODCALLTYPE Stop(UINT32 f, UINT32 op) { return Src()->Stop(f, op); }
    virtual HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const XAUDIO2_BUFFER* b, const XAUDIO2_BUFFER_WMA* w) {
        g_buffers++;
        if (b && b->pAudioData && b->AudioBytes) {   // loudness: float or 16-bit samples
            float peak = 0;
            if (bits == 32)
                for (UINT32 i = 0; i < b->AudioBytes / 4; i += 7) peak = fmaxf(peak, fabsf(((const float*)b->pAudioData)[i]));
            else if (bits == 16)
                for (UINT32 i = 0; i < b->AudioBytes / 2; i += 7) peak = fmaxf(peak, fabsf(((const short*)b->pAudioData)[i] / 32768.f));
            long k = (long)(peak * 1000);
            if (k > 1) g_loud++;
            long old = g_peak.load();
            while (k > old && !g_peak.compare_exchange_weak(old, k)) {}
        }
        HRESULT hr = Src()->SubmitSourceBuffer(b, w);
        static int told = 0;
        if (FAILED(hr) && told++ < 5) Log("XAudio2 2.7: SubmitSourceBuffer: 0x%08lx", (unsigned long)hr);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE FlushSourceBuffers() { return Src()->FlushSourceBuffers(); }
    virtual HRESULT STDMETHODCALLTYPE Discontinuity() { return Src()->Discontinuity(); }
    virtual HRESULT STDMETHODCALLTYPE ExitLoop(UINT32 op) { return Src()->ExitLoop(op); }
    virtual void STDMETHODCALLTYPE GetState(XAUDIO2_VOICE_STATE* s) { Src()->GetState(s, 0); }
    virtual HRESULT STDMETHODCALLTYPE SetFrequencyRatio(float r, UINT32 op) { return Src()->SetFrequencyRatio(r, op); }
    virtual void STDMETHODCALLTYPE GetFrequencyRatio(float* r) { Src()->GetFrequencyRatio(r); }
    virtual HRESULT STDMETHODCALLTYPE SetSourceSampleRate(UINT32 r) { return Src()->SetSourceSampleRate(r); }
};
class SubmixVoice27 : public Voice27 {};
class MasteringVoice27 : public Voice27 {};

// ---- the engine (2.7 layout, a COM object) ----
class XAudio2_27 {
public:
    LONG refs = 1;
    IXAudio2* x = nullptr;
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) {
        if (!out) return E_POINTER;
        if (iid == kIidXAudio2_27 || iid == __uuidof(IUnknown)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&refs); }
    virtual ULONG STDMETHODCALLTYPE Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) {
            if (x) x->Release();
            delete this;
        }
        return n;
    }
    virtual HRESULT STDMETHODCALLTYPE GetDeviceCount(UINT32* n) {
        *n = 1;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetDeviceDetails(UINT32 i, DeviceDetails27* d) {
        if (i) return E_INVALIDARG;
        memset(d, 0, sizeof *d);
        wcscpy_s(d->DeviceID, L"default");
        wcscpy_s(d->DisplayName, L"Xbox audio");
        d->Role = 0xF;   // GlobalDefaultDevice
        // the console's own layout (stereo, 5.1, 7.1 by what the TV/receiver takes): a mastering voice says
        UINT32 ch = 2, rate = 48000;
        DWORD mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        if (x) {
            IXAudio2MasteringVoice* m = nullptr;
            if (SUCCEEDED(x->CreateMasteringVoice(&m))) {
                XAUDIO2_VOICE_DETAILS vd = {};
                m->GetVoiceDetails(&vd);
                m->GetChannelMask(&mask);
                ch = vd.InputChannels;
                rate = vd.InputSampleRate;
                m->DestroyVoice();
            }
        }
        WAVEFORMATEXTENSIBLE& f = d->OutputFormat;
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = (WORD)ch;
        f.Format.nSamplesPerSec = rate;
        f.Format.wBitsPerSample = 32;
        f.Format.nBlockAlign = (WORD)(ch * 4);
        f.Format.nAvgBytesPerSec = rate * ch * 4;
        f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f.Samples.wValidBitsPerSample = 32;
        f.dwChannelMask = mask;
        static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        f.SubFormat = kFloat;
        Log("XAudio2 2.7: device details asked: %u channels (mask 0x%lx), %u Hz", ch, (unsigned long)mask, rate);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Initialize(UINT32 flags, UINT32 processor) {
        if (x) return S_OK;
        HRESULT hr = XAudio2Create(&x, 0, XAUDIO2_DEFAULT_PROCESSOR);
        Log("XAudio2 2.7: Initialize (flags 0x%x) -> the system's XAudio2: 0x%08lx", flags, (unsigned long)hr);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE RegisterForCallbacks(IXAudio2EngineCallback* c) { return x->RegisterForCallbacks(c); }
    virtual void STDMETHODCALLTYPE UnregisterForCallbacks(IXAudio2EngineCallback* c) { x->UnregisterForCallbacks(c); }
    virtual HRESULT STDMETHODCALLTYPE CreateSourceVoice(SourceVoice27** out, const WAVEFORMATEX* fmt, UINT32 flags, float maxRatio,
                                                        IXAudio2VoiceCallback* cb, const XAUDIO2_VOICE_SENDS* sends,
                                                        const XAUDIO2_EFFECT_CHAIN* fx) {
        Sends29 s;
        Chain29 c;
        IXAudio2SourceVoice* v = nullptr;
        HRESULT hr = x->CreateSourceVoice(&v, fmt, flags, maxRatio, cb, s.From(sends), c.From(fx));
        static int told = 0;
        if (told++ < 6 || FAILED(hr))
            Log("XAudio2 2.7: source voice (format %u, %u ch, %lu Hz): 0x%08lx", fmt ? fmt->wFormatTag : 0,
                fmt ? fmt->nChannels : 0, fmt ? (unsigned long)fmt->nSamplesPerSec : 0ul, (unsigned long)hr);
        if (FAILED(hr)) return hr;
        auto* w = new SourceVoice27;
        w->real = v;
        w->tag = fmt ? fmt->wFormatTag : 0;
        w->bits = fmt ? fmt->wBitsPerSample : 0;
        if (fmt) {
            const WAVEFORMATEXTENSIBLE* e = (const WAVEFORMATEXTENSIBLE*)fmt;
            Log("XAudio2 2.7: source voice format: tag 0x%x, %u ch, %lu Hz, %u bits, block %u, cb %u, sub %08lx, mask 0x%lx, flags 0x%x, cb %p",
                fmt->wFormatTag, fmt->nChannels, (unsigned long)fmt->nSamplesPerSec, fmt->wBitsPerSample, fmt->nBlockAlign,
                fmt->cbSize, fmt->wFormatTag == 0xFFFE ? (unsigned long)e->SubFormat.Data1 : 0ul,
                fmt->wFormatTag == 0xFFFE ? (unsigned long)e->dwChannelMask : 0ul, flags, cb);
        }
        AudioReporter();
        *out = w;
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE CreateSubmixVoice(SubmixVoice27** out, UINT32 ch, UINT32 rate, UINT32 flags, UINT32 stage,
                                                        const XAUDIO2_VOICE_SENDS* sends, const XAUDIO2_EFFECT_CHAIN* fx) {
        Sends29 s;
        Chain29 c;
        IXAudio2SubmixVoice* v = nullptr;
        HRESULT hr = x->CreateSubmixVoice(&v, ch, rate, flags, stage, s.From(sends), c.From(fx));
        if (FAILED(hr)) {
            Log("XAudio2 2.7: submix voice (%u ch, %u Hz, flags 0x%x, stage %u, %u sends, effects %u): 0x%08lx", ch, rate, flags,
                stage, sends ? sends->SendCount : 0, fx ? fx->EffectCount : 0, (unsigned long)hr);
            for (UINT32 i = 0; sends && i < sends->SendCount; i++)
                Log("  send %u: flags 0x%x, voice %p (system's %p)", i, sends->pSends[i].Flags, sends->pSends[i].pOutputVoice,
                    s.d[i].pOutputVoice);
            for (UINT32 i = 0; fx && i < fx->EffectCount; i++)
                Log("  effect %u: %p, on %d, %u channels out", i, fx->pEffectDescriptors[i].pEffect,
                    fx->pEffectDescriptors[i].InitialState, fx->pEffectDescriptors[i].OutputChannels);
            return hr;
        }
        auto* w = new SubmixVoice27;
        w->real = v;
        *out = w;
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE CreateMasteringVoice(MasteringVoice27** out, UINT32 ch, UINT32 rate, UINT32 flags,
                                                           UINT32 device, const XAUDIO2_EFFECT_CHAIN* fx) {
        IXAudio2MasteringVoice* v = nullptr;
        Chain29 c;
        HRESULT hr = x->CreateMasteringVoice(&v, ch, rate, 0, nullptr, c.From(fx), AudioCategory_GameEffects);
        Log("XAudio2 2.7: mastering voice (%u ch, %u Hz asked, device %u): 0x%08lx", ch, rate, device, (unsigned long)hr);
        if (FAILED(hr)) return hr;
        auto* w = new MasteringVoice27;
        w->real = v;
        *out = w;
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE StartEngine() {
        g_engineStarts++;
        HRESULT hr = x->StartEngine();
        Log("XAudio2 2.7: StartEngine: 0x%08lx", (unsigned long)hr);
        return hr;
    }
    virtual void STDMETHODCALLTYPE StopEngine() { x->StopEngine(); }
    virtual HRESULT STDMETHODCALLTYPE CommitChanges(UINT32 op) { return x->CommitChanges(op); }
    virtual void STDMETHODCALLTYPE GetPerformanceData(XAUDIO2_PERFORMANCE_DATA* p) { x->GetPerformanceData(p); }
    virtual void STDMETHODCALLTYPE SetDebugConfiguration(const XAUDIO2_DEBUG_CONFIGURATION* c, void*) {
        x->SetDebugConfiguration(c, nullptr);
    }
    virtual ~XAudio2_27() {}
};

// CoCreateInstance: XAudio2 2.7's class is ours; any other goes to the system
typedef HRESULT(WINAPI* cci_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static HRESULT WINAPI S_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out) {
    if (clsid == kClsidXAudio2_27 || clsid == kClsidXAudio2Debug_27) {
        auto* a = new (std::nothrow) XAudio2_27;
        if (!a) return E_OUTOFMEMORY;
        HRESULT hr = a->QueryInterface(iid, out);
        a->Release();
        Log("XAudio2 2.7 asked for: ours (on the system's XAudio2 2.9): 0x%08lx", (unsigned long)hr);
        return hr;
    }
    HRESULT hr = CoCreateInstance(clsid, outer, ctx, iid, out);
    if (FAILED(hr))
        Log("CoCreateInstance {%08lx-...}: 0x%08lx", (unsigned long)clsid.Data1, (unsigned long)hr);
    return hr;
}

void* FindXAudio27Shim(const char* dll, const char* func) {
    if (func && !strcmp(func, "CoCreateInstance") && (!_stricmp(dll, "ole32.dll") || !_stricmp(dll, "combase.dll") ||
                                                     !_strnicmp(dll, "api-ms-win-core-com-", 20)))
        return (void*)S_CoCreateInstance;
    return nullptr;
}

}  // namespace arcade
