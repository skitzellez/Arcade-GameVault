# one-off edit: winmm timers (timeSetEvent/timeKillEvent), the mmio RIFF file reader, iphlpapi GetInterfaceInfo /
# GetAdaptersInfo for 32-bit programs (peload32.cpp). Battle Gear 4 and Dragon Dance call them; the console has none.
from pathlib import Path

p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\peload32.cpp")
s = p.read_text(encoding="utf-8")

decl = r'''static uint64_t B_timeSetEvent(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_timeKillEvent(uint64_t);
static uint64_t B_mmioOpenA(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioClose(uint64_t, uint64_t);
static uint64_t B_mmioRead(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioWrite(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioSeek(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioDescend(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioAscend(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioGetInfo(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioSetInfo(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioAdvance(uint64_t, uint64_t, uint64_t);
static uint64_t B_mmioCreateChunk(uint64_t, uint64_t, uint64_t);
static uint64_t B_joyGetNumDevs() { return 0; }
// iphlpapi for 32-bit programs: one network adapter (index 1, 127.0.0.1 style), as 32-bit Windows lays them out
static uint64_t B_GetInterfaceInfo(uint64_t out, uint64_t sizeP) {   // IP_INTERFACE_INFO: 4 + 260 bytes
    uint32_t* size = sizeP ? (uint32_t*)(uintptr_t)(uint32_t)sizeP : nullptr;
    const uint32_t need = 4 + 4 + 256;
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!out || *size < need) { *size = need; return ERROR_INSUFFICIENT_BUFFER; }
    uint8_t* f = (uint8_t*)(uintptr_t)(uint32_t)out;
    memset(f, 0, need);
    *(int32_t*)f = 1;                 // NumAdapters
    *(uint32_t*)(f + 4) = 1;          // Adapter[0].Index
    const wchar_t* name = L"\\DEVICE\\TCPIP_{00000000-0000-0000-0000-000000000001}";
    memcpy(f + 8, name, (wcslen(name) + 1) * 2);
    return ERROR_SUCCESS;
}
static uint64_t B_GetAdaptersInfo(uint64_t out, uint64_t sizeP) {   // IP_ADAPTER_INFO, 648 bytes in 32-bit
    uint32_t* size = sizeP ? (uint32_t*)(uintptr_t)(uint32_t)sizeP : nullptr;
    const uint32_t need = 648;
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!out || *size < need) { *size = need; return ERROR_BUFFER_OVERFLOW; }
    uint8_t* f = (uint8_t*)(uintptr_t)(uint32_t)out;
    memset(f, 0, need);
    *(uint32_t*)(f + 4) = 1;                                                   // ComboIndex
    strcpy((char*)f + 8, "{00000000-0000-0000-0000-000000000001}");           // AdapterName[260]
    strcpy((char*)f + 268, "Game Vault network");                             // Description[132]
    *(uint32_t*)(f + 400) = 6;                                                 // AddressLength
    static const uint8_t mac[6] = {0x00, 0x15, 0x5D, 0x47, 0x56, 0x01};
    memcpy(f + 404, mac, 6);                                                   // Address[8]
    *(uint32_t*)(f + 412) = 1;                                                 // Index
    *(uint32_t*)(f + 416) = 6;                                                 // Type: Ethernet
    strcpy((char*)f + 432, "192.168.0.2");                                     // IpAddressList.IpAddress
    strcpy((char*)f + 448, "255.255.255.0");                                   // IpAddressList.IpMask
    strcpy((char*)f + 472, "192.168.0.1");                                     // GatewayList.IpAddress
    strcpy((char*)f + 488, "255.255.255.255");
    return ERROR_SUCCESS;
}
'''
anchor = "static uint64_t B_GlobalMemoryStatus(uint64_t out) {"
assert s.count(anchor) == 1
s = s.replace(anchor, decl + anchor)
row = '''    {"kernel32.dll", "GlobalMemoryStatus", 1, false, (void*)B_GlobalMemoryStatus},'''
s = s.replace(row, row + '''
    {"iphlpapi.dll", "GetInterfaceInfo", 2, false, (void*)B_GetInterfaceInfo},
    {"iphlpapi.dll", "GetAdaptersInfo", 2, false, (void*)B_GetAdaptersInfo},
    {"winmm.dll", "timeSetEvent", 5, false, (void*)B_timeSetEvent},
    {"winmm.dll", "timeKillEvent", 1, false, (void*)B_timeKillEvent},
    {"winmm.dll", "joyGetNumDevs", 0, false, (void*)B_joyGetNumDevs},
    {"winmm.dll", "mmioOpenA", 3, false, (void*)B_mmioOpenA},
    {"winmm.dll", "mmioClose", 2, false, (void*)B_mmioClose},
    {"winmm.dll", "mmioRead", 3, false, (void*)B_mmioRead},
    {"winmm.dll", "mmioWrite", 3, false, (void*)B_mmioWrite},
    {"winmm.dll", "mmioSeek", 3, false, (void*)B_mmioSeek},
    {"winmm.dll", "mmioDescend", 4, false, (void*)B_mmioDescend},
    {"winmm.dll", "mmioAscend", 3, false, (void*)B_mmioAscend},
    {"winmm.dll", "mmioGetInfo", 3, false, (void*)B_mmioGetInfo},
    {"winmm.dll", "mmioSetInfo", 3, false, (void*)B_mmioSetInfo},
    {"winmm.dll", "mmioAdvance", 3, false, (void*)B_mmioAdvance},
    {"winmm.dll", "mmioCreateChunk", 3, false, (void*)B_mmioCreateChunk},''')

impl = r'''
// --- winmm timers for 32-bit programs: timeSetEvent's callback runs on a thread of its own with a 32-bit stack,
// every <delay> ms (TIME_PERIODIC) or once; TIME_CALLBACK_EVENT_SET / _PULSE set an event instead
struct Timer32 {
    uint32_t id, delay, proc, user, flags;
    volatile bool stop = false;
};
static std::mutex g_timerLock;
static std::map<uint32_t, Timer32*> g_timers;
static uint32_t g_timerNext = 1;
static DWORD WINAPI Timer32Main(void* p) {
    Timer32* t = (Timer32*)p;
    uint8_t* stack = AllocBelow4G(256 << 10, 0x10000000);
    uint8_t* teb = stack ? MakeTeb32((uint32_t)(uintptr_t)(stack + (256 << 10) - 16), (uint32_t)(uintptr_t)stack) : nullptr;
    if (!teb) return 0;
    St()->teb32 = (uint64_t)teb;
    St()->stackTop = (uint32_t)(uintptr_t)(stack + (256 << 10) - 16);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    do {
        next.QuadPart += freq.QuadPart * t->delay / 1000;
        for (;;) {
            QueryPerformanceCounter(&now);
            int64_t left = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
            if (left <= 0 || t->stop) break;
            Sleep(left > 2 ? (DWORD)left - 1 : 0);
        }
        if (t->stop) break;
        if (t->flags & 0x30) {   // TIME_CALLBACK_EVENT_SET (0x10) / _PULSE (0x20)
            HANDLE e = (HANDLE)(intptr_t)(int32_t)t->proc;
            if (t->flags & 0x20) PulseEvent(e); else SetEvent(e);
        } else {
            uint32_t a[5] = {t->id, 0, t->user, 0, 0};
            Call32(t->proc, a, 5);
        }
    } while (t->flags & 1);   // TIME_PERIODIC
    return 0;
}
static uint64_t B_timeSetEvent(uint64_t delay, uint64_t, uint64_t proc, uint64_t user, uint64_t flags) {
    auto t = new Timer32{0, (uint32_t)delay ? (uint32_t)delay : 1, (uint32_t)proc, (uint32_t)user, (uint32_t)flags};
    {
        std::lock_guard<std::mutex> l(g_timerLock);
        t->id = g_timerNext++;
        g_timers[t->id] = t;
    }
    HANDLE h = CreateThread(nullptr, 0, Timer32Main, t, 0, nullptr);
    if (!h) return 0;
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(h);
    static int told = 0;
    if (told++ < 4) Log("32-bit timer %u: every %u ms%s", t->id, t->delay, (flags & 1) ? "" : " (once)");
    return t->id;
}
static uint64_t B_timeKillEvent(uint64_t id) {
    std::lock_guard<std::mutex> l(g_timerLock);
    auto it = g_timers.find((uint32_t)id);
    if (it == g_timers.end()) return 97;   // MMSYSERR_INVALPARAM
    it->second->stop = true;   // (the thread ends by itself; the record stays, it is small)
    g_timers.erase(it);
    return 0;
}

// --- mmio for 32-bit programs: RIFF (wave) files read whole into memory below 2 GB; buffered access (GetInfo /
// Advance / SetInfo) points straight into that copy
struct Mmio32 {
    uint8_t* data = nullptr;   // below 2 GB
    uint32_t size = 0, pos = 0;
    bool own = false;
};
static std::mutex g_mmioLock;
static std::map<uint32_t, Mmio32> g_mmio;
static uint32_t g_mmioNext = 0x7F000000;
static Mmio32* MmioGet(uint64_t h) {
    auto it = g_mmio.find((uint32_t)h);
    return it == g_mmio.end() ? nullptr : &it->second;
}
#define M32(p) ((uint8_t*)(uintptr_t)(uint32_t)(p))
static uint64_t B_mmioOpenA(uint64_t name, uint64_t info, uint64_t flags) {
    Mmio32 m;
    uint8_t* mi = info ? M32(info) : nullptr;
    if (!name && mi && *(uint32_t*)(mi + 4) == 0x204D454D) {   // FOURCC_MEM: a file in the game's memory
        m.data = M32(*(uint32_t*)(mi + 24));
        m.size = *(uint32_t*)(mi + 20);
    } else if (name) {
        std::string n = (const char*)M32(name);
        std::wstring w(n.begin(), n.end());
        std::vector<uint8_t> all = ReadFile32(w);
        if (all.empty()) {
            if (mi) *(uint32_t*)(mi + 12) = 257;   // MMIOERR_FILENOTFOUND
            return 0;
        }
        m.data = (uint8_t*)Alloc32(all.size());
        if (!m.data) return 0;
        memcpy(m.data, all.data(), all.size());
        m.size = (uint32_t)all.size();
        m.own = true;
    } else {
        return 0;
    }
    std::lock_guard<std::mutex> l(g_mmioLock);
    g_mmioNext += 4;
    g_mmio[g_mmioNext] = m;
    if (mi) *(uint32_t*)(mi + 68) = g_mmioNext;
    return g_mmioNext;
}
static uint64_t B_mmioClose(uint64_t h, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    g_mmio.erase((uint32_t)h);   // (the copy stays in the low heap; wave files are small)
    return 0;
}
static uint64_t B_mmioRead(uint64_t h, uint64_t buf, uint64_t n) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m) return (uint64_t)-1;
    uint32_t k = std::min<uint32_t>((uint32_t)n, m->size - std::min(m->pos, m->size));
    memcpy(M32(buf), m->data + m->pos, k);
    m->pos += k;
    return k;
}
static uint64_t B_mmioWrite(uint64_t, uint64_t, uint64_t) { return (uint64_t)-1; }
static uint64_t B_mmioSeek(uint64_t h, uint64_t off, uint64_t origin) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m) return (uint64_t)-1;
    int64_t base = origin == 1 ? m->pos : origin == 2 ? m->size : 0;
    int64_t at = base + (int32_t)off;
    if (at < 0) return (uint64_t)-1;
    m->pos = (uint32_t)at;
    return m->pos;
}
static uint32_t Rd32(Mmio32* m, uint32_t at) { return at + 4 <= m->size ? *(uint32_t*)(m->data + at) : 0; }
// MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags
static uint64_t B_mmioDescend(uint64_t h, uint64_t ckP, uint64_t parentP, uint64_t flags) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !ckP) return 265;   // MMIOERR_CHUNKNOTFOUND
    uint32_t* ck = (uint32_t*)M32(ckP);
    uint32_t end = m->size;
    if (parentP) {
        uint32_t* par = (uint32_t*)M32(parentP);
        end = std::min(end, par[3] + par[1]);
    }
    uint32_t wantId = ck[0], wantType = ck[2];
    for (uint32_t at = m->pos; at + 8 <= end;) {
        uint32_t id = Rd32(m, at), size = Rd32(m, at + 4);
        bool list = id == 0x46464952 || id == 0x5453494C;   // "RIFF" / "LIST"
        uint32_t type = list ? Rd32(m, at + 8) : 0;
        bool hit = (flags & 0x20) ? (id == 0x46464952 && type == wantType)
                 : (flags & 0x40) ? (id == 0x5453494C && type == wantType)
                 : (flags & 0x10) ? id == wantId
                                  : true;
        if (hit) {
            ck[0] = id, ck[1] = size, ck[2] = type, ck[3] = at + 8, ck[4] = 0;
            m->pos = at + 8 + (list ? 4 : 0);
            return 0;
        }
        at += 8 + size + (size & 1);
    }
    return 265;
}
static uint64_t B_mmioAscend(uint64_t h, uint64_t ckP, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !ckP) return 265;
    uint32_t* ck = (uint32_t*)M32(ckP);
    m->pos = ck[3] + ck[1] + (ck[1] & 1);
    return 0;
}
// MMIOINFO (72 bytes): cchBuffer +20, pchBuffer +24, pchNext +28, pchEndRead +32, pchEndWrite +36, lBufOffset +40,
// lDiskOffset +44, hmmio +68
static uint64_t B_mmioGetInfo(uint64_t h, uint64_t info, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !info) return 258;   // MMIOERR_INVALIDFILE
    uint8_t* mi = M32(info);
    memset(mi, 0, 72);
    uint32_t base = (uint32_t)(uintptr_t)m->data;
    *(uint32_t*)(mi + 20) = m->size;
    *(uint32_t*)(mi + 24) = base;
    *(uint32_t*)(mi + 28) = base + std::min(m->pos, m->size);
    *(uint32_t*)(mi + 32) = base + m->size;
    *(uint32_t*)(mi + 36) = base + m->size;
    *(uint32_t*)(mi + 44) = m->size;
    *(uint32_t*)(mi + 68) = (uint32_t)h;
    return 0;
}
static uint64_t B_mmioSetInfo(uint64_t h, uint64_t info, uint64_t) {
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !info) return 258;
    uint32_t next = *(uint32_t*)(M32(info) + 28);
    m->pos = next - (uint32_t)(uintptr_t)m->data;
    return 0;
}
static uint64_t B_mmioAdvance(uint64_t h, uint64_t info, uint64_t) {   // the whole file is the buffer: nothing more
    std::lock_guard<std::mutex> l(g_mmioLock);
    Mmio32* m = MmioGet(h);
    if (!m || !info) return 258;
    uint8_t* mi = M32(info);
    uint32_t base = (uint32_t)(uintptr_t)m->data;
    m->pos = std::min(*(uint32_t*)(mi + 28) - base, m->size);
    *(uint32_t*)(mi + 32) = base + m->size;
    return 0;
}
static uint64_t B_mmioCreateChunk(uint64_t, uint64_t, uint64_t) { return 262; }   // MMIOERR_CANNOTWRITE
'''
anchor2 = "// --- threads a 32-bit program starts:"
assert s.count(anchor2) == 1
s = s.replace(anchor2, impl + "\n" + anchor2)
p.write_text(s, encoding="utf-8")
print("ok")
