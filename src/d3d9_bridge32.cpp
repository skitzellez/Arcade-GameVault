// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// DIRECT3D 9 FOR 32-BIT PROGRAMS. A 32-bit game's d3d9.dll!Direct3DCreate9 gets the real 64-bit one (DXVK, mapped
// by the loader) behind stand-ins: every D3D9 object it receives is a small object below 4 GB whose vtable holds
// 32-bit thunks (peload32.cpp BridgeFunction32), one per method. A call comes to Call32Method with the 32-bit
// stack arguments; they are turned into 64-bit ones by the method's argument codes (d3d9_sig32.inc, generated from
// the SDK's d3d9.h by tools/gen_d3d9_bridge.py) and the real method is called.
//   - objects in and out: stand-in <-> real object (one stand-in per real object, the most-derived interface)
//   - D3DPRESENT_PARAMETERS / D3DDEVICE_CREATION_PARAMETERS: other layout in 32-bit (an HWND is 4 bytes): converted
//   - locks (textures, surfaces, volumes, vertex/index buffers): DXVK's memory is above 4 GB, out of the game's reach,
//     so a lock gives a copy below 4 GB, written back at the unlock
// Not bridged: the D3D9Ex interfaces (Direct3DCreate9Ex answers "not available").

#include "peload.h"
#include <windows.h>
#include <cstring>
#include <map>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace arcade {

struct MethodSig32 {
    const char* name;
    char ret;           // 'h' number/HRESULT, 'f' float, 'v' void
    const char* codes;  // see tools/gen_d3d9_bridge.py
};
#include "d3d9_sig32.inc"
static const int kIfaces32 = sizeof(kMethods32) / sizeof(kMethods32[0]);

// peload32.cpp
uint32_t BridgeFunction32(void* target, int args, const std::string& name);
void* Alloc32(size_t size);
void Free32(void* p);

// --- calling a 64-bit method with any mix of integer and float arguments ----------------------------------------
// Windows x64 puts the first four arguments in rcx/rdx/r8/r9 or, for floats, xmm0-3: this stub loads both with the
// same 64 bits, so the method finds each argument where it expects it (floats sit in the low 32 bits).
typedef uint64_t (*CallAny_t)(void* fn, const uint64_t* args, uint64_t n);
static CallAny_t CallAny() {
    static CallAny_t stub = [] {
        static const uint8_t code[] = {
            0x53, 0x56, 0x57, 0x55,                    // push rbx, rsi, rdi, rbp
            0x48, 0x89, 0xE5,                          // mov rbp, rsp
            0x48, 0x89, 0xCB,                          // mov rbx, rcx  (fn)
            0x48, 0x89, 0xD6,                          // mov rsi, rdx  (args)
            0x4C, 0x89, 0xC7,                          // mov rdi, r8   (n)
            0x48, 0x8D, 0x04, 0xFD, 0x2F, 0, 0, 0,     // lea rax, [rdi*8 + 47]
            0x48, 0x83, 0xE0, 0xF0,                    // and rax, -16
            0x48, 0x29, 0xC4,                          // sub rsp, rax
            0x48, 0x83, 0xE4, 0xF0,                    // and rsp, -16
            0xB9, 4, 0, 0, 0,                          // mov ecx, 4
            // loop: cmp rcx, rdi ; jae done ; mov rax,[rsi+rcx*8] ; mov [rsp+rcx*8], rax ; inc rcx ; jmp loop
            0x48, 0x39, 0xF9, 0x73, 0x0D, 0x48, 0x8B, 0x04, 0xCE, 0x48, 0x89, 0x04, 0xCC, 0x48, 0xFF, 0xC1, 0xEB, 0xEE,
            0x48, 0x8B, 0x0E,                          // mov rcx, [rsi]
            0x48, 0x8B, 0x56, 0x08,                    // mov rdx, [rsi+8]
            0x4C, 0x8B, 0x46, 0x10,                    // mov r8, [rsi+16]
            0x4C, 0x8B, 0x4E, 0x18,                    // mov r9, [rsi+24]
            0x66, 0x48, 0x0F, 0x6E, 0xC1,              // movq xmm0, rcx
            0x66, 0x48, 0x0F, 0x6E, 0xCA,              // movq xmm1, rdx
            0x66, 0x49, 0x0F, 0x6E, 0xD0,              // movq xmm2, r8
            0x66, 0x49, 0x0F, 0x6E, 0xD9,              // movq xmm3, r9
            0xFF, 0xD3,                                // call rbx
            0x48, 0x89, 0xEC,                          // mov rsp, rbp
            0x5D, 0x5F, 0x5E, 0x5B,                    // pop rbp, rdi, rsi, rbx
            0xC3,                                      // ret
        };
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
        void* m = VirtualAllocFromApp(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        memcpy(m, code, sizeof code);
        ULONG old;
        VirtualProtectFromApp(m, 4096, PAGE_EXECUTE_READ, &old);
#else
        void* m = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        memcpy(m, code, sizeof code);
        DWORD old;
        VirtualProtect(m, 4096, PAGE_EXECUTE_READ, &old);
#endif
        return (CallAny_t)m;
    }();
    return stub;
}
static void* Slot(void* obj, int m) { return (*(void***)obj)[m]; }
static int MethodIndex(int k, const char* name) {
    for (int m = 0; m < kMethods32[k]; m++)
        if (!strcmp(kSig32[k][m].name, name)) return m;
    return -1;
}
template <typename R, typename... A> static R CallM(void* obj, int k, const char* name, A... a) {
    int m = MethodIndex(k, name);
    return ((R(STDMETHODCALLTYPE*)(void*, A...))Slot(obj, m))(obj, a...);
}

// --- stand-ins ---------------------------------------------------------------------------------------------------
struct Proxy {
    void* real;
    int iface;
};
static std::recursive_mutex g_lock;
static std::map<uint32_t, Proxy> g_byProxy;
static std::map<void*, uint32_t> g_byReal;
static uint32_t g_vtbl32[32];   // per interface: its 32-bit vtable (made on first use)

template <int K, int M> static uint64_t Handler(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                                uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t Call32Method(int k, int m, const uint64_t* a);
template <int K, int M>
static uint64_t Handler(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                        uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11) {
    const uint64_t a[12] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    return Call32Method(K, M, a);
}
typedef uint64_t (*Handler_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t);
template <int K, size_t... M> static void FillHandlers(Handler_t* out, std::index_sequence<M...>) {
    ((out[M] = &Handler<K, (int)M>), ...);
}
template <size_t... K> static void FillAll(Handler_t (*out)[128], std::index_sequence<K...>) {
    (FillHandlers<(int)K>(out[K], std::make_index_sequence<128>{}), ...);
}
static Handler_t g_handlers[17][128];

static int ArgCount(const char* codes) {
    int n = 0;
    for (const char* c = codes; *c; c++)
        if (*c != ' ' && (c == codes || c[-1] == ' ')) n++;
    return n;
}
static uint32_t Vtable32(int k) {
    if (g_vtbl32[k]) return g_vtbl32[k];
    static bool filled = false;
    if (!filled) {
        FillAll(g_handlers, std::make_index_sequence<17>{});
        filled = true;
    }
    int n = kMethods32[k];
    uint32_t* v = (uint32_t*)Alloc32(n * 4);
    for (int m = 0; m < n; m++)
        v[m] = BridgeFunction32((void*)g_handlers[k][m], 1 + ArgCount(kSig32[k][m].codes),
                                std::string("d3d9 ") + std::to_string(k) + "." + kSig32[k][m].name);
    g_vtbl32[k] = (uint32_t)(uintptr_t)v;
    return g_vtbl32[k];
}
// the most-derived interface of a resource (GetType), so later calls find their methods
static int Derived(void* real, int k) {
    if (k != kDirect3DResource9 && k != kDirect3DBaseTexture9) return k;
    switch (CallM<int>(real, kDirect3DResource9, "GetType")) {
    case 1: return kDirect3DSurface9;
    case 2: return kDirect3DVolume9;
    case 3: return kDirect3DTexture9;
    case 4: return kDirect3DVolumeTexture9;
    case 5: return kDirect3DCubeTexture9;
    case 6: return kDirect3DVertexBuffer9;
    case 7: return kDirect3DIndexBuffer9;
    }
    return k;
}
static uint32_t Wrap(void* real, int k) {
    if (!real) return 0;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_byReal.find(real);
    if (it != g_byReal.end()) return it->second;
    k = Derived(real, k);
    uint32_t* p = (uint32_t*)Alloc32(16);
    p[0] = Vtable32(k);
    uint32_t p32 = (uint32_t)(uintptr_t)p;
    g_byProxy[p32] = {real, k};
    g_byReal[real] = p32;
    return p32;
}
static void* Unwrap(uint64_t p32) {
    if (!(uint32_t)p32) return nullptr;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_byProxy.find((uint32_t)p32);
    return it == g_byProxy.end() ? nullptr : it->second.real;
}
// a texture's surfaces / volumes (GetSurfaceLevel ...): they go with it (DXVK frees them with their texture; a stale
// entry then matched a new object made at the same address: Battle Gear 4 crashed in SetRenderTarget)
static std::map<uint32_t, std::vector<uint32_t>> g_children;
static std::atomic<uint32_t> g_draws{0}, g_clears{0}, g_copies{0}, g_locks2{0};   // per 2 s, with the presents line
static void Forget(uint32_t p32) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto kids = g_children.find(p32);
    if (kids != g_children.end()) {
        std::vector<uint32_t> list = std::move(kids->second);
        g_children.erase(kids);
        for (uint32_t c : list) Forget(c);
    }
    auto it = g_byProxy.find(p32);
    if (it == g_byProxy.end()) return;
    // the handle stays the game's (never reused): games keep using objects the device still owns after their last
    // Release (Battle Gear 4: the back buffer, whose freed handle was handed out again for an index buffer). A new
    // object at the same address gets a new handle.
    auto r = g_byReal.find(it->second.real);
    if (r != g_byReal.end() && r->second == p32) g_byReal.erase(r);
}

// --- structures with another layout in 32-bit ---------------------------------------------------------------------
#pragma pack(push, 4)
struct PP32 {   // D3DPRESENT_PARAMETERS
    uint32_t w, h, format, count, ms, msq, swap, hwnd, windowed, autoDepth, depthFormat, flags, refresh, interval;
};
#pragma pack(pop)
struct PP64 {
    uint32_t w, h, format, count, ms, msq, swap;
    uint64_t hwnd;
    uint32_t windowed, autoDepth, depthFormat, flags, refresh, interval;
};
static PP64 To64(const PP32& a) {
    return {a.w, a.h, a.format, a.count, a.ms, a.msq, a.swap, (uint64_t)(int64_t)(int32_t)a.hwnd, a.windowed,
            a.autoDepth, a.depthFormat, a.flags, a.refresh, a.interval};
}
static void To32(const PP64& b, PP32& a) {
    a = {b.w, b.h, b.format, b.count, b.ms, b.msq, b.swap, (uint32_t)b.hwnd, b.windowed, b.autoDepth, b.depthFormat,
         b.flags, b.refresh, b.interval};
}
struct LockedRect64 {
    int32_t pitch;
    void* bits;
};
struct LockedBox64 {
    int32_t rowPitch, slicePitch;
    void* bits;
};

// --- locks: a copy below 4 GB ------------------------------------------------------------------------------------
struct LockCopy {
    void* real;
    uint8_t* low;
    size_t size;
    bool readOnly;
};
static std::map<std::pair<uint32_t, uint32_t>, LockCopy> g_locks;   // (stand-in, sub-resource)
static const uint32_t D3DLOCK_READONLY_ = 0x10, D3DLOCK_DISCARD_ = 0x2000;
// bytes per row of 'width' pixels (block formats: per row of 4x4 blocks), and whether rows are blocks
static uint32_t RowBytes(uint32_t format, uint32_t width, bool& blocks) {
    blocks = false;
    switch (format) {
    case 0x31545844:   // DXT1
        blocks = true;
        return ((width + 3) / 4) * 8;
    case 0x32545844: case 0x33545844: case 0x34545844: case 0x35545844:   // DXT2-5
        blocks = true;
        return ((width + 3) / 4) * 16;
    case 28: case 41: case 50: case 52: return width;                                   // A8, P8, L8, A4L4
    case 23: case 24: case 25: case 26: case 27: case 29: case 30: case 40: case 51: case 60: case 61: case 80:
    case 81: case 111: return width * 2;
    case 36: case 112: case 115: return width * 8;
    case 116: return width * 16;
    default: return width * 4;
    }
}
static void LockOpen(uint32_t p32, uint32_t sub, void* realBits, size_t size, uint32_t flags, void** out32Ptr) {
    uint8_t* low = (uint8_t*)Alloc32(size ? size : 1);
    if ((uintptr_t)low >= 0x80000000 || !low) Log("32-bit D3D9: lock copy not below 2 GB (%p)", low);
    if (!(flags & D3DLOCK_DISCARD_) && realBits) memcpy(low, realBits, size);
    std::lock_guard<std::recursive_mutex> l(g_lock);
    g_locks[{p32, sub}] = {realBits, low, size, (flags & D3DLOCK_READONLY_) != 0};
    *(uint32_t*)out32Ptr = (uint32_t)(uintptr_t)low;
    static int said = 0;
    if (said++ < 6) Log("32-bit D3D9: lock %u bytes, real %p, copy %p, flags 0x%x", (unsigned)size, realBits, low, flags);
}
static void LockClose(uint32_t p32, uint32_t sub) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_locks.find({p32, sub});
    if (it == g_locks.end()) return;
    if (!it->second.readOnly && it->second.real) memcpy(it->second.real, it->second.low, it->second.size);
    if (it->second.size >= (1u << 20)) {   // (testing) a big surface's picture: how much of it is not black
        static int told = 0;
        if (told < 30) {
            const uint32_t* px = (const uint32_t*)it->second.low;
            size_t n = it->second.size / 4, lit = 0, step = n / 997 + 1;
            for (size_t i = 0; i < n; i += step) lit += (px[i] & 0xFFFFFF) != 0;
            static size_t lastLit = (size_t)-1;
            if (lit != lastLit) {
                told++;
                Log("32-bit D3D9: unlock 0x%08x (%zu bytes): %zu of %zu samples lit", p32, it->second.size, lit, n / step);
                lastLit = lit;
            }
        }
    }
    Free32(it->second.low);
    g_locks.erase(it);
}
// the size of a locked rectangle's memory: rows of 'pitch', the last one only as long as the rectangle
static size_t RectBytes(const uint32_t* desc /*D3DSURFACE_DESC*/, const RECT* r, int32_t pitch) {
    uint32_t format = desc[0], w = desc[6], h = desc[7];
    if (r) {
        w = r->right - r->left;
        h = r->bottom - r->top;
    }
    bool blocks;
    uint32_t row = RowBytes(format, w, blocks);
    uint32_t rows = blocks ? (h + 3) / 4 : h;
    if (!rows) return 0;
    return (size_t)(rows - 1) * pitch + (row < (uint32_t)pitch ? row : (uint32_t)pitch);
}

// --- (testing) frame 300 of a 32-bit game saved to LocalState\frame32.bmp, read back before it is presented --------
static void SaveFrame32(void* dev) {
    void* back = nullptr;
    HRESULT bh = CallM<HRESULT>(dev, kDirect3DDevice9, "GetBackBuffer", (UINT)0, (UINT)0, (UINT)0, &back);
    if (bh < 0 || !back) {
        Log("32-bit D3D9: no back buffer to save (0x%08lx)", (unsigned long)bh);
        return;
    }
    uint32_t desc[8] = {};
    CallM<HRESULT>(back, kDirect3DSurface9, "GetDesc", (void*)desc);
    void* copy = nullptr;
    HRESULT hr = CallM<HRESULT>(dev, kDirect3DDevice9, "CreateOffscreenPlainSurface", (UINT)desc[6], (UINT)desc[7],
                                (UINT)desc[0], (UINT)2 /*SYSTEMMEM*/, &copy, (void*)nullptr);
    if (hr >= 0) hr = CallM<HRESULT>(dev, kDirect3DDevice9, "GetRenderTargetData", back, copy);
    LockedRect64 lr = {};
    if (hr >= 0) hr = CallM<HRESULT>(copy, kDirect3DSurface9, "LockRect", (void*)&lr, (void*)nullptr, (DWORD)0x10);
    if (hr >= 0) {
        wchar_t dir[MAX_PATH] = L".";
        GetEnvironmentVariableW(L"ARCADE_LOCAL", dir, MAX_PATH);
        std::wstring path = std::wstring(dir) + L"\\frame32.bmp";
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
        HANDLE f = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
#else
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
#endif
        if (f != INVALID_HANDLE_VALUE) {
            uint32_t w = desc[6], h = desc[7];
            uint8_t head[54] = {'B', 'M'};   // BMP file header (14) + info header (40), top-down 32-bit
            uint32_t size = 54 + w * h * 4, off = 54, info = 40, planesBits = 1 | (32u << 16);
            int32_t bw = (int32_t)w, bh = -(int32_t)h;
            memcpy(head + 2, &size, 4);
            memcpy(head + 10, &off, 4);
            memcpy(head + 14, &info, 4);
            memcpy(head + 18, &bw, 4);
            memcpy(head + 22, &bh, 4);
            memcpy(head + 26, &planesBits, 4);
            DWORD n;
            WriteFile(f, head, sizeof head, &n, nullptr);
            for (uint32_t y = 0; y < h; y++) WriteFile(f, (uint8_t*)lr.bits + (size_t)y * lr.pitch, w * 4, &n, nullptr);
            CloseHandle(f);
            Log("32-bit D3D9: frame saved (%ux%u, format %u)", w, h, desc[0]);
        } else {
            Log("32-bit D3D9: frame file not made (%ls, error %lu)", path.c_str(), GetLastError());
        }
        CallM<HRESULT>(copy, kDirect3DSurface9, "UnlockRect");
    } else {
        Log("32-bit D3D9: frame not saved (0x%08lx)", (unsigned long)hr);
    }
    if (copy) CallM<ULONG>(copy, kDirect3DSurface9, "Release");
    CallM<ULONG>(back, kDirect3DSurface9, "Release");
}

// --- one call -------------------------------------------------------------------------------------------------------
static uint64_t Call32Method(int k, int m, const uint64_t* a) {
    uint32_t this32 = (uint32_t)a[0];
    void* real = Unwrap(this32);
    if (!real) return 0x80004003;   // E_POINTER
    const MethodSig32& sig = kSig32[k][m];
    // the argument codes
    char codes[24][4];
    int nc = 0;
    for (const char* c = sig.codes; *c && nc < 24;) {
        while (*c == ' ') c++;
        int j = 0;
        while (*c && *c != ' ' && j < 3) codes[nc][j++] = *c++;
        codes[nc][j] = 0;
        if (j) nc++;
    }
    uint64_t args[16] = {(uint64_t)real};
    uint64_t outs[16] = {};
    PP64 pp64[2];
    uint32_t cp64[6] = {};   // D3DDEVICE_CREATION_PARAMETERS (64-bit: hFocusWindow at +8)
    LockedRect64 lr64 = {};
    LockedBox64 lb64 = {};
    int npp = 0, ppOf[24] = {};
    for (int i = 0; i < nc; i++) {
        uint64_t v = a[1 + i];
        uint64_t& o = args[1 + i];
        switch (codes[i][0]) {
        case 'u': case 'f': o = (uint32_t)v; break;
        case 'p': o = (uint32_t)v; break;
        case 'h': o = v; break;                       // sign-extended by the bridge
        case 'I':
            o = (uint64_t)Unwrap(v);
            if ((uint32_t)v && !o) {   // an object the bridge never handed out (released, or not ours): no call
                static int told = 0;
                if (told++ < 10) Log("32-bit D3D9: %s with an unknown object 0x%08x: refused", sig.name, (uint32_t)v);
                return 0x8876086C;   // D3DERR_INVALIDCALL
            }
            break;
        case 'O': case 'Q': case 'D': o = (uint64_t)&outs[i]; break;
        case 'n':
            if ((uint32_t)v) *(uint32_t*)(uintptr_t)(uint32_t)v = 0;
            o = 0;
            break;
        case 'P':
            if ((uint32_t)v && npp < 2) {
                pp64[npp] = To64(*(PP32*)(uintptr_t)(uint32_t)v);
                ppOf[i] = npp;
                o = (uint64_t)&pp64[npp++];
            } else {
                o = 0;
            }
            break;
        case 'C': o = (uint64_t)cp64; break;
        case 'R': o = (uint64_t)&lr64; break;
        case 'B': o = (uint64_t)&lb64; break;
        case 'V': o = (uint64_t)&outs[i]; break;
        default: o = v;
        }
    }
    const char* name = sig.name;
    // a surface's GDI device context: answered here, the real surface never held in "DC" state (the console's GDI
    // cannot draw into it; DXVK's DC left dxwrapper's primary surface stuck: every later lock failed, nothing shown)
    if (k == kDirect3DSurface9 && !strcmp(name, "GetDC")) {
        static uint32_t next = 0x0A5C7000;
        if ((uint32_t)a[1]) *(uint32_t*)(uintptr_t)(uint32_t)a[1] = (next += 4);
        return 0;
    }
    if (k == kDirect3DSurface9 && !strcmp(name, "ReleaseDC")) return 0;
    if (!strncmp(name, "Draw", 4)) g_draws++;
    else if (!strcmp(name, "Clear")) g_clears++;
    else if (!strcmp(name, "UpdateSurface") || !strcmp(name, "StretchRect") || !strcmp(name, "UpdateTexture") ||
             !strcmp(name, "ColorFill"))
        g_copies++;
    else if (!strcmp(name, "LockRect"))
        g_locks2++;
    if (!strcmp(name, "SetRenderTarget")) {
        static int told = 0;
        if (told++ < 12) Log("32-bit D3D9: SetRenderTarget(%llu, 0x%08x -> %p, type %d)", (unsigned long long)args[1], (uint32_t)a[2],
                             (void*)args[2], args[2] ? CallM<int>((void*)args[2], kDirect3DResource9, "GetType") : -1);
        if (args[1] > 3) return 0x8876086C;                 // (4 targets at most)
        if (args[1] == 0 && args[2] == 0) return 0x8876086C;   // (render target 0 is required)
    }
    bool unlockRect = !strcmp(name, "UnlockRect"), unlockBox = !strcmp(name, "UnlockBox"),
         unlockBuf = !strcmp(name, "Unlock") && (k == kDirect3DVertexBuffer9 || k == kDirect3DIndexBuffer9);
    // before an unlock: the game's copy goes back to the real memory
    if (unlockRect || unlockBox) {
        uint32_t sub = k == kDirect3DTexture9 || k == kDirect3DVolumeTexture9 ? (uint32_t)a[1]
                       : k == kDirect3DCubeTexture9                         ? (uint32_t)a[1] * 64 + (uint32_t)a[2]
                                                                            : 0;
        LockClose(this32, sub);
    } else if (unlockBuf) {
        LockClose(this32, 0);
    }
    if (k == kDirect3DDevice9 && !strcmp(name, "Present")) {
        static int frames = 0;
        static DWORD last = GetTickCount(), lastFrames = 0;
        if (++frames == 300) {
            Log("32-bit D3D9: saving frame 300");
            SaveFrame32(real);
            Log("32-bit D3D9: saving frame 300 done");
        }
        if (GetTickCount() - last >= 2000) {   // (testing) frames per second, every 2 s
            Log("32-bit D3D9: %lu presents in %lu ms (total %d), %u draws, %u clears, %u copies, %u locks", frames - lastFrames,
                GetTickCount() - last, frames, g_draws.exchange(0), g_clears.exchange(0), g_copies.exchange(0), g_locks2.exchange(0));
            last = GetTickCount();
            lastFrames = frames;
        }
    }
    uint64_t r;
    if (sig.ret == 'f') {   // GetNPatchMode
        float f = ((float(STDMETHODCALLTYPE*)(void*))Slot(real, m))(real);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        r = bits;
    } else {
        r = CallAny()(Slot(real, m), args, 1 + nc < 4 ? 4 : 1 + nc);
    }
    bool ok = (int32_t)r >= 0;
    if (k == kDirect3DSurface9 && (!strcmp(name, "LockRect") || !strcmp(name, "UnlockRect") || !strcmp(name, "GetDC") ||
                                   !strcmp(name, "ReleaseDC"))) {   // (testing) the order of locks on surfaces
        static int n = 0;
        if (n++ < 40) Log("32-bit D3D9: surface 0x%08x %s -> 0x%08x", this32, name, (uint32_t)r);
    }
    if (!ok || !strcmp(name, "TestCooperativeLevel")) {   // (testing) failures, and the device's state
        static std::map<std::string, int> told;
        if (told[name]++ < 3) {
            Log("32-bit D3D9: %s -> 0x%08x (args %08x %08x %08x %08x %08x)", name, (uint32_t)r, (uint32_t)a[1], (uint32_t)a[2],
                (uint32_t)a[3], (uint32_t)a[4], (uint32_t)a[5]);
            if (k == kDirect3DSurface9) {
                uint32_t d[8] = {};   // D3DSURFACE_DESC: format, type, usage, pool, multisample, quality, width, height
                CallM<long>(real, kDirect3DSurface9, "GetDesc", (void*)d);
                Log("  the surface: %ux%u format %u, pool %u, usage 0x%x, type %u, multisample %u", d[6], d[7], d[0], d[3], d[2], d[1], d[4]);
            }
        }
    }
    // after the call: outputs back to the game
    for (int i = 0; i < nc; i++) {
        uint32_t p32 = (uint32_t)a[1 + i];
        switch (codes[i][0]) {
        case 'O':
            if (p32) {
                uint32_t child = ok ? Wrap((void*)outs[i], atoi(codes[i] + 1)) : 0;
                if (child && atoi(codes[i] + 1) == kDirect3DSurface9) {
                    static int told = 0;
                    if (told++ < 40) Log("32-bit D3D9: surface 0x%08x (%p) from %s", child, (void*)outs[i], name);
                }
                *(uint32_t*)(uintptr_t)p32 = child;
                if (child && (k == kDirect3DTexture9 || k == kDirect3DCubeTexture9 || k == kDirect3DVolumeTexture9)) {
                    std::lock_guard<std::recursive_mutex> l(g_lock);
                    auto& v = g_children[this32];
                    if (std::find(v.begin(), v.end(), child) == v.end()) v.push_back(child);
                }
            }
            break;
        case 'Q':
            if (p32) {
                uint32_t w = 0;
                if (ok && outs[i]) {
                    if ((void*)outs[i] == real) {
                        w = this32;
                    } else {
                        const GUID* iid = (const GUID*)(uintptr_t)(uint32_t)a[i];   // the IID before it
                        int want = kDirect3DResource9;
                        for (int j = 0; j < kIfaces32; j++)
                            if (iid && IsEqualGUID(*iid, kIid32[j])) want = j;
                        w = Wrap((void*)outs[i], want);
                    }
                }
                *(uint32_t*)(uintptr_t)p32 = w;
            }
            break;
        case 'D':
            if (p32) *(uint32_t*)(uintptr_t)p32 = (uint32_t)outs[i];
            break;
        case 'P':
            if (p32 && ok) To32(pp64[ppOf[i]], *(PP32*)(uintptr_t)p32);
            break;
        case 'C':
            if (p32 && ok) {
                uint32_t* c = (uint32_t*)(uintptr_t)p32;
                c[0] = cp64[0], c[1] = cp64[1], c[2] = cp64[2], c[3] = cp64[4];
            }
            break;
        case 'R':
            if (p32 && ok) {
                // the locked rectangle's size: sub-resource description, then a copy below 4 GB
                uint32_t desc[8] = {};
                uint32_t sub = 0;
                const RECT* rect = nullptr;
                if (k == kDirect3DTexture9) {
                    CallM<HRESULT>(real, k, "GetLevelDesc", (UINT)a[1], (void*)desc);
                    sub = (uint32_t)a[1];
                    rect = (const RECT*)(uintptr_t)(uint32_t)a[3];
                } else if (k == kDirect3DCubeTexture9) {
                    CallM<HRESULT>(real, k, "GetLevelDesc", (UINT)a[2], (void*)desc);
                    sub = (uint32_t)a[1] * 64 + (uint32_t)a[2];
                    rect = (const RECT*)(uintptr_t)(uint32_t)a[4];
                } else {
                    CallM<HRESULT>(real, k, "GetDesc", (void*)desc);
                    rect = (const RECT*)(uintptr_t)(uint32_t)a[2];
                }
                uint32_t flags = (uint32_t)a[nc];   // the last argument
                int32_t* out = (int32_t*)(uintptr_t)p32;
                out[0] = lr64.pitch;
                LockOpen(this32, sub, lr64.bits, RectBytes(desc, rect, lr64.pitch), flags, (void**)&out[1]);
            }
            break;
        case 'B':
            if (p32 && ok) {
                uint32_t desc[7] = {};   // D3DVOLUME_DESC: Format Type Usage Pool Width Height Depth
                uint32_t sub = 0;
                const uint32_t* box = nullptr;   // D3DBOX: Left Top Right Bottom Front Back
                if (k == kDirect3DVolumeTexture9) {
                    CallM<HRESULT>(real, k, "GetLevelDesc", (UINT)a[1], (void*)desc);
                    sub = (uint32_t)a[1];
                    box = (const uint32_t*)(uintptr_t)(uint32_t)a[3];
                } else {
                    CallM<HRESULT>(real, k, "GetDesc", (void*)desc);
                    box = (const uint32_t*)(uintptr_t)(uint32_t)a[2];
                }
                uint32_t h = box ? box[3] - box[1] : desc[5], d = box ? box[5] - box[4] : desc[6];
                bool blocks;
                RowBytes(desc[0], 1, blocks);
                if (blocks) h = (h + 3) / 4;
                size_t size = (size_t)(d ? d : 1) * lb64.slicePitch;
                (void)h;
                int32_t* out = (int32_t*)(uintptr_t)p32;
                out[0] = lb64.rowPitch;
                out[1] = lb64.slicePitch;
                LockOpen(this32, sub, lb64.bits, size, (uint32_t)a[nc], (void**)&out[2]);
            }
            break;
        case 'V':
            if (p32 && ok) {
                uint32_t desc[6] = {};   // VERTEXBUFFER_DESC: Format Type Usage Pool Size (FVF)
                CallM<HRESULT>(real, k, "GetDesc", (void*)desc);
                uint32_t offset = (uint32_t)a[1], size = (uint32_t)a[2];
                // to the buffer's end, whatever was asked: real Direct3D hands out a pointer into the whole buffer, and
                // games write past the piece they locked (Battle Fantasia's index buffer: a fault one page on)
                (void)size;
                size = desc[4] > offset ? desc[4] - offset : 0;
                LockOpen(this32, 0, (void*)outs[i], size, (uint32_t)a[4], (void**)(uintptr_t)p32);
            }
            break;
        }
    }
    if (!strcmp(name, "Release") && (uint32_t)r == 0) Forget(this32);
    return r;
}

// --- d3d9.dll for 32-bit programs ---------------------------------------------------------------------------------
static uint64_t B32_Direct3DCreate9(uint64_t sdk) {
    void* d = Dll64Load("d3d9.dll");
    void* fn = nullptr;
    if (!d || !Dll64Export(d, "Direct3DCreate9", &fn) || !fn) {
        Log("32-bit D3D9: the 64-bit d3d9.dll (DXVK) is not there");
        return 0;
    }
    void* real = ((void*(WINAPI*)(UINT))fn)((UINT)sdk);
    uint32_t p = Wrap(real, kDirect3D9);
    Log("32-bit D3D9: Direct3DCreate9(%u) -> DXVK through the bridge (%s)", (unsigned)sdk, p ? "ok" : "failed");
    return p;
}
static uint64_t B32_NotThere2(uint64_t, uint64_t) { return 0x8876086A; }   // D3DERR_NOTAVAILABLE
static uint64_t B32_Zero() { return 0; }
void* FindD3D9Bridge32(const char* dll, const char* func, int& args) {
    if (_stricmp(dll, "d3d9.dll")) return nullptr;
    if (!strcmp(func, "Direct3DCreate9")) return args = 1, (void*)B32_Direct3DCreate9;
    if (!strcmp(func, "Direct3DCreate9Ex")) return args = 2, (void*)B32_NotThere2;
    if (!strncmp(func, "D3DPERF_", 8)) {   // markers for profilers: nothing to do
        args = !strcmp(func, "D3DPERF_BeginEvent") || !strcmp(func, "D3DPERF_SetMarker") || !strcmp(func, "D3DPERF_SetRegion")
                   ? 2
               : !strcmp(func, "D3DPERF_SetOptions") ? 1
                                                     : 0;
        return (void*)B32_Zero;
    }
    return nullptr;
}

}  // namespace arcade
