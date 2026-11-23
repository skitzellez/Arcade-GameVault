// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// Winsock (ws2_32.dll) for 32-bit programs, by name or by ordinal (arcade games link cabinets with it: Ford Racing).
// Most calls go straight to the system's; the ones whose structures differ between 32 and 64 bits are converted:
// WSADATA (field order), fd_set (socket size) for select / __WSAFDIsSet, hostent and inet_ntoa (pointers made below
// 4 GB). Sockets are small numbers, the same in both.

#include "peload.h"
#include <winsock2.h>
#include <mutex>
#include <set>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstring>
#include <string>

namespace arcade {

static void* P(uint64_t v) { return (void*)(uintptr_t)(uint32_t)v; }
// the sockets this program made: a call on any other (closed, -1, garbage) returns WSAENOTSOCK without reaching
// Windows, whose strict handle checks in an app end the process on a bad handle (Battle Gear 4: ioctlsocket)
static std::mutex g_sockLock;
static std::set<uint32_t> g_socks;
static uint64_t Sock(SOCKET s) {
    if (s == INVALID_SOCKET) return 0xFFFFFFFFull;
    std::lock_guard<std::mutex> l(g_sockLock);
    g_socks.insert((uint32_t)s);
    return (uint64_t)(uint32_t)s;
}
static bool Known(uint64_t v) {
    std::lock_guard<std::mutex> l(g_sockLock);
    return g_socks.count((uint32_t)v) != 0;
}
static uint64_t NotSock() {
    static int told = 0;
    if (told++ < 5) Log("32-bit Winsock: a call on a socket that is not open: refused");
    WSASetLastError(WSAENOTSOCK);
    return 0xFFFFFFFFull;
}
static SOCKET S(uint64_t v) { return (uint32_t)v == 0xFFFFFFFFu ? INVALID_SOCKET : (SOCKET)(uint32_t)v; }

struct FdSet32 {
    uint32_t count;
    uint32_t sock[FD_SETSIZE];
};
static fd_set* In(uint64_t p32, fd_set& out) {
    if (!(uint32_t)p32) return nullptr;
    FdSet32* f = (FdSet32*)P(p32);
    out.fd_count = f->count < FD_SETSIZE ? f->count : FD_SETSIZE;
    for (u_int i = 0; i < out.fd_count; i++) out.fd_array[i] = (SOCKET)f->sock[i];
    return &out;
}
static void Out(uint64_t p32, const fd_set& in) {
    if (!(uint32_t)p32) return;
    FdSet32* f = (FdSet32*)P(p32);
    f->count = in.fd_count;
    for (u_int i = 0; i < in.fd_count; i++) f->sock[i] = (uint32_t)in.fd_array[i];
}

static uint64_t W_WSAStartup(uint64_t ver, uint64_t data) {
    WSADATA d = {};
    int r = WSAStartup((WORD)ver, &d);
    uint8_t* o = (uint8_t*)P(data);   // 32-bit WSADATA: versions, description[257], status[129], max sockets/UDP, vendor
    if (o) {
        memset(o, 0, 400);
        *(uint16_t*)o = d.wVersion;
        *(uint16_t*)(o + 2) = d.wHighVersion;
        memcpy(o + 4, d.szDescription, 257);
        memcpy(o + 261, d.szSystemStatus, 129);
        *(uint16_t*)(o + 390) = 32767;
        *(uint16_t*)(o + 392) = 65467;
    }
    static int told = 0;
    if (told++ < 2) Log("32-bit Winsock: WSAStartup %u.%u -> %d", (unsigned)(ver & 0xFF), (unsigned)(ver >> 8 & 0xFF), r);
    return (uint64_t)(uint32_t)r;
}
static uint64_t W_WSACleanup() { return (uint32_t)WSACleanup(); }
static uint64_t W_WSAGetLastError() { return (uint32_t)WSAGetLastError(); }
static uint64_t W_WSASetLastError(uint64_t e) {
    WSASetLastError((int)e);
    return 0;
}
static uint64_t W_socket(uint64_t af, uint64_t type, uint64_t proto) {
    SOCKET s = socket((int)af, (int)type, (int)proto);
    static int told = 0;
    if (told++ < 10) Log("32-bit Winsock: socket(%d, %d, %d) = %llu (error %d)", (int)af, (int)type, (int)proto,
                         (unsigned long long)s, s == INVALID_SOCKET ? WSAGetLastError() : 0);
    return Sock(s);
}
static uint64_t W_WSASocketA(uint64_t af, uint64_t type, uint64_t proto, uint64_t info, uint64_t g, uint64_t flags) {
    return Sock(WSASocketA((int)af, (int)type, (int)proto, (LPWSAPROTOCOL_INFOA)P(info), (GROUP)g, (DWORD)flags));
}
static uint64_t W_closesocket(uint64_t s) {
    if (!Known(s)) return NotSock();
    { std::lock_guard<std::mutex> l(g_sockLock); g_socks.erase((uint32_t)s); }
    return (uint32_t)closesocket(S(s));
}
static uint64_t W_bind(uint64_t s, uint64_t a, uint64_t n) { if (!Known(s)) return NotSock(); return (uint32_t)bind(S(s), (sockaddr*)P(a), (int)n); }
static uint64_t W_connect(uint64_t s, uint64_t a, uint64_t n) {
    if (!Known(s)) return NotSock();
    int r = connect(S(s), (sockaddr*)P(a), (int)n);
    static int told = 0;
    if (told++ < 30 && P(a)) {
        const sockaddr_in* in = (const sockaddr_in*)P(a);
        const uint8_t* ip = (const uint8_t*)&in->sin_addr;
        Log("32-bit Winsock: connect to %u.%u.%u.%u:%u = %d (error %d)", ip[0], ip[1], ip[2], ip[3], ntohs(in->sin_port), r, r ? WSAGetLastError() : 0);
    }
    return (uint32_t)r;
}
static uint64_t W_listen(uint64_t s, uint64_t n) { if (!Known(s)) return NotSock(); return (uint32_t)listen(S(s), (int)n); }
static uint64_t W_accept(uint64_t s, uint64_t a, uint64_t n) { if (!Known(s)) return NotSock(); return Sock(accept(S(s), (sockaddr*)P(a), (int*)P(n))); }
static uint64_t W_shutdown(uint64_t s, uint64_t how) { if (!Known(s)) return NotSock(); return (uint32_t)shutdown(S(s), (int)how); }
static uint64_t W_send(uint64_t s, uint64_t b, uint64_t n, uint64_t f) { if (!Known(s)) return NotSock(); return (uint32_t)send(S(s), (const char*)P(b), (int)n, (int)f); }
static uint64_t W_recv(uint64_t s, uint64_t b, uint64_t n, uint64_t f) { if (!Known(s)) return NotSock(); return (uint32_t)recv(S(s), (char*)P(b), (int)n, (int)f); }
static uint64_t W_sendto(uint64_t s, uint64_t b, uint64_t n, uint64_t f, uint64_t a, uint64_t al) {
    if (!Known(s)) return NotSock();
    return (uint32_t)sendto(S(s), (const char*)P(b), (int)n, (int)f, (const sockaddr*)P(a), (int)al);
}
static uint64_t W_recvfrom(uint64_t s, uint64_t b, uint64_t n, uint64_t f, uint64_t a, uint64_t al) {
    if (!Known(s)) return NotSock();
    return (uint32_t)recvfrom(S(s), (char*)P(b), (int)n, (int)f, (sockaddr*)P(a), (int*)P(al));
}
static uint64_t W_ioctlsocket(uint64_t s, uint64_t cmd, uint64_t arg) { if (!Known(s)) { Log("32-bit Winsock: ioctlsocket on %llu", (unsigned long long)(uint32_t)s); return NotSock(); } return (uint32_t)ioctlsocket(S(s), (long)cmd, (u_long*)P(arg)); }
static uint64_t W_setsockopt(uint64_t s, uint64_t lvl, uint64_t opt, uint64_t v, uint64_t n) {
    if (!Known(s)) return NotSock();
    return (uint32_t)setsockopt(S(s), (int)lvl, (int)opt, (const char*)P(v), (int)n);
}
static uint64_t W_getsockopt(uint64_t s, uint64_t lvl, uint64_t opt, uint64_t v, uint64_t n) {
    if (!Known(s)) return NotSock();
    return (uint32_t)getsockopt(S(s), (int)lvl, (int)opt, (char*)P(v), (int*)P(n));
}
static uint64_t W_getsockname(uint64_t s, uint64_t a, uint64_t n) { if (!Known(s)) return NotSock(); return (uint32_t)getsockname(S(s), (sockaddr*)P(a), (int*)P(n)); }
static uint64_t W_getpeername(uint64_t s, uint64_t a, uint64_t n) { if (!Known(s)) return NotSock(); return (uint32_t)getpeername(S(s), (sockaddr*)P(a), (int*)P(n)); }
static uint64_t W_select(uint64_t n, uint64_t r, uint64_t w, uint64_t e, uint64_t t) {
    fd_set rs, ws, es;
    fd_set* pr = In(r, rs);
    fd_set* pw = In(w, ws);
    fd_set* pe = In(e, es);
    int k = select(0, pr, pw, pe, (const timeval*)P(t));
    if (pr) Out(r, rs);
    if (pw) Out(w, ws);
    if (pe) Out(e, es);
    return (uint32_t)k;
}
static uint64_t W_WSAFDIsSet(uint64_t s, uint64_t set) {
    FdSet32* f = (FdSet32*)P(set);
    if (!f) return 0;
    for (uint32_t i = 0; i < f->count && i < FD_SETSIZE; i++)
        if (f->sock[i] == (uint32_t)s) return 1;
    return 0;
}
static uint64_t W_htons(uint64_t v) { return htons((u_short)v); }
static uint64_t W_htonl(uint64_t v) { return htonl((u_long)v); }
static uint64_t W_inet_addr(uint64_t s) { return (uint32_t)inet_addr((const char*)P(s)); }
static uint64_t W_inet_ntoa(uint64_t a) {   // in_addr by value; the text in memory a 32-bit program can reach
    static char* out = nullptr;
    if (!out) out = (char*)Alloc32(32);
    in_addr in;
    in.S_un.S_addr = (ULONG)a;
    inet_ntop(AF_INET, &in, out, 32);
    return (uint32_t)(uintptr_t)out;
}
static uint64_t W_gethostname(uint64_t b, uint64_t n) { return (uint32_t)gethostname((char*)P(b), (int)n); }
static uint64_t W_gethostbyname(uint64_t name) {   // a 32-bit hostent (pointers below 4 GB), IPv4 addresses
    static uint8_t* block = nullptr;
    if (!block) block = (uint8_t*)Alloc32(1024);
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo((const char*)P(name), nullptr, &hints, &res) || !res) {
        WSASetLastError(WSAHOST_NOT_FOUND);
        return 0;
    }
    memset(block, 0, 1024);
    uint32_t base = (uint32_t)(uintptr_t)block;
    char* text = (char*)block + 512;
    strncpy_s(text, 256, (const char*)P(name), _TRUNCATE);
    uint32_t* list = (uint32_t*)(block + 64);   // address pointers, then the addresses
    uint32_t* addrs = (uint32_t*)(block + 256);
    int n = 0;
    for (addrinfo* r = res; r && n < 16; r = r->ai_next) {
        addrs[n] = ((sockaddr_in*)r->ai_addr)->sin_addr.S_un.S_addr;
        list[n] = base + 256 + n * 4;
        n++;
    }
    freeaddrinfo(res);
    uint32_t* h = (uint32_t*)block;   // h_name, h_aliases, (h_addrtype, h_length), h_addr_list
    h[0] = base + 512;
    h[1] = base + 48;                 // an empty alias list
    *(uint16_t*)(block + 8) = AF_INET;
    *(uint16_t*)(block + 10) = 4;
    h[3] = base + 64;
    return base;
}
static uint64_t W_WSAIoctl(uint64_t s, uint64_t code, uint64_t in, uint64_t inLen, uint64_t out, uint64_t outLen, uint64_t ret,
                           uint64_t ov, uint64_t cr) {
    if ((uint32_t)ov || (uint32_t)cr) {   // overlapped: not for 32-bit structures here
        WSASetLastError(WSAEOPNOTSUPP);
        return 0xFFFFFFFFull;
    }
    return (uint32_t)WSAIoctl(S(s), (DWORD)code, P(in), (DWORD)inLen, P(out), (DWORD)outLen, (LPDWORD)P(ret), nullptr, nullptr);
}

struct WsFn {
    const char* name;
    int ord;
    int args;
    void* fn;
};
static const WsFn kWs[] = {
    {"accept", 1, 3, (void*)W_accept}, {"bind", 2, 3, (void*)W_bind}, {"closesocket", 3, 1, (void*)W_closesocket},
    {"connect", 4, 3, (void*)W_connect}, {"getpeername", 5, 3, (void*)W_getpeername}, {"getsockname", 6, 3, (void*)W_getsockname},
    {"getsockopt", 7, 5, (void*)W_getsockopt}, {"htonl", 8, 1, (void*)W_htonl}, {"htons", 9, 1, (void*)W_htons},
    {"ioctlsocket", 10, 3, (void*)W_ioctlsocket}, {"inet_addr", 11, 1, (void*)W_inet_addr}, {"inet_ntoa", 12, 1, (void*)W_inet_ntoa},
    {"listen", 13, 2, (void*)W_listen}, {"ntohl", 14, 1, (void*)W_htonl}, {"ntohs", 15, 1, (void*)W_htons},
    {"recv", 16, 4, (void*)W_recv}, {"recvfrom", 17, 6, (void*)W_recvfrom}, {"select", 18, 5, (void*)W_select},
    {"send", 19, 4, (void*)W_send}, {"sendto", 20, 6, (void*)W_sendto}, {"setsockopt", 21, 5, (void*)W_setsockopt},
    {"shutdown", 22, 2, (void*)W_shutdown}, {"socket", 23, 3, (void*)W_socket}, {"gethostbyname", 52, 1, (void*)W_gethostbyname},
    {"gethostname", 57, 2, (void*)W_gethostname}, {"WSAGetLastError", 111, 0, (void*)W_WSAGetLastError},
    {"WSASetLastError", 112, 1, (void*)W_WSASetLastError}, {"WSAStartup", 115, 2, (void*)W_WSAStartup},
    {"WSACleanup", 116, 0, (void*)W_WSACleanup}, {"__WSAFDIsSet", 151, 2, (void*)W_WSAFDIsSet},
    {"WSASocketA", 0, 6, (void*)W_WSASocketA}, {"WSAIoctl", 0, 9, (void*)W_WSAIoctl},
};
void* FindWinsock32(const char* dll, const char* func, int& args) {
    if (_stricmp(dll, "ws2_32.dll") && _stricmp(dll, "wsock32.dll")) return nullptr;
    int ord = func[0] == '#' ? atoi(func + 1) : 0;
    // wsock32.dll numbers three of them its own way: 10 inet_addr, 11 inet_ntoa, 12 ioctlsocket (ws2_32: 10
    // ioctlsocket, 11 inet_addr, 12 inet_ntoa). Battle Gear 4's inet_addr was called as ioctlsocket: wrong stack
    if (ord && !_stricmp(dll, "wsock32.dll")) {
        static const char* const own[] = {"inet_addr", "inet_ntoa", "ioctlsocket"};
        if (ord >= 10 && ord <= 12) {
            func = own[ord - 10];
            ord = 0;
        }
    }
    for (const WsFn& w : kWs)
        if ((ord && w.ord == ord) || (!ord && !strcmp(w.name, func))) {
            args = w.args;
            return w.fn;
        }
    return nullptr;
}

}  // namespace arcade
