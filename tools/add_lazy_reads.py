# one-off edit: lazy reads into big blocks (filebacked.cpp), wired into ReadFile (arcade_io.cpp)
from pathlib import Path

src = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src")


def edit(name, pairs):
    p = src / name
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert s.count(a) == 1, (name, a[:60])
        s = s.replace(a, b)
    p.write_text(s, encoding="utf-8")


edit("filebacked.cpp", [
("""// memory. A block touched before such a read gets ordinary memory page by page (on first touch), as before.""",
"""// memory. Reads in pieces (Wwise reads banks 128 KB at a time) are LAZY: only noted (which file, where), and a page
// is read from the file when the game first touches it - sound the game never plays never takes memory. Pages
// touched otherwise get ordinary memory on first touch."""),
("""struct Big {
    uint8_t* base;
    size_t size;     // what the game asked for
    size_t span;     // reserved (rounded to 64 KB)
    BigState state;
};""", """struct Extent {     // a lazy read: block bytes [at, at+len) are file bytes from 'from' of 'file'
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
};"""),
("""static int64_t g_mappedBytes = 0;""", """static int64_t g_mappedBytes = 0, g_lazyBytes = 0, g_lazyLoaded = 0;"""),
("""static bool FileBackedOn() {""", """// positional read from our handle copy (also for handles opened for overlapped reads)
static DWORD ReadAt(HANDLE f, uint64_t from, void* to, DWORD len) {
    OVERLAPPED o = {};
    o.Offset = (DWORD)from;
    o.OffsetHigh = (DWORD)(from >> 32);
    static thread_local HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    o.hEvent = ev;
    ResetEvent(ev);
    DWORD got = 0;
    if (!ReadFile(f, to, len, &got, &o)) {
        if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(f, &o, &got, TRUE)) return 0;
    }
    return got;
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

static bool FileBackedOn() {"""),
("""    // first touch of a page: ordinary memory for it (64 KB at a time)
    uint8_t* page = (uint8_t*)((uintptr_t)at & ~(uintptr_t)0xFFFF);
    size_t n = 0x10000;
    if (page + n > b->base + b->span) n = b->base + b->span - page;
    if (!VirtualAlloc(page, n, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    b->state = BigState::Touched;""", """    // first touch of a page: memory for it, and the file bytes noted for it
    if (!CommitPage(*b, (size_t)(at - b->base) / 0x1000)) return EXCEPTION_CONTINUE_SEARCH;
    b->state = BigState::Touched;"""),
("""    g_big.push_back({p, size, span, BigState::Reserved});
    return p;""", """    Big nb;
    nb.base = p;
    nb.size = size;
    nb.span = span;
    nb.state = BigState::Reserved;
    nb.has.assign(span / 0x1000, 0);
    g_big.push_back(std::move(nb));
    return p;"""),
("""        Big b = g_big[i];
        g_big.erase(g_big.begin() + i);""", """        Big b = std::move(g_big[i]);
        g_big.erase(g_big.begin() + i);
        for (const Extent& e : b.lazy) g_lazyBytes -= e.len;
        for (HANDLE f : b.files) CloseHandle(f);"""),
("""int64_t FileBackedBytes() {
    std::lock_guard<std::mutex> l(g_bigLock);
    return g_mappedBytes;
}""", """int64_t FileBackedBytes() {   // mapped, plus noted and never read
    std::lock_guard<std::mutex> l(g_bigLock);
    return g_mappedBytes + g_lazyBytes - g_lazyLoaded;
}"""),
("""    static int told = 0;
    if (told++ < 40) Log("file-backed (testing): read of %lu bytes at block %p +0x%llx (block %llu MB)", n, b->base,
                         (unsigned long long)((uint8_t*)buf - b->base), (unsigned long long)(b->size >> 20));
    uint8_t* from = (uint8_t*)((uintptr_t)buf & ~(uintptr_t)0xFFF);
    uint8_t* to = (uint8_t*)buf + n;
    if (to > b->base + b->span) to = b->base + b->span;
    if (VirtualAlloc(from, to - from, MEM_COMMIT, PAGE_READWRITE)) b->state = BigState::Touched;
}""", """    size_t first = (size_t)((uint8_t*)buf - b->base) / 0x1000;
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
    if (ov) at.QuadPart = ((LONGLONG)ov->OffsetHigh << 32) | ov->Offset;
    else if (!SetFilePointerEx(h, at, &at, FILE_CURRENT)) return false;
    if (!GetFileSizeEx(h, &fileSize) || at.QuadPart >= fileSize.QuadPart) return false;
    DWORD len = n;
    if ((uint64_t)at.QuadPart + len > (uint64_t)fileSize.QuadPart) len = (DWORD)(fileSize.QuadPart - at.QuadPart);
    if (b->lastGame != h || !b->lastOurs) {
        HANDLE ours = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &ours, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
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
        SetFilePointerEx(h, step, nullptr, FILE_CURRENT);
    } else {
        ov->Internal = 0;
        ov->InternalHigh = len;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    if (done) *done = len;
    static int told = 0;
    if (told++ < 3) Log("file-backed: lazy read of %lu bytes at block %p +0x%llx", len, b->base, (unsigned long long)off);
    return true;
}"""),
])
edit("arcade_io.cpp", [("""        BigPrepare(b, n);
""", """        if (BigLazyRead(h, b, n, done, ov)) return TRUE;   // a piece into a big block: read when touched
        BigPrepare(b, n);
""")])
edit("peload.h", [("void BigPrepare(void* buf, unsigned long n);",
                   "void BigPrepare(void* buf, unsigned long n);\nbool BigLazyRead(void* h, void* buf, unsigned long n, unsigned long* done, void* ov);")])
print("ok")
