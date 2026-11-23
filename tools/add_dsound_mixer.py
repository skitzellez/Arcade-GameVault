# one-off edit: x86dll/dsound.cpp becomes a real DirectSound: a mixer thread mixes the playing buffers into 48 kHz
# stereo and hands it to gvaudio.dll (bridged to an XAudio2 voice, src/gvaudio.cpp)
from pathlib import Path
p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\x86dll\dsound.cpp")
s = p.read_text(encoding="utf-8")


def rep(a, b):
    global s
    assert s.count(a) == 1, a[:60]
    s = s.replace(a, b)


rep("""// dsound.dll for 32-bit programs (built 32-bit, loaded by the 32-bit bridge from the package's x86 folder): a SILENT
// DirectSound 8. Buffers take the game's sound data and their play position moves with time, so a game streaming
// music keeps running; nothing is heard yet (real sound: later, through XAudio2).
#include <windows.h>
#include <cstring>
""", """// dsound.dll for 32-bit programs (built 32-bit, loaded by the 32-bit bridge from the package's x86 folder): DirectSound 8
// with a mixer. A thread mixes every playing buffer (8/16-bit PCM, mono/stereo; volume, pan, frequency, looping) into
// 16-bit stereo 48 kHz, 10 ms at a time, and hands it to gvaudio.dll (the loader's: an XAudio2 voice). Play positions
// are where the mixer is, so a game streaming music writes in step.
#include <windows.h>
#include <cmath>
#include <cstring>
#include <vector>
""")
rep("""    LONG volume, pan;
    DWORD freq;
};""", """    LONG volume, pan;
    DWORD freq;
    double frame;   // the mixer's place (frames)
};""")
a = s.index("DWORD Position(Buffer* b) {")
b = s.index("template <typename T> ULONG STDMETHODCALLTYPE AddRefT")
s = s[:a] + """// --- the mixer --------------------------------------------------------------------------------------------------
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
    if (HMODULE m = LoadLibraryA("gvaudio.dll")) {
        g_write = (write_t)GetProcAddress(m, "GvAudioWrite");
        g_queued = (queued_t)GetProcAddress(m, "GvAudioQueuedMs");
    }
    OutputDebugStringA(g_write ? "DirectSound 8: mixer -> XAudio2" : "DirectSound 8: no sound out (gvaudio.dll not there)");
    CreateThread(nullptr, 0, MixThread, nullptr, 0, nullptr);
}
""" + s[b:]
rep("""    LONG n = InterlockedDecrement(&b->refs);
    if (n <= 0) {
        delete[] b->data;
        delete b;
    }""", """    LONG n = InterlockedDecrement(&b->refs);
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
    }""")
a = s.index("HRESULT STDMETHODCALLTYPE B_Play(Buffer* b, DWORD, DWORD, DWORD flags) {")
b = s.index("HRESULT STDMETHODCALLTYPE B_SetFormat(")
s = s[:a] + """HRESULT STDMETHODCALLTYPE B_Play(Buffer* b, DWORD, DWORD, DWORD flags) {
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
""" + s[b:]
a = s.index("HRESULT STDMETHODCALLTYPE B_Stop(Buffer* b) {")
b = s.index("HRESULT STDMETHODCALLTYPE B_Unlock(")
s = s[:a] + """HRESULT STDMETHODCALLTYPE B_Stop(Buffer* b) {
    EnterCriticalSection(&g_mix);
    if (b->playing) b->pos = b->size ? ((DWORD)b->frame * Align(b)) % b->size : 0;
    b->playing = false;
    LeaveCriticalSection(&g_mix);
    return S_OK;
}
""" + s[b:]
rep("""    b->freq = b->fmt.nSamplesPerSec;
    b->data = new BYTE[b->size ? b->size : 1]();
    *out = b;
    return S_OK;""", """    b->freq = b->fmt.nSamplesPerSec;
    b->data = new BYTE[b->size ? b->size : 1]();
    if (b->fmt.wBitsPerSample == 8) memset(b->data, 0x80, b->size);   // 8-bit silence
    if (!(b->flags & 1)) {   // secondary buffers are mixed (the primary one is not)
        StartMixer();
        EnterCriticalSection(&g_mix);
        g_playing.push_back(b);
        LeaveCriticalSection(&g_mix);
    }
    *out = b;
    return S_OK;""")
rep("""    *out = new Sound{g_ds, 1};
    OutputDebugStringA("DirectSound 8: stand-in (silent)");""", """    *out = new Sound{g_ds, 1};
    StartMixer();""")
p.write_text(s, encoding="utf-8")
print("ok")
