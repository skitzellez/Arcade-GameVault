# one-off edit: XAPO effects made for XAudio2 2.7 (other interface IDs) wrapped for 2.9 (xaudio27.cpp)
from pathlib import Path
p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\xaudio27.cpp")
s = p.read_text(encoding="utf-8")
def rep(a, b):
    global s
    assert s.count(a) == 1, a[:60]
    s = s.replace(a, b)
rep('#include <xaudio2.h>\n', '#include <xaudio2.h>\n#include <xapo.h>\n')
rep('''// ---- voices (2.7 layout: no IUnknown) ----''', r'''// ---- effects: a game's XAPOs answer 2.7's interface IDs; the methods are the same, so 2.9 gets a go-between ----
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

// ---- voices (2.7 layout: no IUnknown) ----''')
rep('''    virtual HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN* c) { return real->SetEffectChain(c); }''',
    '''    virtual HRESULT STDMETHODCALLTYPE SetEffectChain(const XAUDIO2_EFFECT_CHAIN* c) {
        Chain29 x;
        return real->SetEffectChain(x.From(c));
    }''')
rep('''        Sends29 s;
        IXAudio2SourceVoice* v = nullptr;
        HRESULT hr = x->CreateSourceVoice(&v, fmt, flags, maxRatio, cb, s.From(sends), fx);''', '''        Sends29 s;
        Chain29 c;
        IXAudio2SourceVoice* v = nullptr;
        HRESULT hr = x->CreateSourceVoice(&v, fmt, flags, maxRatio, cb, s.From(sends), c.From(fx));''')
rep('''        Sends29 s;
        IXAudio2SubmixVoice* v = nullptr;
        HRESULT hr = x->CreateSubmixVoice(&v, ch, rate, flags, stage, s.From(sends), fx);''', '''        Sends29 s;
        Chain29 c;
        IXAudio2SubmixVoice* v = nullptr;
        HRESULT hr = x->CreateSubmixVoice(&v, ch, rate, flags, stage, s.From(sends), c.From(fx));''')
rep('''        HRESULT hr = x->CreateMasteringVoice(&v, ch, rate, 0, nullptr, fx, AudioCategory_GameEffects);''', '''        Chain29 c;
        HRESULT hr = x->CreateMasteringVoice(&v, ch, rate, 0, nullptr, c.From(fx), AudioCategory_GameEffects);''')
p.write_text(s, encoding="utf-8")
print("ok")
