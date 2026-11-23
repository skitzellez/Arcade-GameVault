// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
// Sega RingEdge / RingWide platform for its games (Caladrius AC): the services Sega's amLib talks to by PCP on
// 127.0.0.1:40100 and up (keychip, storage, network) and the board's drivers (\\.\columba: the BIOS's DMI table,
// \\.\mxsram: battery-backed SRAM, \\.\mxsuperio, \\.\mxsmbus). What the services answer and how the drivers
// behave follow njz3's windowsloader (src/hook/pcp.cpp, src/hook/games/ringedge.cpp, GPL-3.0); the SRAM's
// geometry is segatools' (Unlicense).
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>
#include "peload.h"

namespace arcade {

static bool g_on = false;
static std::string g_gameId = "SBVF", g_platform = "AAL", g_board = "AAL";
static std::wstring g_dir;   // the game's folder (the SRAM file)

// --- PCP: ">" prompt, a line "command=value&field=value", the answer line, a prompt again ----------------------
static void PcpValue(const char* req, const char* key, char* out, size_t size) {
    size_t n = strlen(key);
    out[0] = 0;
    for (const char* p = req; p && *p;) {
        if (!strncmp(p, key, n) && (p[n] == '=' || p[n] == '&' || !p[n])) {
            const char* v = p[n] == '=' ? p + n + 1 : p + n;
            size_t len = strcspn(v, "&");
            if (len >= size) len = size - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}
static void PcpAnswer(const char* req, char* resp, size_t size);
// the auth service (amAuth, RingEdge 2: ALL.Net / NESiCA), as aderumier's windowsarcadeloader answers it (GPL-3.0)
static void AuthAnswer(const char* req, char* resp, size_t size) {
    char command[128];
    size_t n = strcspn(req, "=&");
    if (n >= sizeof command) n = sizeof command - 1;
    memcpy(command, req, n);
    command[n] = 0;
    if (!strcmp(command, "current.status")) snprintf(resp, size, "%s=ok&error=0&icon=normal&exit=false", command);
    else if (!strcmp(command, "title.server.address")) snprintf(resp, size, "%s=ok&ipaddr=0", command);
    else if (!strcmp(command, "close.time")) snprintf(resp, size, "%s=ok&time=alltime", command);
    else if (!strcmp(command, "place.info")) snprintf(resp, size, "%s=ok&size=%d", command, 0xCE);
    else if (!strcmp(command, "serial.id")) snprintf(resp, size, "%s=ok&keychip=A72E-01A12345678&main=AAL0000000000", command);
    else if (!strcmp(command, "system.time")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(resp, size, "%s=ok&time=%04d%02d%02d%02d%02d%02d", command, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    } else snprintf(resp, size, "%s=ok", command);
}
static void PcpAnswer(const char* req, char* resp, size_t size) {
    char command[128], value[256];
    size_t n = strcspn(req, "=&");
    if (n >= sizeof command) n = sizeof command - 1;
    memcpy(command, req, n);
    command[n] = 0;
    PcpValue(req, command, value, sizeof value);
    if (!strcmp(command, "keychip.ds.compute") || !strcmp(command, "keychip.ssd.proof")) {   // made-up, never all zero
        int bytes = command[9] == 'd' ? 20 : 16;
        unsigned x = 0x9E3779B9u;
        for (const char* c = value; *c; c++) x = (x ^ (unsigned char)*c) * 0x01000193u;
        size_t k = snprintf(resp, size, "%s=", command);
        for (int i = 0; i < bytes && k + 3 < size; i++) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            k += snprintf(resp + k, size - k, "%02X", (x >> 8) & 0xFF);
        }
        return;
    }
    if (!strcmp(command, "request")) {   // the network / update services: "response=<op>&result=success&..."
        static const struct { const char *op, *fields; } ops[] = {
            {"query_nic_status", "status=1"}, {"query_ip_address", "ip_address=192.168.1.4"},
            {"query_subnetmask", "subnetmask=255.255.255.0"}, {"query_gateway", "gateway=192.168.1.254"},
            {"query_primary_dns", "primary_dns=192.168.1.254"}, {"query_secondary_dns", "secondary_dns=0.0.0.0"},
            {"query_mac_address", "mac_address=00:D0:F1:01:02:03"}, {"query_dhcp_status", "dhcp_status=3"},
            {"isupdate", "incompatible=0&server=127.0.0.1&originalf=notavailable&originalb=notavailable&patchf=notavailable&patchb=notavailable"},
            {"query_appdata_status", "status=available&id=SBWU"},
            {"get_status", "instruction_enable=0&dld_st=uptodate&deliver_version=0&release_time=0&order_time=0&work_time=0&work_version=0&patchb_time=0&patchb_version=0&patchb_status=0&patchf_time=0&patchf_version=0&patchf_status=0&auth_time=0&bbflag=0"},
        };
        const char* fields = "";
        for (auto& o : ops)
            if (!strcmp(value, o.op)) fields = o.fields;
        snprintf(resp, size, "response=%s&result=success%s%s", value, *fields ? "&" : "", fields);
        return;
    }
    if (!strcmp(command, "keychip.appboot.gameid")) { snprintf(resp, size, "%s=%s", command, g_gameId.c_str()); return; }
    if (!strcmp(command, "keychip.appboot.platformid")) { snprintf(resp, size, "%s=%s", command, g_platform.c_str()); return; }
    static const struct { const char *command, *value, *fields; } known[] = {
        {"mxmaster.foreground.getcount", nullptr, "count=1"}, {"keychip.appboot.region", "4", ""},
        {"keychip.appboot.modeltype", "2", ""}, {"keychip.appboot.systemflag", "0", ""},
        {"keychip.appboot.networkaddr", "192.168.1.0", ""}, {"keychip.billing.keyid", "A72E-01A12345678", ""},
        {"keychip.version", "0104", ""},
    };
    for (auto& k : known)
        if (!strcmp(command, k.command)) {
            const char* v = !strcmp(value, "?") && k.value ? k.value : value;
            snprintf(resp, size, "%s=%s%s%s", command, v, *k.fields ? "&" : "", k.fields);
            return;
        }
    snprintf(resp, size, "%s=%s", command, value);   // not known: its own words back (logged)
}
static DWORD WINAPI PcpServer(void* p) {
    unsigned short port = (unsigned short)(uintptr_t)p;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l == INVALID_SOCKET || bind(l, (sockaddr*)&a, sizeof a) || listen(l, 4)) {
        Log("RingEdge: cannot listen on 127.0.0.1:%u (error %d)", port, WSAGetLastError());
        return 0;
    }
    for (;;) {
        SOCKET s = accept(l, nullptr, nullptr);
        if (s == INVALID_SOCKET) { Sleep(100); continue; }
        static int told = 0;
        if (told++ < 20) Log("RingEdge: the game connected to service %u", port);
        if (port == 40191) {   // auth data: the place.info record (zeros), then closed
            char rec[0xCE] = {};
            send(s, rec, sizeof rec, 0);
            closesocket(s);
            continue;
        }
        if (port == 40107 || port == 40115) {   // binary data channels: nothing to send
            closesocket(s);
            continue;
        }
        char line[4096], resp[4096], last[256] = "";
        int used = 0;
        for (bool open = true; open;) {
            send(s, ">", 1, 0);
            int end = -1;
            while (end < 0) {
                for (int i = 0; i < used; i++)
                    if (line[i] == '\n') { end = i; break; }
                if (end >= 0) break;
                if (used >= (int)sizeof line - 1) used = 0;
                int got = recv(s, line + used, (int)sizeof line - 1 - used, 0);
                if (got <= 0) { open = false; break; }
                static int raw = 0;
                if (raw++ < 30) {   // (testing) the bytes as they come
                    char hex[200] = "";
                    for (int i = 0; i < got && i < 48; i++) sprintf(hex + strlen(hex), "%02X ", (uint8_t)line[used + i]);
                    Log("RingEdge %u: got %d bytes: %s", port, got, hex);
                }
                used += got;
            }
            if (!open) break;
            int len = end > 0 && line[end - 1] == '\r' ? end - 1 : end;
            char req[4096];
            memcpy(req, line, len);
            req[len] = 0;
            memmove(line, line + end + 1, used - end - 1);
            used -= end + 1;
            if (port == 40190) AuthAnswer(req, resp, sizeof resp - 2);
            else PcpAnswer(req, resp, sizeof resp - 2);
            if (strcmp(req, last)) Log("RingEdge %u: \"%s\" -> \"%s\"", port, req, resp);
            snprintf(last, sizeof last, "%s", req);
            strcat(resp, "\r\n");
            send(s, resp, (int)strlen(resp), 0);
        }
        closesocket(s);
    }
}

// --- the drivers ----------------------------------------------------------------------------------------------
static const HANDLE kColumba = (HANDLE)(uintptr_t)0x0A5C3C00, kSram = (HANDLE)(uintptr_t)0x0A5C3C10,
                    kSuperio = (HANDLE)(uintptr_t)0x0A5C3C20, kSmbus = (HANDLE)(uintptr_t)0x0A5C3C30;
static uint8_t g_bios[0x20000];   // 0xE0000..0xFFFFF: the DMI table at 0xE0000, its entry point at 0xF0000
static std::vector<uint8_t> g_sram;   // 512 KB
static uint64_t g_sramPos = 0;
static std::mutex g_devLock;
static int Dmi(uint8_t* p, int type, WORD handle, const uint8_t* fields, int nf, const char* const* strings, int ns) {
    int n = 0;
    p[n++] = (uint8_t)type, p[n++] = (uint8_t)(4 + nf), p[n++] = (uint8_t)handle, p[n++] = (uint8_t)(handle >> 8);
    memcpy(p + n, fields, nf);
    n += nf;
    for (int i = 0; i < ns; i++) {
        size_t len = strlen(strings[i]) + 1;
        memcpy(p + n, strings[i], len);
        n += (int)len;
    }
    if (!ns) p[n++] = 0;
    p[n++] = 0;
    return n;
}
static void BuildBios() {
    static const char* const bios[] = {"American Megatrends Inc.", "080015", "05/21/2010"};
    const char* const system[] = {"NEC", g_board.c_str()};
    const char* const oem[] = {"SEGA", g_board.c_str(), g_platform.c_str()};
    static const uint8_t biosF[14] = {1, 2, 0x00, 0xF0, 3, 0, 0x80, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t sysF[4] = {1, 2, 0, 0};
    static const uint8_t oemF[1] = {3};
    int n = 0;
    n += Dmi(g_bios + n, 0, 0, biosF, 14, bios, 3);
    n += Dmi(g_bios + n, 1, 1, sysF, 4, system, 2);
    n += Dmi(g_bios + n, 11, 2, oemF, 1, oem, 3);
    n += Dmi(g_bios + n, 127, 3, nullptr, 0, nullptr, 0);
    uint8_t* ep = g_bios + 0x10000;
    memcpy(ep, "_DMI_", 5);
    *(WORD*)(ep + 6) = 0x800;
    *(DWORD*)(ep + 8) = 0xE0000;
    *(WORD*)(ep + 12) = 4;
    ep[14] = 0x24;
    uint8_t sum = 0;
    for (int i = 0; i < 15; i++) sum += ep[i];
    ep[5] = (uint8_t)(0x100 - sum);
}
static std::wstring SramFile() { return g_dir + L"sram.bin"; }
static void SramSave() {
    HANDLE f = CreateFile2FromAppW(SramFile().c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, g_sram.data(), (DWORD)g_sram.size(), &w, nullptr);
    CloseHandle(f);
}
HANDLE RingEdgeOpen(const char* name) {
    if (!g_on || !name) return nullptr;
    const char* n = !_strnicmp(name, "\\\\.\\", 4) ? name + 4 : name;
    HANDLE h = !_stricmp(n, "columba") ? kColumba : !_stricmp(n, "mxsram") ? kSram : !_stricmp(n, "mxsuperio") ? kSuperio
             : !_stricmp(n, "mxsmbus") ? kSmbus : nullptr;
    if (h) {
        static int told = 0;
        if (told++ < 10) Log("RingEdge: the game opened %s (emulated driver)", name);
    }
    return h;
}
bool RingEdgeHandle(HANDLE h) { return g_on && (h == kColumba || h == kSram || h == kSuperio || h == kSmbus); }
// --- \.\mxsmbus: the EEPROM (amSysData, 64 KB, LocalState-side eeprom.bin beside sram.bin) and a PCA9535 port expander
// (address 0x20: port 0 the DIP switches, port 1 bits 2/3 the board's Test and Service buttons, active low). A request:
// status, protocol, address, command, length, 32 data bytes (the _EX form: 16-bit address and command). As
// njz3/windowsloader ringedge.cpp (GPL-3.0).
static std::vector<uint8_t> g_eeprom;
static uint8_t g_smbusRegs[128][256];
static std::wstring EepromFile() { return g_dir + L"eeprom.bin"; }
static void EepromSave() {
    HANDLE f = CreateFile2FromAppW(EepromFile().c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, g_eeprom.data(), (DWORD)g_eeprom.size(), &w, nullptr);
    CloseHandle(f);
}
static void EepromLoad() {
    g_eeprom.assign(0x10000, 0);
    HANDLE f = CreateFile2FromAppW(EepromFile().c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD r = 0;
    ReadFile(f, g_eeprom.data(), (DWORD)g_eeprom.size(), &r, nullptr);
    CloseHandle(f);
}
static uint8_t Pca9535(int port) {
    if (port == 0) return 0xFF;   // DIP switches: all off
    PadIn p = CurrentPad();
    bool view = (p.buttons & 0x20) != 0;
    uint8_t v = 0xFF;
    if (view && (p.buttons & 0x10)) v &= ~0x04;    // View + Menu: Test
    if (view && (p.buttons & 0x100)) v &= ~0x08;   // View + LB: Service
    return v;
}
static DWORD Smbus(bool ex, const void* in, DWORD inSize, void* out, DWORD outSize) {
    DWORD size = ex ? 39 : 37;
    if (inSize < size || outSize < size) {
        if (out && outSize) memset(out, 0, outSize);
        return outSize;
    }
    uint8_t* r = (uint8_t*)out;
    memmove(r, in, size);
    int protocol = r[1];
    int address = ex ? *(const WORD*)(r + 2) : r[2];
    int command = ex ? *(const WORD*)(r + 4) : r[3];
    int length = ex ? r[6] : r[4];
    uint8_t* data = r + (ex ? 7 : 5);
    if (length > 32) length = 32;
    uint8_t* regs = g_smbusRegs[address & 0x7F];
    std::lock_guard<std::mutex> l(g_devLock);
    switch (protocol) {
    case 4: regs[command & 0xFF] = data[0]; break;   // write a byte
    case 5: data[0] = address == 0x20 && command < 2 ? Pca9535(command) : regs[command & 0xFF]; break;   // read a byte
    case 8:   // write a block (the EEPROM)
        if (command >= 0 && command + length <= (int)g_eeprom.size()) {
            memcpy(g_eeprom.data() + command, data, length);
            EepromSave();
        }
        break;
    case 9:   // read a block
        if (command >= 0 && command + length <= (int)g_eeprom.size()) memcpy(data, g_eeprom.data() + command, length);
        break;
    }
    static int told = 0;
    if (protocol != 9 && told++ < 20) Log("RingEdge: SMBus protocol %d address %02x command %04x length %d", protocol, address, command, length);
    r[0] = 0;   // done
    return size;
}
bool RingEdgeIoctl(HANDLE h, DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize, DWORD* ret) {
    if (!RingEdgeHandle(h)) return false;
    DWORD got = 0;
    if (h == kColumba && code == 0x9C406104 && inSize >= 16) {   // read physical memory: the BIOS area
        uint64_t at = *(const uint64_t*)in;
        DWORD count = *(const DWORD*)((const uint8_t*)in + 12);
        if (count > outSize) count = outSize;
        for (DWORD i = 0; i < count; i++) {
            uint64_t a = at + i;
            ((uint8_t*)out)[i] = a >= 0xE0000 && a < 0xE0000 + sizeof g_bios ? g_bios[a - 0xE0000] : 0;
        }
        got = count;
    } else if (code == 0x9C406000 && outSize >= 2) {   // the driver interface's version: 1 for the SRAM, 0x100 the others
        memset(out, 0, outSize);
        *(WORD*)out = 1;   // (RingEdge amLib: amSramInit, amEepromInit, amDipswInit want the low word 1; 0x100 refused)
        got = outSize >= 4 ? 4 : 2;
    } else if (h == kSram && code == 0x9C406004 && outSize >= 4) {   // its sector size
        *(DWORD*)out = 512;
        got = 4;
    } else if (h == kSram && code == 0x70000 /*IOCTL_DISK_GET_DRIVE_GEOMETRY*/ && outSize >= 24) {
        struct Geometry { int64_t cylinders; DWORD media, tracksPerCylinder, sectorsPerTrack, bytesPerSector; };
        Geometry* g = (Geometry*)out;
        memset(g, 0, sizeof *g);
        g->cylinders = 1, g->media = 12 /*FixedMedia*/, g->tracksPerCylinder = 1;
        g->sectorsPerTrack = (DWORD)(g_sram.size() / 512), g->bytesPerSector = 512;
        got = sizeof *g;
    } else if (h == kSmbus && code == 0x9C406008 && outSize >= 4) {   // the SMBus interface's version
        *(DWORD*)out = 1;
        got = 4;
    } else if (h == kSmbus && (code == 0x9C402004 || code == 0x9C40200C)) {
        got = Smbus(code == 0x9C40200C, in, inSize, out, outSize);
    } else {   // others: done, zeros (logged)
        if (out && outSize) memset(out, 0, outSize);
        got = outSize;
        static int told = 0;
        if (told++ < 20) Log("RingEdge: driver request 0x%08lx (%lu in, %lu out): zeros", code, inSize, outSize);
    }
    if (ret) *ret = got;
    return true;
}
bool RingEdgeRead(HANDLE h, void* b, DWORD n, DWORD* done) {
    if (!RingEdgeHandle(h)) return false;
    std::lock_guard<std::mutex> l(g_devLock);
    { static int told = 0; if (h == kSram && told++ < 12) Log("RingEdge: SRAM read %lu bytes at 0x%llx", n, (unsigned long long)g_sramPos); }
    DWORD k = 0;
    if (h == kSram)
        while (k < n && g_sramPos < g_sram.size()) ((uint8_t*)b)[k++] = g_sram[g_sramPos++];
    if (done) *done = k;
    return true;
}
bool RingEdgeWrite(HANDLE h, const void* b, DWORD n, DWORD* done) {
    { static int told = 0; if (h == kSram && told++ < 12) Log("RingEdge: SRAM write %lu bytes at 0x%llx", n, (unsigned long long)g_sramPos); }
    if (!RingEdgeHandle(h)) return false;
    std::lock_guard<std::mutex> l(g_devLock);
    DWORD k = 0;
    if (h == kSram) {
        while (k < n && g_sramPos < g_sram.size()) g_sram[g_sramPos++] = ((const uint8_t*)b)[k++];
        SramSave();
    }
    if (done) *done = n;
    return true;
}
bool RingEdgeSeek(HANDLE h, int64_t to, DWORD how, int64_t* now) {
    if (!RingEdgeHandle(h)) return false;
    std::lock_guard<std::mutex> l(g_devLock);
    int64_t base = how == FILE_CURRENT ? (int64_t)g_sramPos : how == FILE_END ? (int64_t)g_sram.size() : 0;
    g_sramPos = (uint64_t)(base + to);
    if (now) *now = (int64_t)g_sramPos;
    return true;
}
void RingEdgeStart(const std::wstring& gameDir, const char* gameId) {
    if (g_on) return;
    g_on = true;
    g_dir = gameDir;
    if (gameId) g_gameId = gameId;
    BuildBios();
    EepromLoad();
    g_sram.assign(0x80000, 0);
    HANDLE f = CreateFile2FromAppW(SramFile().c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD r = 0;
        ReadFile(f, g_sram.data(), (DWORD)g_sram.size(), &r, nullptr);
        CloseHandle(f);
    }
    for (unsigned short port = 40100; port <= 40114; port += 2)
        CloseHandle(CreateThread(nullptr, 0, PcpServer, (void*)(uintptr_t)port, 0, nullptr));
    for (unsigned short port : {40107, 40113, 40115, 40190, 40191})   // keychip/storage data, fetcher, auth (+ data)
        CloseHandle(CreateThread(nullptr, 0, PcpServer, (void*)(uintptr_t)port, 0, nullptr));
    Log("RingEdge: platform services on 127.0.0.1:40100-40114, drivers columba/mxsram/mxsuperio/mxsmbus (game %s)", g_gameId.c_str());
}
}  // namespace arcade
