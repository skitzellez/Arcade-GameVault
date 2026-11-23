// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
// msvcrt.dll for 32-bit games: the console has no 32-bit C runtime, and the DLLs TeknoParrot games bring (d3dx9_42,
// libavs ...) import Windows' old msvcrt.dll. Most of its functions are the static C runtime's own, exported under
// their names (msvcrt.def); the ones it no longer has (old data tables, lock helpers, exception filters) are here.
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern "C" {
// data the old runtime exported
int g_mb_cur_max = 1;            // __mb_cur_max
int g_adjust_fdiv = 0;           // _adjust_fdiv
char g_pioinfo[64][2048] = {};   // __pioinfo: the low-level file table (games only look at it through macros)
char g_badioinfo[64] = {};       // __badioinfo
char g_iob[20][32] = {};         // _iob: the old FILE array (stdin/stdout/stderr; nothing is printed on a console)

// the old runtime's locks (_lock / _unlock by number)
static CRITICAL_SECTION g_locks[64];
static LONG g_locksReady = 0;
static void LocksReady() {
    if (InterlockedCompareExchange(&g_locksReady, 1, 0) == 0) {
        for (auto& c : g_locks) InitializeCriticalSection(&c);
        InterlockedExchange(&g_locksReady, 2);
    }
    while (g_locksReady != 2) Sleep(0);
}
void __cdecl msvcrt_lock(int n) {
    LocksReady();
    EnterCriticalSection(&g_locks[n & 63]);
}
void __cdecl msvcrt_unlock(int n) {
    LocksReady();
    LeaveCriticalSection(&g_locks[n & 63]);
}
void __cdecl msvcrt_amsg_exit(int) { abort(); }
void __cdecl msvcrt_terminate() { abort(); }   // ?terminate@@YAXXZ
int __cdecl msvcrt_XcptFilter(unsigned long, struct _EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_SEARCH; }
// a DLL's atexit list (__dllonexit): kept in the DLL's own table, as the old runtime did
typedef int(__cdecl* onexit_t)(void);
onexit_t __cdecl msvcrt_dllonexit(onexit_t f, onexit_t** begin, onexit_t** end) {
    if (!begin || !end || !*begin) return f;
    size_t n = (size_t)(*end - *begin);
    onexit_t* list = (onexit_t*)realloc(*begin, (n + 1) * sizeof(onexit_t));
    if (!list) return nullptr;
    list[n] = f;
    *begin = list;
    *end = list + n + 1;
    return f;
}
char* __cdecl msvcrt_unDName(char* out, const char* name, int len, void*, void*, unsigned short) {   // no undecorating
    if (!out || len <= 0) return nullptr;
    strncpy(out, name ? name : "", len - 1);
    out[len - 1] = 0;
    return out;
}
// _ftol: the FPU's top value to a 32-bit integer, truncated (the newer runtime's _ftol2 does exactly this)
long __cdecl _ftol2(void);
__declspec(naked) void msvcrt_ftol() { __asm jmp _ftol2 }
// the old C++ frame handler name: the newer one handles the same frames
EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler3(void*, void*, void*, void*);
__declspec(naked) void msvcrt_CxxFrameHandler() { __asm jmp __CxxFrameHandler3 }
}

void __cdecl terminate() { abort(); }   // ?terminate@@YAXXZ (the old runtime exported it; the static one has none)

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
