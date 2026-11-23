// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// FILE-BACKED BIG BLOCKS: an Xbox game app has ~5 GB in all, and some arcade games read whole data files into memory
// (Star Wars Battle Pod: every Wwise sound bank, 794 MB of music alone). A big block the game asks for (32 MB+) is
// only reserved here; when the game then reads a file into the whole block in one go, the file itself is mapped
// there read-only instead of being copied, so those pages come from the disk when used and are not the app's own
// memory. Reads in pieces (Wwise reads banks 128 KB at a time) are LAZY: only noted (which file, where), and a page
// is read from the file when the game first touches it - sound the game never plays never takes memory. Pages
// touched otherwise get ordinary memory on first touch.

#include "peload.h"
#include <windows.h>
#include <mutex>
#include <vector>

namespace arcade {

enum class BigState { Reserved, Touched, Mapped };
struct Extent {     // a lazy read: block bytes [at, at+len) are file bytes from 'from' of 'file'
    size_t at;
    uint64_t from;
    uint32_t len;
    HANDLE file;    // our copy of the game's handle
};
struct Big {
    uint8_t* base;
    size_t size;     // what the game asked for
    size_t span;     // reserved (rounded to 64 KB)
    BigState state;
    std::vector<uint8_t> has;      // per 4 KB page: has memory
    std::vector<Extent> lazy;
    std::vector<HANDLE> files;     // our handle copies (closed with the block)
    HANDLE lastGame = nullptr, lastOurs = nullptr;
};
static std::mutex g_bigLock;
static std::vector<Big> g_big;
static int64_t g_mappedBytes = 0, g_lazyBytes = 0, g_lazyLoaded = 0;
static const size_t kBigMin = 32u << 20;

static Big* FindBig(const void* p) {   // (lock held) the block that contains p
    for (Big& b : g_big)
        if ((const uint8_t*)p >= b.base && (const uint8_t*)p < b.base + b.span) return &b;
    return nullptr;
}

// positional read from our handle copy. Read whole 4 KB sectors into a buffer of ours (files opened for unbuffered
// reads take nothing else), and the event's low bit set so the read is not also posted to a completion port the game
// tied the file to (its own I/O thread would get a read it never made)
static DWORD ReadAt(HANDLE f, uint64_t from, void* to, DWORD len) {
    if (NetFsHandle(f)) return (DWORD)NetFsPread(f, from, to, len);   // a file on the NAS
    uint64_t lo = from & ~0xFFFull, hi = (from + len + 0xFFF) & ~0xFFFull;
    static thread_local uint8_t* buf = nullptr;
    static thread_local size_t bufSize = 0;
    if (bufSize < hi - lo) {
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        bufSize = (size_t)(hi - lo);
        buf = (uint8_t*)VirtualAlloc(nullptr, bufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) {
            bufSize = 0;
            return 0;
        }
    }
    static thread_local HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED o = {};
    o.Offset = (DWORD)lo;
    o.OffsetHigh = (DWORD)(lo >> 32);
    o.hEvent = (HANDLE)((uintptr_t)ev | 1);
    ResetEvent(ev);
    DWORD got = 0;
    if (!ReadFile(f, buf, (DWORD)(hi - lo), &got, &o)) {
        if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(f, &o, &got, TRUE)) {
            static int told = 0;
            if (told++ < 5) Log("file-backed: read at %llu failed (%lu)", (unsigned long long)lo, GetLastError());
            return 0;
        }
    }
    size_t skip = (size_t)(from - lo);
    if (got <= skip) return 0;
    DWORD n = (DWORD)(got - skip < len ? got - skip : len);
    memcpy(to, buf + skip, n);
    return n;
}
// (lock held) page i gets memory, then the file bytes noted for it
static bool CommitPage(Big& b, size_t i) {
    if (b.has[i]) return true;
    uint8_t* page = b.base + i * 0x1000;
    if (!VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) return false;
    b.has[i] = 1;
    size_t lo = i * 0x1000, hi = lo + 0x1000;
    for (const Extent& e : b.lazy) {
        size_t a = e.at > lo ? e.at : lo, z = e.at + e.len < hi ? e.at + e.len : hi;
        if (a >= z) continue;
        ReadAt(e.file, e.from + (a - e.at), b.base + a, (DWORD)(z - a));
        g_lazyLoaded += (int64_t)(z - a);
    }
    return true;
}

static bool FileBackedOn() {
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableA("ARCADE_FILEBACKED", nullptr, 0) > 0 &&
                     GetEnvironmentVariableA("ARCADE_NO_FILEBACKED", nullptr, 0) == 0;   // (testing: off)
    return on != 0;
}
void SetFileBacked(bool on) {
    if (on) SetEnvironmentVariableA("ARCADE_FILEBACKED", "1");
}

static LONG CALLBACK BigTouch(PEXCEPTION_POINTERS e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* at = (uint8_t*)e->ExceptionRecord->ExceptionInformation[1];
    std::lock_guard<std::mutex> l(g_bigLock);
    Big* b = FindBig(at);
    if (!b) return EXCEPTION_CONTINUE_SEARCH;
    if (b->state == BigState::Mapped) {
        static int told = 0;
        if (told++ < 5)
            Log("file-backed block %p (%llu MB): the game WROTE into it at +0x%llx", b->base,
                (unsigned long long)(b->size >> 20), (unsigned long long)(at - b->base));
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // first touch of a page: memory for it, and the file bytes noted for it
    if (!CommitPage(*b, (size_t)(at - b->base) / 0x1000)) return EXCEPTION_CONTINUE_SEARCH;
    b->state = BigState::Touched;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void* BigAllocMaybe(size_t size) {
    if (size < kBigMin || !FileBackedOn()) return nullptr;
    static bool watching = false;
    if (!watching) {
        watching = true;
        typedef PVOID(WINAPI * add_t)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        auto add = (add_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "AddVectoredExceptionHandler");
        if (add) add(1, BigTouch);
    }
    size_t span = (size + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, span, MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    std::lock_guard<std::mutex> l(g_bigLock);
    Big nb;
    nb.base = p;
    nb.size = size;
    nb.span = span;
    nb.state = BigState::Reserved;
    nb.has.assign(span / 0x1000, 0);
    g_big.push_back(std::move(nb));
    return p;
}
bool BigFree(void* p) {
    std::lock_guard<std::mutex> l(g_bigLock);
    for (size_t i = 0; i < g_big.size(); i++) {
        if (g_big[i].base != p) continue;
        Big b = std::move(g_big[i]);
        g_big.erase(g_big.begin() + i);
        for (const Extent& e : b.lazy) g_lazyBytes -= e.len;
        for (HANDLE f : b.files) CloseHandle(f);
        if (b.state == BigState::Mapped) {
            UnmapViewOfFile(b.base);
            g_mappedBytes -= (int64_t)b.size;
        } else {
            VirtualFree(b.base, 0, MEM_RELEASE);
        }
        return true;
    }
    return false;
}
bool BigSize(const void* p, size_t* size) {
    std::lock_guard<std::mutex> l(g_bigLock);
    for (Big& b : g_big)
        if (b.base == p) {
            *size = b.size;
            return true;
        }
    return false;
}
int64_t FileBackedBytes() {   // mapped, plus noted and never read
    std::lock_guard<std::mutex> l(g_bigLock);
    return g_mappedBytes + g_lazyBytes - g_lazyLoaded;
}

// a read that is not mapped: the system does not fill memory that is only reserved, so the pages get memory first
void BigPrepare(void* buf, unsigned long n) {
    if (!FileBackedOn() || !n) return;
    std::lock_guard<std::mutex> l(g_bigLock);
    Big* b = FindBig(buf);
    if (!b || b->state == BigState::Mapped) return;
    size_t first = (size_t)((uint8_t*)buf - b->base) / 0x1000;
    size_t end = (size_t)((uint8_t*)buf + n - b->base + 0xFFF) / 0x1000;
    if (end > b->has.size()) end = b->has.size();
    for (size_t i = first; i < end; i++) CommitPage(*b, i);
    b->state = BigState::Touched;
}

// a piece of a file read into a big block: noted, read when touched (pages that already have memory: read now)
bool BigLazyRead(void* h, void* buf, unsigned long n, unsigned long* done, void* ovp) {
    LPOVERLAPPED ov = (LPOVERLAPPED)ovp;
    if (!FileBackedOn() || n < 0x10000) return false;
    std::lock_guard<std::mutex> l(g_bigLock);
    Big* b = FindBig(buf);
    if (!b || b->state == BigState::Mapped || (uint8_t*)buf + n > b->base + b->span) return false;
    LARGE_INTEGER at = {}, fileSize = {};
    bool net = NetFsHandle(h);   // (a file on the NAS: its place and size from netfs)
    if (ov) at.QuadPart = ((LONGLONG)ov->OffsetHigh << 32) | ov->Offset;
    else if (net) at.QuadPart = (LONGLONG)NetFsPos(h);
    else if (!SetFilePointerEx(h, at, &at, FILE_CURRENT)) return false;
    if (net) fileSize.QuadPart = (LONGLONG)NetFsSize(h);
    else if (!GetFileSizeEx(h, &fileSize)) return false;
    if (at.QuadPart >= fileSize.QuadPart) return false;
    DWORD len = n;
    if ((uint64_t)at.QuadPart + len > (uint64_t)fileSize.QuadPart) len = (DWORD)(fileSize.QuadPart - at.QuadPart);
    if (b->lastGame != h || !b->lastOurs) {
        HANDLE ours = nullptr;
        if (net) {   // our own reference to the NAS file
            NetFsAddRef(h);
            ours = h;
        } else if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &ours, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            static int told = 0;
            if (told++ < 3) Log("file-backed: DuplicateHandle failed (%lu)", GetLastError());
            return false;
        }
        b->files.push_back(ours);
        b->lastGame = h;
        b->lastOurs = ours;
    }
    size_t off = (size_t)((uint8_t*)buf - b->base);
    b->lazy.push_back({off, (uint64_t)at.QuadPart, len, b->lastOurs});
    g_lazyBytes += len;
    // pages that already have memory get their part now
    for (size_t i = off / 0x1000; i <= (off + len - 1) / 0x1000; i++) {
        if (!b->has[i]) continue;
        size_t lo = i * 0x1000 > off ? i * 0x1000 : off;
        size_t hi = (i + 1) * 0x1000 < off + len ? (i + 1) * 0x1000 : off + len;
        ReadAt(b->lastOurs, at.QuadPart + (lo - off), b->base + lo, (DWORD)(hi - lo));
        g_lazyLoaded += (int64_t)(hi - lo);
    }
    if (!ov) {
        LARGE_INTEGER step;
        step.QuadPart = len;
        if (net) {   // (the NAS file's place: through its stand-in)
            typedef BOOL(WINAPI * sfp_t)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
            if (void* s = FindNetFsShim("kernel32.dll", "SetFilePointerEx")) ((sfp_t)s)(h, step, nullptr, FILE_CURRENT);
        } else {
            SetFilePointerEx(h, step, nullptr, FILE_CURRENT);
        }
    } else {
        ov->Internal = 0;
        ov->InternalHigh = len;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    if (done) *done = len;
    static int told = 0;
    if (told++ < 3) Log("file-backed: lazy read of %lu bytes at block %p +0x%llx", len, b->base, (unsigned long long)off);
    return true;
}

// the game reads a file into memory: when that memory is a whole untouched big block, the file is mapped there
bool BigRead(void* h, void* buf, unsigned long n, unsigned long* done, void* ovp) {
    LPOVERLAPPED ov = (LPOVERLAPPED)ovp;
    if (!FileBackedOn() || n < kBigMin) return false;
    std::lock_guard<std::mutex> l(g_bigLock);
    Big* b = FindBig(buf);
    if (!b || b->base != buf || b->state != BigState::Reserved || ((size_t)n + 0xFFF) / 0x1000 < (b->size + 0xFFF) / 0x1000)
        return false;
    LARGE_INTEGER at = {};
    if (ov) at.QuadPart = ((LONGLONG)ov->OffsetHigh << 32) | ov->Offset;
    else if (!SetFilePointerEx(h, at, &at, FILE_CURRENT)) return false;
    LARGE_INTEGER fileSize = {};
    if ((at.QuadPart & 0xFFFF) || !GetFileSizeEx(h, &fileSize) || at.QuadPart + n > fileSize.QuadPart) {
        Log("file-backed: %lu bytes from offset %lld cannot be mapped (file %lld bytes)", (unsigned long)n,
            (long long)at.QuadPart, (long long)fileSize.QuadPart);
        return false;
    }
    HANDLE map = CreateFileMappingFromApp(h, nullptr, PAGE_READONLY, 0, nullptr);
    if (!map) {
        Log("file-backed: CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }
    typedef PVOID(WINAPI * map3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    // (MapViewOfFile3FromApp refuses a chosen address on Windows 11: the plain one first)
    static map3_t map3 = (map3_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "MapViewOfFile3");
    if (!map3) map3 = (map3_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "MapViewOfFile3FromApp");
    if (!map3) {
        CloseHandle(map);
        Log("file-backed: no MapViewOfFile3");
        return false;
    }
    // the reservation makes way for the view at the same address
    VirtualFree(b->base, 0, MEM_RELEASE);
    void* view = map3(map, GetCurrentProcess(), b->base, (ULONG64)at.QuadPart, n, 0, PAGE_READONLY, nullptr, 0);
    CloseHandle(map);
    if (view != b->base) {
        if (view) UnmapViewOfFile(view);
        void* again = VirtualAlloc(b->base, b->span, MEM_RESERVE, PAGE_READWRITE);
        Log("file-backed: mapping at %p failed (%lu), block %s", b->base, GetLastError(), again ? "reserved again" : "LOST");
        return false;
    }
    b->state = BigState::Mapped;
    g_mappedBytes += (int64_t)b->size;
    if (!ov) {
        LARGE_INTEGER step;
        step.QuadPart = n;
        SetFilePointerEx(h, step, nullptr, FILE_CURRENT);
    } else {
        ov->Internal = 0;
        ov->InternalHigh = n;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    if (done) *done = n;
    Log("file-backed: %lu MB of a file mapped at %p (offset %lld), %lld MB mapped in all", (unsigned long)(n >> 20), view,
        (long long)at.QuadPart, (long long)(g_mappedBytes >> 20));
    return true;
}

}  // namespace arcade
