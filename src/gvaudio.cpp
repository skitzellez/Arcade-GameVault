// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// Sound out for 32-bit programs: our 32-bit DirectSound (x86dll/dsound.cpp) mixes the game's sound buffers and hands
// the mix here ("gvaudio.dll", bridged): 16-bit stereo 48 kHz, played by an XAudio2 2.9 voice.
//   GvAudioWrite(samples, bytes) - queues a piece; GvAudioQueuedMs() - how much is still to play.

#include "peload.h"
#include <windows.h>
#include <xaudio2.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace arcade {

static IXAudio2* g_xa = nullptr;
static IXAudio2MasteringVoice* g_master = nullptr;
static IXAudio2SourceVoice* g_voice = nullptr;
static std::mutex g_lock;
static const int kRing = 32;
static std::vector<uint8_t> g_ring[kRing];
static int g_next = 0;

static bool Open() {
    if (g_voice) return true;
    static bool failed = false;
    if (failed) return false;
    HRESULT hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) hr = g_xa->CreateMasteringVoice(&g_master);
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    if (SUCCEEDED(hr)) hr = g_xa->CreateSourceVoice(&g_voice, &f);
    if (SUCCEEDED(hr)) hr = g_voice->Start();
    Log("32-bit sound: XAudio2 voice for the game's DirectSound mix (48 kHz stereo): 0x%08lx", (unsigned long)hr);
    if (FAILED(hr)) failed = true;
    return SUCCEEDED(hr);
}
static uint64_t G_Write(uint64_t samples, uint64_t bytes) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!Open() || !(uint32_t)bytes) return 0;
    XAUDIO2_VOICE_STATE st;
    g_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued >= kRing - 1) return 0;   // full: this piece is dropped
    std::vector<uint8_t>& b = g_ring[g_next];
    g_next = (g_next + 1) % kRing;
    b.assign((const uint8_t*)(uintptr_t)(uint32_t)samples, (const uint8_t*)(uintptr_t)(uint32_t)samples + (uint32_t)bytes);
    XAUDIO2_BUFFER x = {};
    x.AudioBytes = (UINT32)b.size();
    x.pAudioData = b.data();
    g_voice->SubmitSourceBuffer(&x);
    return 1;
}
static uint64_t G_QueuedMs() {
    std::lock_guard<std::mutex> l(g_lock);
    if (!Open()) return 0;
    XAUDIO2_VOICE_STATE st;
    g_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (uint64_t)st.BuffersQueued * 10;   // the mixer sends 10 ms pieces
}

void* FindGvAudio32(const char* dll, const char* func, int& args) {
    if (_stricmp(dll, "gvaudio.dll")) return nullptr;
    if (!strcmp(func, "GvAudioWrite")) return args = 2, (void*)G_Write;
    if (!strcmp(func, "GvAudioQueuedMs")) return args = 0, (void*)G_QueuedMs;
    return nullptr;
}

}  // namespace arcade
