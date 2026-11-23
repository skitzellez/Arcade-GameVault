// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// iDmacDrv32.dll for NESiCAxLive / Taito Type X games (built 32-bit, loaded by the 32-bit bridge from the package's x86
// folder, before the game's own): the "Fast I/O" board. Dumps ship an empty stand-in that never fills in an answer
// (OpenParrot jumps over it); this one answers. Register reads come from gvio.dll (the loader's: the Xbox pad in the
// board's layout); the answers follow OpenParrot's FastIoEmu.cpp (teknogods/OpenParrot, GPL-3.0). All __cdecl, the game
// imports them by ordinal (1 Open, 2 Close, 5 RegisterRead, 6 RegisterWrite, ...).
#include <windows.h>

namespace {
typedef DWORD(WINAPI* fastio_t)(DWORD command);
fastio_t g_read = nullptr;
void Start() {
    static bool started = false;
    if (started) return;
    started = true;
    if (HMODULE m = LoadLibraryA("gvio.dll")) g_read = (fastio_t)GetProcAddress(m, "GvFastIoRead");
    OutputDebugStringA(g_read ? "iDmacDrv32: Fast I/O board -> the pad" : "iDmacDrv32: no pad (gvio.dll not there)");
}
}  // namespace

extern "C" {
DWORD __cdecl iDmacDrvOpen(int, DWORD* out, DWORD* flag) {
    Start();
    if (out) *out = 284;
    if (flag) *flag = 0;
    return 0;
}
DWORD __cdecl iDmacDrvClose(int, void*) { return 0; }
int __cdecl iDmacDrvDmaRead(int, void*, UINT_PTR, void*) { return 0; }
int __cdecl iDmacDrvDmaWrite(int, void*, UINT_PTR, void*) { return 0; }
int __cdecl iDmacDrvRegisterRead(int, DWORD command, DWORD* out, DWORD* result) {
    Start();
    if (out) *out = g_read ? g_read(command) : 0;
    if (result) *result = 0;
    return 0;
}
int __cdecl iDmacDrvRegisterWrite(int, DWORD command, int, DWORD* result) {
    if (!result) return 0;
    if (command <= 0x4100) {
        if (command == 0x4100 || command == 0x4000 || command == 0x4004) *result = (DWORD)-1;
        return 0;
    }
    if (command > 0x410C) {
        if (command == 0x4180 || command == 0x4184 || command == 0x4188 || command == 0x418C) *result = (DWORD)-1;
        return 0;
    }
    if (command != 0x4108) *result = (DWORD)-1;
    return 0;
}
int __cdecl iDmacDrvRegisterBufferRead(int, DWORD, void*, UINT_PTR, void*) { return 0; }
int __cdecl iDmacDrvRegisterBufferWrite(int, DWORD, void*, UINT_PTR, void*) { return 0; }
int __cdecl iDmacDrvMemoryRead(int, DWORD, void*, void*) { return 0; }
int __cdecl iDmacDrvMemoryWrite(int, DWORD, int, void*) { return 0; }
int __cdecl iDmacDrvMemoryReadExt(int, DWORD, int, void*, DWORD, void*) { return 0; }
int __cdecl iDmacDrvMemoryWriteExt(int, int, int, void*, size_t, void*) { return 0; }
int __cdecl iDmacDrvMemoryBufferRead(int, DWORD, void*, UINT_PTR, void*) { return 0; }
int __cdecl iDmacDrvMemoryBufferWrite(int, int, void*, UINT_PTR, void*) { return 0; }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, void*) { return TRUE; }
