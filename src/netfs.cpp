// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// GAMES PLAYED STRAIGHT FROM THE NAS (no copy first): Game Vault's link names the game's program on a share
// (smb://server/share/folder/.../game.exe) with root= (the game's folder) and netcred= (its one-time sign-in file,
// lines "server<TAB>share<TAB>user<TAB>password", read once, written over and deleted, as for RPCS3 for Game Vault).
// The game's folder is then \\gvnas\game\... for the program: every file call on that path - open, read, seek, size,
// attributes, folder listings, the current folder, full paths, file mappings - is answered from the share through
// libsmb2 (smb2.dll, the one Game Vault ships). What the game writes goes to LocalState\nas overlay\<game>\... and is
// read from there afterwards; the NAS copy is never changed.

#include "peload.h"
#include <winsock2.h>
#include <windows.h>
#include <fileapifromapp.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arcade {

namespace {
struct smb2_context;
struct smb2fh;
struct smb2dir;
struct Stat64 {
    uint32_t type, nlink;
    uint64_t ino, size, atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec, btime, btime_nsec;
};
struct DirEnt {
    const char* name;
    Stat64 st;
};
struct Api {
    smb2_context* (*init_context)();
    void (*destroy_context)(smb2_context*);
    void (*set_timeout)(smb2_context*, int);
    void (*set_security_mode)(smb2_context*, uint16_t);
    void (*set_domain)(smb2_context*, const char*);
    void (*set_password)(smb2_context*, const char*);
    int (*connect_share)(smb2_context*, const char*, const char*, const char*);
    int (*disconnect_share)(smb2_context*);
    smb2fh* (*open)(smb2_context*, const char*, int);
    int (*close)(smb2_context*, smb2fh*);
    int (*pread)(smb2_context*, smb2fh*, uint8_t*, uint32_t, uint64_t);
    uint32_t (*get_max_read_size)(smb2_context*);
    const char* (*get_error)(smb2_context*);
    smb2dir* (*opendir)(smb2_context*, const char*);
    DirEnt* (*readdir)(smb2_context*, smb2dir*);
    void (*closedir)(smb2_context*, smb2dir*);
    int (*stat)(smb2_context*, const char*, Stat64*);
} g_api;

const wchar_t kRoot[] = L"\\\\gvnas\\game";   // the game's folder as the program sees it
std::recursive_mutex g_lock;                   // (a libsmb2 context is for one thread at a time)
smb2_context* g_ctx = nullptr;
uint32_t g_maxRead = 1 << 20;
std::string g_server, g_share, g_base;         // base: the game's folder in the share ("Arcade\Ford Racing")
std::string g_user = "Guest", g_password, g_domain;
std::wstring g_overlay;                        // LocalState\nas overlay\<game folder name>
std::wstring g_cwd;                            // the current folder when it is in the game's folder ("" else)
bool g_on = false;

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 1 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::wstring FromAnsi(const char* s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 1 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

bool Connect() {   // (lock held)
    if (g_ctx) return true;
    g_ctx = g_api.init_context();
    if (!g_ctx) return false;
    g_api.set_timeout(g_ctx, 15);
    g_api.set_security_mode(g_ctx, 0x0001);   // signing enabled
    if (!g_domain.empty()) g_api.set_domain(g_ctx, g_domain.c_str());
    g_api.set_password(g_ctx, g_password.c_str());
    if (g_api.connect_share(g_ctx, g_server.c_str(), g_share.c_str(), g_user.c_str()) < 0) {
        Log("NAS: %s\\%s: %s", g_server.c_str(), g_share.c_str(), g_api.get_error(g_ctx));
        g_api.destroy_context(g_ctx);
        g_ctx = nullptr;
        return false;
    }
    uint32_t m = g_api.get_max_read_size(g_ctx);
    g_maxRead = std::max<uint32_t>(64 << 10, std::min<uint32_t>(m, 1 << 20));
    return true;
}

// a path of the program's -> the part below the game's folder ("FordRacing\data\x.ini"), with the current folder
// for a relative one; false: not in the game's folder
bool IsDevice(const std::wstring& p) {   // COM2, \.\COM2, NUL, CON...: a device, never a file of the game's
    if (p.rfind(L"\\\\.\\", 0) == 0) return true;
    std::wstring n = p;
    while (!n.empty() && (n.back() == L':' || n.back() == L' ')) n.pop_back();
    if (n.find_first_of(L"\\/") != std::wstring::npos) return false;
    for (auto& c : n) c = towupper(c);
    if (n == L"NUL" || n == L"CON" || n == L"AUX" || n == L"PRN" || n == L"CONIN$" || n == L"CONOUT$") return true;
    return n.size() >= 4 && n.size() <= 5 && (n.rfind(L"COM", 0) == 0 || n.rfind(L"LPT", 0) == 0) && iswdigit(n[3]);
}
bool Rel(const std::wstring& in, std::wstring& rel) {
    if (!g_on || in.empty() || IsDevice(in)) return false;
    std::wstring p = in;
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    size_t rl = wcslen(kRoot);
    bool abs = p.size() >= 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
    if (!abs) {
        if (g_cwd.empty()) return false;
        if (p[0] == L'\\') return false;   // the current drive's root: not ours
        p = g_cwd + L"\\" + p;
    }
    if (p.size() < rl || _wcsnicmp(p.c_str(), kRoot, rl) || (p.size() > rl && p[rl] != L'\\')) return false;
    // . and .. resolved
    std::vector<std::wstring> parts;
    for (size_t at = rl; at < p.size();) {
        while (at < p.size() && p[at] == L'\\') at++;
        size_t e = p.find(L'\\', at);
        if (e == std::wstring::npos) e = p.size();
        std::wstring part = p.substr(at, e - at);
        at = e;
        if (part.empty() || part == L".") continue;
        if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    rel.clear();
    for (auto& s : parts) rel += (rel.empty() ? L"" : L"\\") + s;
    return true;
}
std::string SmbPath(const std::wstring& rel) {
    std::string r = g_base;
    if (!rel.empty()) r += (r.empty() ? "" : "\\") + Narrow(rel);
    return r;
}
std::wstring Overlay(const std::wstring& rel) { return g_overlay + (rel.empty() ? L"" : L"\\" + rel); }
bool LocalExists(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA* ad = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    if (ad) *ad = d;
    return true;
}
void MakeDirs(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1))
        CreateDirectoryFromAppW(path.substr(0, at).c_str(), nullptr);
    CreateDirectoryFromAppW(path.c_str(), nullptr);
}
FILETIME Ft(uint64_t unixSeconds) {
    uint64_t t = (unixSeconds + 11644473600ull) * 10000000ull;
    return FILETIME{DWORD(t), DWORD(t >> 32)};
}
bool NetStat(const std::wstring& rel, Stat64& st) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (!Connect()) return false;
    return g_api.stat(g_ctx, SmbPath(rel).c_str(), &st) >= 0;
}

// --- open files on the share ---------------------------------------------------------------------------------------
struct NetFile {
    smb2fh* fh = nullptr;
    std::wstring rel;
    uint64_t size = 0, pos = 0, mtime = 0;
    std::vector<uint8_t> cache;   // the piece read last
    uint64_t cacheAt = 0;
    int refs = 1;
};
std::map<uintptr_t, NetFile*> g_files;
uintptr_t g_nextHandle = 0x6E000000;   // below 4 GB (32-bit programs' handles too), not a real handle's range
struct NetFind {
    std::vector<WIN32_FIND_DATAW> items;
    size_t next = 0;
};
std::map<uintptr_t, NetFind*> g_finds;
struct NetMap {
    NetFile* f;
};
std::map<uintptr_t, NetMap> g_maps;
std::map<void*, size_t> g_views;

HANDLE NewHandle() {
    g_nextHandle += 4;
    return (HANDLE)g_nextHandle;
}
NetFile* FileOf(HANDLE h) {
    auto it = g_files.find((uintptr_t)h);
    return it == g_files.end() ? nullptr : it->second;
}
// reads from the share through a piece cache: many small reads cost one network round trip per piece
size_t NetRead(NetFile* f, uint64_t at, uint8_t* out, size_t n) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    size_t done = 0;
    while (done < n && at + done < f->size) {
        uint64_t p = at + done;
        if (!f->cache.empty() && p >= f->cacheAt && p < f->cacheAt + f->cache.size()) {
            size_t k = std::min<size_t>(n - done, size_t(f->cacheAt + f->cache.size() - p));
            memcpy(out + done, f->cache.data() + (p - f->cacheAt), k);
            done += k;
            continue;
        }
        if (!Connect()) break;
        size_t want = n - done;
        if (want >= g_maxRead) {   // big reads: straight into the caller's memory
            uint32_t k = uint32_t(std::min<uint64_t>(std::min<size_t>(want, g_maxRead), f->size - p));
            int got = g_api.pread(g_ctx, f->fh, out + done, k, p);
            if (got <= 0) break;
            done += size_t(got);
            continue;
        }
        uint32_t k = uint32_t(std::min<uint64_t>(256u << 10, f->size - p));
        f->cache.resize(k);
        int got = g_api.pread(g_ctx, f->fh, f->cache.data(), k, p);
        if (got <= 0) {
            f->cache.clear();
            break;
        }
        f->cache.resize(size_t(got));
        f->cacheAt = p;
    }
    return done;
}

bool Match(const wchar_t* pat, const wchar_t* s) {   // * and ?, any case
    for (; *pat; pat++, s++) {
        if (*pat == L'*') {
            while (*pat == L'*') pat++;
            if (!*pat) return true;
            for (; *s; s++)
                if (Match(pat, s)) return true;
            return false;
        }
        if (!*s) return false;
        if (*pat != L'?' && towlower(*pat) != towlower(*s)) return false;
    }
    return !*s || (pat[-1] == L'.' && !*s);
}

// --- the stand-ins ---------------------------------------------------------------------------------------------------
typedef HANDLE(WINAPI* cfw_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
cfw_t RealCreateFileW() {   // the board stand-ins' (a JVS port) when there are some, else the system's
    static cfw_t f = [] {
        void* io = FindIoShim("kernel32.dll", "CreateFileW");
        return io ? (cfw_t)io : (cfw_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW");
    }();
    return f;
}
}  // namespace

bool NetFsOn() { return g_on; }
bool NetFsHandle(void* h) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    return FileOf((HANDLE)h) != nullptr;
}
void NetFsAddRef(void* h) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (NetFile* f = FileOf((HANDLE)h)) f->refs++;
}
uint64_t NetFsSize(void* h) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf((HANDLE)h);
    return f ? f->size : 0;
}
uint64_t NetFsPos(void* h) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf((HANDLE)h);
    return f ? f->pos : 0;
}
size_t NetFsPread(void* h, uint64_t at, void* out, size_t n) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf((HANDLE)h);
    return f ? NetRead(f, at, (uint8_t*)out, n) : 0;
}

// a path of the program's: on the share (the overlay first)? Its whole contents (loading the program, its DLLs)
bool NetFsReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    std::wstring rel;
    if (!Rel(path, rel)) return false;
    std::wstring ov = Overlay(rel);
    if (LocalExists(ov)) return false;   // (the caller reads the local file)
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (!Connect()) return false;
    Stat64 st;
    if (g_api.stat(g_ctx, SmbPath(rel).c_str(), &st) < 0 || st.type != 0) return false;
    smb2fh* fh = g_api.open(g_ctx, SmbPath(rel).c_str(), 0);
    if (!fh) return false;
    NetFile f;
    f.fh = fh;
    f.size = st.size;
    out.resize(size_t(st.size));
    size_t got = NetRead(&f, 0, out.data(), out.size());
    g_api.close(g_ctx, fh);
    return got == out.size();
}
// the local file a path stands for when the game wrote it (the overlay), else the path as it is
std::wstring NetFsLocal(const std::wstring& path) {
    std::wstring rel;
    if (!Rel(path, rel)) return path;
    std::wstring ov = Overlay(rel);
    return LocalExists(ov) ? ov : path;
}

static HANDLE WINAPI N_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    std::wstring rel;
    if (!name || !Rel(name, rel)) return RealCreateFileW()(name, acc, share, sa, disp, fl, t);
    if (!(acc & GENERIC_WRITE) && disp == OPEN_EXISTING) {
        std::wstring patched = PatchedFileFor(name);
        if (!patched.empty()) return RealCreateFileW()(patched.c_str(), acc, share, nullptr, disp, fl, nullptr);
    }
    std::wstring ov = Overlay(rel);
    bool write = (acc & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0 || disp == CREATE_ALWAYS || disp == CREATE_NEW ||
                 disp == TRUNCATE_EXISTING;
    if (write || LocalExists(ov)) {   // written by the game: its own copy (made from the share's the first time)
        if (write && !LocalExists(ov)) {
            MakeDirs(ov.substr(0, ov.find_last_of(L'\\')));
            std::vector<uint8_t> data;
            if (disp != CREATE_ALWAYS && disp != TRUNCATE_EXISTING && NetFsReadAll(name, data)) {
                HANDLE o = CreateFile2FromAppW(ov.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
                DWORD w = 0;
                if (o != INVALID_HANDLE_VALUE) WriteFile(o, data.data(), DWORD(data.size()), &w, nullptr), CloseHandle(o);
            }
        }
        return RealCreateFileW()(ov.c_str(), acc, share, nullptr, disp, fl, nullptr);
    }
    std::lock_guard<std::recursive_mutex> l(g_lock);
    Stat64 st;
    if (!Connect() || g_api.stat(g_ctx, SmbPath(rel).c_str(), &st) < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (st.type == 1) {   // a folder (FILE_FLAG_BACKUP_SEMANTICS): a handle with no data
        auto* f = new NetFile;
        f->rel = rel;
        f->mtime = st.mtime;
        HANDLE h = NewHandle();
        g_files[(uintptr_t)h] = f;
        return h;
    }
    smb2fh* fh = g_api.open(g_ctx, SmbPath(rel).c_str(), 0);
    if (!fh) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    auto* f = new NetFile;
    f->fh = fh;
    f->rel = rel;
    f->size = st.size;
    f->mtime = st.mtime;
    HANDLE h = NewHandle();
    g_files[(uintptr_t)h] = f;
    static int told = 0;
    if (told++ < 30) Log("NAS: %ls (%llu KB)", rel.c_str(), (unsigned long long)(st.size >> 10));
    return h;
}
static HANDLE WINAPI N_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    return N_CreateFileW(name ? FromAnsi(name).c_str() : nullptr, acc, share, sa, disp, fl, t);
}
HANDLE NetFsCreateFile(const wchar_t* name, DWORD acc, DWORD share, DWORD disp, DWORD fl, bool* handled) {
    std::wstring rel;
    *handled = name && Rel(name, rel);
    return *handled ? N_CreateFileW(name, acc, share, nullptr, disp, fl, nullptr) : INVALID_HANDLE_VALUE;
}

static BOOL WINAPI N_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    std::unique_lock<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf(h);
    if (!f) {
        l.unlock();
        typedef BOOL(WINAPI * rf_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
        static rf_t next = [] {
            void* io = FindIoShim("kernel32.dll", "ReadFile");
            return io ? (rf_t)io : (rf_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "ReadFile");
        }();
        return next(h, b, n, done, ov);
    }
    uint64_t at = ov ? (uint64_t(ov->OffsetHigh) << 32 | ov->Offset) : f->pos;
    size_t got = at < f->size ? NetRead(f, at, (uint8_t*)b, n) : 0;
    if (!ov) f->pos = at + got;
    if (done) *done = DWORD(got);
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = got;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    return TRUE;
}
bool NetFsReadFile(void* h, void* b, unsigned long n, unsigned long* done, void* ov) {
    if (!NetFsHandle(h)) return false;
    N_ReadFile((HANDLE)h, b, n, done, (LPOVERLAPPED)ov);
    return true;
}

namespace {
// the function each name stands for, looked up once BY NAME (it was kept per argument list: CloseHandle, FindClose and
// FlushFileBuffers, all BOOL(HANDLE), shared the first one looked up: a game's CloseHandle could run FindClose)
static void* RealFn(const char* name) {
    static std::mutex lock;
    static std::map<std::string, void*> fns;
    std::lock_guard<std::mutex> l(lock);
    auto it = fns.find(name);
    if (it != fns.end()) return it->second;
    // (ReadFile, CloseHandle: the board stand-ins' when the game has a JVS port, they pass the rest on)
    void* f = FindIoShim("kernel32.dll", name);
    if (!f) f = (void*)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), name);
    if (!f) f = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    if (!f) Log("NAS: Windows has no %s here", name);
    return fns[name] = f;
}
template <typename R, typename... A> R Real(const char* name, A... a) {
    auto f = (R(WINAPI*)(A...))RealFn(name);
    if (!f) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return R{};
    }
    return f(a...);
}
BOOL WINAPI N_CloseHandle(HANDLE h) {
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        if (NetFile* f = FileOf(h)) {
            if (--f->refs <= 0) {
                if (f->fh && g_ctx) g_api.close(g_ctx, f->fh);
                delete f;
                g_files.erase((uintptr_t)h);
            }
            return TRUE;
        }
        if (g_maps.erase((uintptr_t)h)) return TRUE;
    }
    return Real<BOOL>("CloseHandle", h);
}
BOOL WINAPI N_SetFilePointerEx(HANDLE h, LARGE_INTEGER d, PLARGE_INTEGER out, DWORD how) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf(h);
    if (!f) return Real<BOOL>("SetFilePointerEx", h, d, out, how);
    int64_t base = how == FILE_BEGIN ? 0 : how == FILE_CURRENT ? int64_t(f->pos) : int64_t(f->size);
    int64_t p = base + d.QuadPart;
    if (p < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return FALSE;
    }
    f->pos = uint64_t(p);
    if (out) out->QuadPart = p;
    return TRUE;
}
DWORD WINAPI N_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD how) {
    if (!NetFsHandle(h)) return Real<DWORD>("SetFilePointer", h, lo, hi, how);
    LARGE_INTEGER d, o;
    d.QuadPart = hi ? (int64_t(*hi) << 32 | uint32_t(lo)) : int64_t(lo);
    if (!N_SetFilePointerEx(h, d, &o, how)) return INVALID_SET_FILE_POINTER;
    if (hi) *hi = LONG(o.QuadPart >> 32);
    return DWORD(o.QuadPart);
}
BOOL WINAPI N_GetFileSizeEx(HANDLE h, PLARGE_INTEGER s) {
    if (!NetFsHandle(h)) return Real<BOOL>("GetFileSizeEx", h, s);
    s->QuadPart = int64_t(NetFsSize(h));
    return TRUE;
}
DWORD WINAPI N_GetFileSize(HANDLE h, LPDWORD hi) {
    if (!NetFsHandle(h)) return Real<DWORD>("GetFileSize", h, hi);
    uint64_t s = NetFsSize(h);
    if (hi) *hi = DWORD(s >> 32);
    return DWORD(s);
}
DWORD WINAPI N_GetFileType(HANDLE h) { return NetFsHandle(h) ? FILE_TYPE_DISK : Real<DWORD>("GetFileType", h); }
BOOL WINAPI N_GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf(h);
    if (!f) return Real<BOOL>("GetFileTime", h, c, a, w);
    FILETIME t = Ft(f->mtime);
    if (c) *c = t;
    if (a) *a = t;
    if (w) *w = t;
    return TRUE;
}
BOOL WINAPI N_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION i) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf(h);
    if (!f) return Real<BOOL>("GetFileInformationByHandle", h, i);
    memset(i, 0, sizeof *i);
    i->dwFileAttributes = f->fh ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_DIRECTORY;
    i->ftCreationTime = i->ftLastAccessTime = i->ftLastWriteTime = Ft(f->mtime);
    i->nFileSizeHigh = DWORD(f->size >> 32);
    i->nFileSizeLow = DWORD(f->size);
    i->nNumberOfLinks = 1;
    i->nFileIndexLow = DWORD((uintptr_t)h);
    return TRUE;
}
BOOL WINAPI N_FlushFileBuffers(HANDLE h) { return NetFsHandle(h) ? TRUE : Real<BOOL>("FlushFileBuffers", h); }

bool AttrOf(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& d, bool& ours) {
    std::wstring rel;
    ours = Rel(path, rel);
    if (!ours) return false;
    if (LocalExists(Overlay(rel), &d)) return true;
    Stat64 st;
    if (!NetStat(rel, st)) return false;
    memset(&d, 0, sizeof d);
    d.dwFileAttributes = st.type == 1 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    d.ftCreationTime = d.ftLastAccessTime = d.ftLastWriteTime = Ft(st.mtime);
    d.nFileSizeHigh = DWORD(st.size >> 32);
    d.nFileSizeLow = DWORD(st.size);
    return true;
}
DWORD WINAPI N_GetFileAttributesW(LPCWSTR p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    bool ours;
    if (p && AttrOf(p, d, ours)) return d.dwFileAttributes;
    if (ours) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return Real<DWORD>("GetFileAttributesW", p);
}
DWORD WINAPI N_GetFileAttributesA(LPCSTR p) { return N_GetFileAttributesW(p ? FromAnsi(p).c_str() : nullptr); }
BOOL WINAPI N_GetFileAttributesExW(LPCWSTR p, GET_FILEEX_INFO_LEVELS lvl, LPVOID out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    bool ours;
    if (p && AttrOf(p, d, ours)) {
        memcpy(out, &d, sizeof d);
        return TRUE;
    }
    if (ours) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return Real<BOOL>("GetFileAttributesExW", p, lvl, out);
}
BOOL WINAPI N_GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS lvl, LPVOID out) {
    return N_GetFileAttributesExW(p ? FromAnsi(p).c_str() : nullptr, lvl, out);
}

// folder listings: the share's, then the overlay's files not on it
HANDLE FindStart(const std::wstring& pattern, WIN32_FIND_DATAW* out, bool* ours) {
    std::wstring rel;
    *ours = Rel(pattern, rel);
    if (!*ours) return INVALID_HANDLE_VALUE;
    size_t slash = rel.find_last_of(L'\\');
    std::wstring dirRel = slash == std::wstring::npos ? L"" : rel.substr(0, slash);
    std::wstring spec = slash == std::wstring::npos ? rel : rel.substr(slash + 1);
    if (spec.empty()) spec = L"*";
    auto* nf = new NetFind;
    std::map<std::wstring, bool> seen;
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        if (Connect())
            if (smb2dir* d = g_api.opendir(g_ctx, SmbPath(dirRel).c_str())) {
                for (DirEnt* e; (e = g_api.readdir(g_ctx, d));) {
                    std::wstring n = Widen(e->name);
                    if (n == L"." || n == L".." || !Match(spec.c_str(), n.c_str())) continue;
                    WIN32_FIND_DATAW fd{};
                    fd.dwFileAttributes = e->st.type == 1 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
                    fd.ftCreationTime = fd.ftLastAccessTime = fd.ftLastWriteTime = Ft(e->st.mtime);
                    fd.nFileSizeHigh = DWORD(e->st.size >> 32);
                    fd.nFileSizeLow = DWORD(e->st.size);
                    wcsncpy_s(fd.cFileName, n.c_str(), _TRUNCATE);
                    seen[n] = true;
                    nf->items.push_back(fd);
                }
                g_api.closedir(g_ctx, d);
            }
    }
    WIN32_FIND_DATAW fd;
    HANDLE lf = FindFirstFileExFromAppW((Overlay(dirRel) + L"\\" + spec).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (lf != INVALID_HANDLE_VALUE) {
        do {
            if (!seen.count(fd.cFileName) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) nf->items.push_back(fd);
        } while (FindNextFileW(lf, &fd));
        FindClose(lf);
    }
    if (nf->items.empty()) {
        delete nf;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    *out = nf->items[0];
    nf->next = 1;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    HANDLE h = NewHandle();
    g_finds[(uintptr_t)h] = nf;
    return h;
}
void ToA(const WIN32_FIND_DATAW& w, WIN32_FIND_DATAA* a) {
    memcpy(a, &w, offsetof(WIN32_FIND_DATAW, cFileName));
    WideCharToMultiByte(CP_ACP, 0, w.cFileName, -1, a->cFileName, MAX_PATH, nullptr, nullptr);
    a->cAlternateFileName[0] = 0;
}
HANDLE WINAPI N_FindFirstFileW(LPCWSTR p, LPWIN32_FIND_DATAW out) {
    bool ours;
    HANDLE h = FindStart(p ? p : L"", out, &ours);
    return ours ? h : Real<HANDLE>("FindFirstFileW", p, out);
}
HANDLE WINAPI N_FindFirstFileA(LPCSTR p, LPWIN32_FIND_DATAA out) {
    bool ours;
    WIN32_FIND_DATAW w;
    HANDLE h = FindStart(FromAnsi(p), &w, &ours);
    if (!ours) return Real<HANDLE>("FindFirstFileA", p, out);
    if (h != INVALID_HANDLE_VALUE) ToA(w, out);
    return h;
}
HANDLE WINAPI N_FindFirstFileExW(LPCWSTR p, FINDEX_INFO_LEVELS lv, LPVOID out, FINDEX_SEARCH_OPS op, LPVOID f, DWORD fl) {
    bool ours;
    HANDLE h = FindStart(p ? p : L"", (WIN32_FIND_DATAW*)out, &ours);
    return ours ? h : Real<HANDLE>("FindFirstFileExW", p, lv, out, op, f, fl);
}
HANDLE WINAPI N_FindFirstFileExA(LPCSTR p, FINDEX_INFO_LEVELS lv, LPVOID out, FINDEX_SEARCH_OPS op, LPVOID f, DWORD fl) {
    bool ours;
    WIN32_FIND_DATAW w;
    HANDLE h = FindStart(FromAnsi(p), &w, &ours);
    if (!ours) return Real<HANDLE>("FindFirstFileExA", p, lv, out, op, f, fl);
    if (h != INVALID_HANDLE_VALUE) ToA(w, (WIN32_FIND_DATAA*)out);
    return h;
}
NetFind* FindOf(HANDLE h) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_finds.find((uintptr_t)h);
    return it == g_finds.end() ? nullptr : it->second;
}
BOOL WINAPI N_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW out) {
    NetFind* f = FindOf(h);
    if (!f) return Real<BOOL>("FindNextFileW", h, out);
    if (f->next >= f->items.size()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    *out = f->items[f->next++];
    return TRUE;
}
BOOL WINAPI N_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA out) {
    NetFind* f = FindOf(h);
    if (!f) return Real<BOOL>("FindNextFileA", h, out);
    if (f->next >= f->items.size()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    ToA(f->items[f->next++], out);
    return TRUE;
}
BOOL WINAPI N_FindClose(HANDLE h) {
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_finds.find((uintptr_t)h);
        if (it != g_finds.end()) {
            delete it->second;
            g_finds.erase(it);
            return TRUE;
        }
    }
    return Real<BOOL>("FindClose", h);
}

// the current folder: in the game's folder it is ours (the system cannot go there)
DWORD WINAPI N_GetCurrentDirectoryW(DWORD n, LPWSTR out) {
    if (g_cwd.empty()) return Real<DWORD>("GetCurrentDirectoryW", n, out);
    if (n <= g_cwd.size()) return DWORD(g_cwd.size() + 1);
    wcscpy_s(out, n, g_cwd.c_str());
    return DWORD(g_cwd.size());
}
DWORD WINAPI N_GetCurrentDirectoryA(DWORD n, LPSTR out) {
    if (g_cwd.empty()) return Real<DWORD>("GetCurrentDirectoryA", n, out);
    std::string a = Narrow(g_cwd);
    if (n <= a.size()) return DWORD(a.size() + 1);
    strcpy_s(out, n, a.c_str());
    return DWORD(a.size());
}
BOOL WINAPI N_SetCurrentDirectoryW(LPCWSTR p) {
    std::wstring rel;
    if (p && Rel(p, rel)) {
        g_cwd = std::wstring(kRoot) + (rel.empty() ? L"" : L"\\" + rel);
        return TRUE;
    }
    if (Real<BOOL>("SetCurrentDirectoryW", p)) {
        g_cwd.clear();
        return TRUE;
    }
    return FALSE;
}
BOOL WINAPI N_SetCurrentDirectoryA(LPCSTR p) { return N_SetCurrentDirectoryW(p ? FromAnsi(p).c_str() : nullptr); }
DWORD WINAPI N_GetFullPathNameW(LPCWSTR p, DWORD n, LPWSTR out, LPWSTR* file) {
    std::wstring rel;
    if (!p || !Rel(p, rel)) return Real<DWORD>("GetFullPathNameW", p, n, out, file);
    std::wstring full = std::wstring(kRoot) + (rel.empty() ? L"" : L"\\" + rel);
    if (n <= full.size()) return DWORD(full.size() + 1);
    wcscpy_s(out, n, full.c_str());
    if (file) *file = out + full.find_last_of(L'\\') + 1;
    return DWORD(full.size());
}
DWORD WINAPI N_GetFullPathNameA(LPCSTR p, DWORD n, LPSTR out, LPSTR* file) {
    std::wstring rel;
    if (!p || !Rel(FromAnsi(p), rel)) return Real<DWORD>("GetFullPathNameA", p, n, out, file);
    std::string full = Narrow(std::wstring(kRoot) + (rel.empty() ? L"" : L"\\" + rel));
    if (n <= full.size()) return DWORD(full.size() + 1);
    strcpy_s(out, n, full.c_str());
    if (file) *file = out + full.find_last_of('\\') + 1;
    return DWORD(full.size());
}
BOOL WINAPI N_CreateDirectoryW(LPCWSTR p, LPSECURITY_ATTRIBUTES sa) {
    std::wstring rel;
    if (!p || !Rel(p, rel)) return Real<BOOL>("CreateDirectoryW", p, sa);
    MakeDirs(Overlay(rel));
    return TRUE;
}
BOOL WINAPI N_CreateDirectoryA(LPCSTR p, LPSECURITY_ATTRIBUTES sa) { return N_CreateDirectoryW(p ? FromAnsi(p).c_str() : nullptr, sa); }
BOOL WINAPI N_DeleteFileW(LPCWSTR p) {
    std::wstring rel;
    if (!p || !Rel(p, rel)) return Real<BOOL>("DeleteFileW", p);
    DeleteFileFromAppW(Overlay(rel).c_str());
    return TRUE;   // (the share's copy stays: the NAS is never changed)
}
BOOL WINAPI N_DeleteFileA(LPCSTR p) { return N_DeleteFileW(p ? FromAnsi(p).c_str() : nullptr); }

// one not on the share: Windows' own (an app has CreateFileMappingFromApp, not CreateFileMappingW/A: Battle Pod's PhysX
// asked for shared memory, INVALID_HANDLE_VALUE, and the lookup of CreateFileMappingA came back empty: a call to 0)
static HANDLE OwnMapping(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name) {
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    return CreateFileMappingFromApp(h, sa, prot, (ULONG64)hi << 32 | lo, name);
#else
    return CreateFileMappingW(h, sa, prot, hi, lo, name);
#endif
}
// a file mapping of a file on the share: its view is the file read into memory (read-only use)
HANDLE WINAPI N_CreateFileMappingW(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    NetFile* f = FileOf(h);
    if (!f) return OwnMapping(h, sa, prot, hi, lo, name);
    HANDLE m = NewHandle();
    g_maps[(uintptr_t)m] = NetMap{f};
    f->refs++;
    return m;
}
HANDLE WINAPI N_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) {
    if (!NetFsHandle(h)) return OwnMapping(h, sa, prot, hi, lo, name ? FromAnsi(name).c_str() : nullptr);
    return N_CreateFileMappingW(h, sa, prot, hi, lo, nullptr);
}
LPVOID WINAPI N_MapViewOfFile(HANDLE m, DWORD acc, DWORD hi, DWORD lo, SIZE_T n) {
    NetFile* f = nullptr;
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_maps.find((uintptr_t)m);
        if (it != g_maps.end()) f = it->second.f;
    }
    if (!f) return Real<LPVOID>("MapViewOfFile", m, acc, hi, lo, n);
    uint64_t at = uint64_t(hi) << 32 | lo;
    if (!n) n = SIZE_T(f->size - std::min(at, f->size));
    void* p = VirtualAlloc(nullptr, n ? n : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    NetRead(f, at, (uint8_t*)p, n);
    std::lock_guard<std::recursive_mutex> l(g_lock);
    g_views[p] = n;
    return p;
}
BOOL WINAPI N_UnmapViewOfFile(LPCVOID p) {
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        if (g_views.erase((void*)p)) {
            VirtualFree((void*)p, 0, MEM_RELEASE);
            return TRUE;
        }
    }
    return Real<BOOL>("UnmapViewOfFile", p);
}
BOOL WINAPI N_GetDiskFreeSpaceExW(LPCWSTR p, PULARGE_INTEGER a, PULARGE_INTEGER t, PULARGE_INTEGER f) {
    std::wstring rel;
    if (!p || !Rel(p, rel)) return Real<BOOL>("GetDiskFreeSpaceExW", p, a, t, f);
    for (PULARGE_INTEGER x : {a, t, f})
        if (x) x->QuadPart = 64ull << 30;
    return TRUE;
}

struct NShim {
    const char* name;
    void* fn;
};
const NShim kNet[] = {
    {"CreateFileW", (void*)N_CreateFileW}, {"CreateFileA", (void*)N_CreateFileA}, {"ReadFile", (void*)N_ReadFile},
    {"CloseHandle", (void*)N_CloseHandle}, {"SetFilePointer", (void*)N_SetFilePointer},
    {"SetFilePointerEx", (void*)N_SetFilePointerEx}, {"GetFileSize", (void*)N_GetFileSize},
    {"GetFileSizeEx", (void*)N_GetFileSizeEx}, {"GetFileType", (void*)N_GetFileType}, {"GetFileTime", (void*)N_GetFileTime},
    {"GetFileInformationByHandle", (void*)N_GetFileInformationByHandle}, {"FlushFileBuffers", (void*)N_FlushFileBuffers},
    {"GetFileAttributesW", (void*)N_GetFileAttributesW}, {"GetFileAttributesA", (void*)N_GetFileAttributesA},
    {"GetFileAttributesExW", (void*)N_GetFileAttributesExW}, {"GetFileAttributesExA", (void*)N_GetFileAttributesExA},
    {"FindFirstFileW", (void*)N_FindFirstFileW}, {"FindFirstFileA", (void*)N_FindFirstFileA},
    {"FindFirstFileExW", (void*)N_FindFirstFileExW}, {"FindFirstFileExA", (void*)N_FindFirstFileExA},
    {"FindNextFileW", (void*)N_FindNextFileW}, {"FindNextFileA", (void*)N_FindNextFileA}, {"FindClose", (void*)N_FindClose},
    {"GetCurrentDirectoryW", (void*)N_GetCurrentDirectoryW}, {"GetCurrentDirectoryA", (void*)N_GetCurrentDirectoryA},
    {"SetCurrentDirectoryW", (void*)N_SetCurrentDirectoryW}, {"SetCurrentDirectoryA", (void*)N_SetCurrentDirectoryA},
    {"GetFullPathNameW", (void*)N_GetFullPathNameW}, {"GetFullPathNameA", (void*)N_GetFullPathNameA},
    {"CreateDirectoryW", (void*)N_CreateDirectoryW}, {"CreateDirectoryA", (void*)N_CreateDirectoryA},
    {"DeleteFileW", (void*)N_DeleteFileW}, {"DeleteFileA", (void*)N_DeleteFileA},
    {"CreateFileMappingW", (void*)N_CreateFileMappingW}, {"CreateFileMappingA", (void*)N_CreateFileMappingA},
    {"MapViewOfFile", (void*)N_MapViewOfFile}, {"UnmapViewOfFile", (void*)N_UnmapViewOfFile},
    {"GetDiskFreeSpaceExW", (void*)N_GetDiskFreeSpaceExW},
};
}  // namespace

// the stand-ins above, for a program playing from the share (FindShim asks before the others)
void* FindNetFsShim(const char* dll, const char* func) {
    if (!g_on || !func) return nullptr;
    if (_stricmp(dll, "kernel32.dll") && _stricmp(dll, "kernelbase.dll") && _strnicmp(dll, "api-ms-win-core-", 16)) return nullptr;
    for (const NShim& s : kNet)
        if (!strcmp(s.name, func)) return s.fn;
    return nullptr;
}

// Game Vault's link: smb://server/share/<root>/.../program.exe, root (the game's folder, a smb:// URL too) and the
// sign-in file. The program's path as the game sees it (\\gvnas\game\...) in out, "" with *error set when not
bool NetFsMount(const std::wstring& exeUrl, const std::wstring& rootUrl, const std::wstring& credFile, const std::wstring& local,
                std::wstring& out, std::string& error) {
    auto parse = [](const std::wstring& url, std::string& server, std::string& share, std::string& path) {
        std::wstring u = url;
        if (_wcsnicmp(u.c_str(), L"smb://", 6)) return false;
        u = u.substr(6);
        for (auto& c : u)
            if (c == L'\\') c = L'/';
        size_t a = u.find(L'/');
        if (a == std::wstring::npos) return false;
        size_t b = u.find(L'/', a + 1);
        server = Narrow(u.substr(0, a));
        share = Narrow(b == std::wstring::npos ? u.substr(a + 1) : u.substr(a + 1, b - a - 1));
        std::wstring p = b == std::wstring::npos ? L"" : u.substr(b + 1);
        for (auto& c : p)
            if (c == L'/') c = L'\\';
        path = Narrow(p);
        return true;
    };
    std::string server, share, exePath, rserver, rshare, rootPath;
    if (!parse(exeUrl, server, share, exePath) || !parse(rootUrl.empty() ? exeUrl : rootUrl, rserver, rshare, rootPath))
        return error = "not a network path", false;
    if (rootUrl.empty()) rootPath = exePath.substr(0, exePath.find_last_of('\\') == std::string::npos ? 0 : exePath.find_last_of('\\'));
    if (_stricmp(server.c_str(), rserver.c_str()) || _stricmp(share.c_str(), rshare.c_str()) ||
        _strnicmp(exePath.c_str(), rootPath.c_str(), rootPath.size()))
        return error = "the program is not in the game's folder", false;
    HMODULE m = nullptr;
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    m = LoadPackagedLibrary(L"smb2.dll", 0);
#else
    m = LoadLibraryW(L"smb2.dll");
#endif
    if (!m) return error = "smb2.dll could not be loaded", false;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
        return fn != nullptr;
    };
    bool ok = get(g_api.init_context, "smb2_init_context") && get(g_api.destroy_context, "smb2_destroy_context") &&
              get(g_api.set_timeout, "smb2_set_timeout") && get(g_api.set_security_mode, "smb2_set_security_mode") &&
              get(g_api.set_domain, "smb2_set_domain") && get(g_api.set_password, "smb2_set_password") &&
              get(g_api.connect_share, "smb2_connect_share") && get(g_api.disconnect_share, "smb2_disconnect_share") &&
              get(g_api.open, "smb2_open") && get(g_api.close, "smb2_close") && get(g_api.pread, "smb2_pread") &&
              get(g_api.get_max_read_size, "smb2_get_max_read_size") && get(g_api.get_error, "smb2_get_error") &&
              get(g_api.opendir, "smb2_opendir") && get(g_api.readdir, "smb2_readdir") && get(g_api.closedir, "smb2_closedir") &&
              get(g_api.stat, "smb2_stat");
    if (!ok) return error = "smb2.dll is missing a function", false;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // the sign-in, read once and gone from the disk
    g_server = server, g_share = share, g_base = rootPath;
    if (!credFile.empty()) {
        std::string text;
        HANDLE f = CreateFile2FromAppW(credFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) text.append(buf, n);
            SetFilePointer(f, 0, nullptr, FILE_BEGIN);
            std::string blank(text.size(), ' ');
            DWORD w = 0;
            WriteFile(f, blank.data(), DWORD(blank.size()), &w, nullptr);
            CloseHandle(f);
        }
        DeleteFileFromAppW(credFile.c_str());
        size_t at = 0;
        while (at < text.size()) {
            size_t e = text.find('\n', at);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(at, e - at);
            at = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            std::vector<std::string> f4;
            size_t pos = 0;
            for (int i = 0; i < 3; i++) {
                size_t t = line.find('\t', pos);
                if (t == std::string::npos) break;
                f4.push_back(line.substr(pos, t - pos));
                pos = t + 1;
            }
            f4.push_back(line.substr(pos));
            if (f4.size() == 4 && !_stricmp(f4[0].c_str(), server.c_str()) && !_stricmp(f4[1].c_str(), share.c_str())) {
                std::string user = f4[2].empty() ? "Guest" : f4[2];
                size_t sl = user.find('\\');
                if (sl != std::string::npos) g_domain = user.substr(0, sl), user = user.substr(sl + 1);
                g_user = user;
                g_password = f4[3];
            }
            std::fill(line.begin(), line.end(), ' ');
        }
        std::fill(text.begin(), text.end(), ' ');
    }
    {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        if (!Connect()) return error = "the network place could not be reached", false;
    }
    std::string folder = rootPath.substr(rootPath.find_last_of('\\') == std::string::npos ? 0 : rootPath.find_last_of('\\') + 1);
    for (auto& c : folder)
        if (strchr("<>:\"/\\|?*", c)) c = '_';
    g_overlay = local + L"\\nas overlay\\" + Widen(folder.empty() ? share : folder);
    MakeDirs(g_overlay);
    g_on = true;
    out = std::wstring(kRoot) + L"\\" + Widen(exePath.substr(rootPath.size() + (rootPath.empty() ? 0 : 1)));
    g_cwd = out.substr(0, out.find_last_of(L'\\'));
    Log("NAS: %s\\%s\\%s as %ls (signed in as %s); what the game writes goes to %ls", server.c_str(), share.c_str(),
        rootPath.c_str(), kRoot, g_user.c_str(), g_overlay.c_str());
    return true;
}

}  // namespace arcade
