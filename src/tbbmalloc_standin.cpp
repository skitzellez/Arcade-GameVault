// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// tbbmalloc.dll stand-in (Unreal Engine 3 arcade games: Star Wars Battle Pod): TBB 3.0's allocator keeps every block
// it ever freed for itself, so a game's peak stays taken for good. On an Xbox the game and its graphics share ~5 GB,
// so the scalable_* functions go to the process heap instead, which gives big freed blocks back at once.
// Each block carries a small header: its start, size and a mark (msize, realloc, aligned blocks; the "safer_" forms
// hand a block that is not ours to the original function they are given).

#include "peload.h"
#include <windows.h>
#include <cstring>
#include <intrin.h>
#include <atomic>

namespace arcade {

static const uint64_t kMark = 0x474B4C4256544C41ull;   // 'ALTVBLKG'
struct Head {
    void* start;    // what HeapAlloc gave
    size_t size;    // what the game asked for
    uint64_t mark;
    uint64_t pad;   // (header: 32 bytes, keeps 16-byte alignment)
};
static HANDLE Heap() {
    static HANDLE h = GetProcessHeap();
    return h;
}
static Head* HeadOf(const void* p) { return p ? (Head*)p - 1 : nullptr; }
static bool Ours(const void* p) {
    size_t n;
    return p && (BigSize(p, &n) || HeadOf(p)->mark == kMark);
}
static size_t SizeOf(const void* p) {
    size_t n;
    return BigSize(p, &n) ? n : HeadOf(p)->size;
}

// (testing) how much the game holds through this allocator, and its big blocks (logged with the caller)
static std::atomic<int64_t> g_held{0}, g_peak{0};
static void* g_bigCaller = nullptr;
static void Held(int64_t delta) {
    int64_t now = (g_held += delta);
    int64_t p = g_peak.load();
    while (now > p && !g_peak.compare_exchange_weak(p, now)) {}
}
int64_t TbbHeldBytes() { return g_held.load(); }
int64_t TbbPeakBytes() { return g_peak.load(); }

static void* Alloc(size_t size, size_t align) {
    if (align <= 0x10000)
        if (void* big = BigAllocMaybe(size)) {   // filebacked.cpp: reserved, maybe a mapped file later
            Held((int64_t)size);
            return big;
        }
    if (align < 16) align = 16;
    size_t total = size + sizeof(Head) + align;
    uint8_t* raw = (uint8_t*)HeapAlloc(Heap(), 0, total ? total : 1);
    if (!raw) return nullptr;
    uintptr_t user = ((uintptr_t)raw + sizeof(Head) + align - 1) & ~(uintptr_t)(align - 1);
    Head* h = (Head*)user - 1;
    h->start = raw;
    h->size = size;
    h->mark = kMark;
    Held((int64_t)size);
    if (size >= (16u << 20)) {
        void* by = g_bigCaller;
        uint8_t* b = (uint8_t*)MappedImageBase(by);
        Log("game memory: big block %llu MB (from %s+0x%llx), game holds %lld MB", (unsigned long long)(size >> 20),
            MappedImageName(by), (unsigned long long)((uint8_t*)by - b), (long long)(g_held.load() >> 20));
    }
    return (void*)user;
}
static void Free(void* p) {
    if (!p) return;
    size_t bn;
    if (BigSize(p, &bn) && BigFree(p)) {
        Held(-(int64_t)bn);
        return;
    }
    Head* h = HeadOf(p);
    Held(-(int64_t)h->size);
    h->mark = 0;
    HeapFree(Heap(), 0, h->start);
}
static void* Realloc(void* p, size_t size, size_t align) {
    if (!p) return Alloc(size, align);
    if (!size) {
        Free(p);
        return nullptr;
    }
    void* n = Alloc(size, align);
    if (!n) return nullptr;
    size_t old = SizeOf(p);
    memcpy(n, p, old < size ? old : size);
    Free(p);
    return n;
}

static void* __cdecl T_malloc(size_t n) {
    g_bigCaller = _ReturnAddress();
    return Alloc(n, 16);
}
static void __cdecl T_free(void* p) { Free(p); }
static void* __cdecl T_calloc(size_t n, size_t s) {
    void* p = Alloc(n * s, 16);
    if (p) memset(p, 0, n * s);
    return p;
}
static void* __cdecl T_realloc(void* p, size_t n) {
    g_bigCaller = _ReturnAddress();
    return Realloc(p, n, 16);
}
static size_t __cdecl T_msize(void* p) { return p ? SizeOf(p) : 0; }
static void* __cdecl T_aligned_malloc(size_t n, size_t a) {
    g_bigCaller = _ReturnAddress();
    return Alloc(n, a);
}
static void __cdecl T_aligned_free(void* p) { Free(p); }
static void* __cdecl T_aligned_realloc(void* p, size_t n, size_t a) { return Realloc(p, n, a); }
static int __cdecl T_posix_memalign(void** out, size_t a, size_t n) {
    *out = Alloc(n, a);
    return *out ? 0 : 12;   // ENOMEM
}
// the "safer_" forms: a block that is not ours goes to the original function the caller passes
typedef void(__cdecl* free_t)(void*);
typedef size_t(__cdecl* msize_t)(void*);
typedef void*(__cdecl* realloc_t)(void*, size_t);
typedef void*(__cdecl* arealloc_t)(void*, size_t, size_t);
static void __cdecl T_safer_free(void* p, free_t orig) {
    if (!p) return;
    if (Ours(p)) Free(p);
    else if (orig) orig(p);
}
static size_t __cdecl T_safer_msize(void* p, msize_t orig) {
    if (!p) return 0;
    return Ours(p) ? SizeOf(p) : orig ? orig(p) : 0;
}
static void* __cdecl T_safer_realloc(void* p, size_t n, void* orig) {
    if (!p || Ours(p)) return Realloc(p, n, 16);
    return orig ? ((realloc_t)orig)(p, n) : nullptr;   // not ours (the C runtime's): its own realloc
}
static void* __cdecl T_safer_aligned_realloc(void* p, size_t n, size_t a, void* orig) {
    if (!p || Ours(p)) return Realloc(p, n, a);
    return orig ? ((arealloc_t)orig)(p, n, a) : nullptr;
}

struct TbbShim {
    const char* name;
    void* fn;
};
static const TbbShim kTbb[] = {
    {"scalable_malloc", (void*)T_malloc}, {"scalable_free", (void*)T_free}, {"scalable_calloc", (void*)T_calloc},
    {"scalable_realloc", (void*)T_realloc}, {"scalable_msize", (void*)T_msize},
    {"scalable_aligned_malloc", (void*)T_aligned_malloc}, {"scalable_aligned_free", (void*)T_aligned_free},
    {"scalable_aligned_realloc", (void*)T_aligned_realloc}, {"scalable_posix_memalign", (void*)T_posix_memalign},
    {"safer_scalable_free", (void*)T_safer_free}, {"safer_scalable_msize", (void*)T_safer_msize},
    {"safer_scalable_realloc", (void*)T_safer_realloc},
    {"safer_scalable_aligned_realloc", (void*)T_safer_aligned_realloc},
};
static bool TbbStandInOn() {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    return true;   // an Xbox: always
#else
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableA("ARCADE_TBB_STANDIN", nullptr, 0) > 0;
    return on != 0;
#endif
}
void* FindTbbShim(const char* dll, const char* func) {
    if (!func || _stricmp(dll, "tbbmalloc.dll") || !TbbStandInOn()) return nullptr;
    for (const TbbShim& s : kTbb)
        if (!strcmp(s.name, func)) return s.fn;
    return nullptr;
}

}  // namespace arcade
