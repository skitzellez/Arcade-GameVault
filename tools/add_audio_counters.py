# one-off edit (testing): what the game feeds XAudio2 2.7 - buffers, starts, loudness - logged every 5 s
from pathlib import Path
p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\xaudio27.cpp")
s = p.read_text(encoding="utf-8")
def rep(a, b):
    global s
    assert s.count(a) == 1, a[:60]
    s = s.replace(a, b)
rep('#include <new>\n', '#include <new>\n#include <atomic>\n#include <cmath>\n#include <thread>\n')
rep('''// ---- effects:''', '''// (testing) what reaches the voices
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

// ---- effects:''')
rep('''    virtual HRESULT STDMETHODCALLTYPE Start(UINT32 f, UINT32 op) { return Src()->Start(f, op); }''',
    '''    WORD tag = 0, bits = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(UINT32 f, UINT32 op) {
        g_starts++;
        return Src()->Start(f, op);
    }''')
rep('''    virtual HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const XAUDIO2_BUFFER* b, const XAUDIO2_BUFFER_WMA* w) {
        return Src()->SubmitSourceBuffer(b, w);''', '''    virtual HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const XAUDIO2_BUFFER* b, const XAUDIO2_BUFFER_WMA* w) {
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
        return hr;''')
rep('''        auto* w = new SourceVoice27;
        w->real = v;''', '''        auto* w = new SourceVoice27;
        w->real = v;
        w->tag = fmt ? fmt->wFormatTag : 0;
        w->bits = fmt ? fmt->wBitsPerSample : 0;
        AudioReporter();''')
rep('''    virtual HRESULT STDMETHODCALLTYPE StartEngine() { return x->StartEngine(); }''', '''    virtual HRESULT STDMETHODCALLTYPE StartEngine() {
        g_engineStarts++;
        HRESULT hr = x->StartEngine();
        Log("XAudio2 2.7: StartEngine: 0x%08lx", (unsigned long)hr);
        return hr;
    }''')
p.write_text(s, encoding="utf-8")
print("ok")
