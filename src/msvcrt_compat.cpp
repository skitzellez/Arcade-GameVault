// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// Old msvcrt.dll functions that ucrtbase.dll (where msvcrt imports go) does not have: the DirectX June 2010 DLLs a
// game ships or needs (d3dx9_43, D3DCompiler_43, d3dx11_43, X3DAudio1_7, XAPOFX1_5, XAudio2_7) and dbghelp import
// them. Memory and C++ runtime pieces, printf forms, DLL start-up helpers, and three data exports.

#include "peload.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>

namespace arcade {

// operator new / delete (msvcrt's, x64 names)
static void* __cdecl M_new(size_t n) { return malloc(n ? n : 1); }
static void __cdecl M_delete(void* p) { free(p); }
static void __cdecl M_terminate() {
    Log("msvcrt: terminate() called");
    abort();
}
static void __cdecl M_typeinfo_dtor(void*) {}

// std::exception as msvcrt laid it out: { vtable, const char* what, bool owned }
struct OldException {
    void** vtbl;
    const char* what;
    bool owned;
};
static void* __cdecl E_delete(OldException* self, unsigned flags) {   // scalar deleting destructor
    if (flags & 1) free(self);
    return self;
}
static const char* __cdecl E_what(const OldException* self) { return self->what ? self->what : "unknown exception"; }
static void* g_exceptionVtbl[2] = {(void*)E_delete, (void*)E_what};
static OldException* __cdecl E_ctor_msg(OldException* self, const char* const* msg, int) {   // exception(const char* const&, int)
    self->vtbl = g_exceptionVtbl;
    self->what = msg ? *msg : nullptr;
    self->owned = false;
    return self;
}
static OldException* __cdecl E_ctor_copy(OldException* self, const OldException* other) {
    self->vtbl = g_exceptionVtbl;
    self->what = other ? other->what : nullptr;
    self->owned = false;
    return self;
}
static void __cdecl E_dtor(OldException*) {}

// DLL start-up and errors
static int __cdecl M_XcptFilter(unsigned long, void*) { return EXCEPTION_CONTINUE_SEARCH; }
typedef int(__cdecl* onexit_t)();
static onexit_t __cdecl M_dllonexit(onexit_t f, void*, void*) { return f; }   // (run at process end: never here)
static onexit_t __cdecl M_onexit(onexit_t f) { return f; }
static void __cdecl M_lock(int) {}
static void __cdecl M_unlock(int) {}
static void __cdecl M_amsg_exit(int code) {
    Log("msvcrt: _amsg_exit(%d)", code);
    abort();
}

// printf forms with msvcrt's own rules (-1 when the text does not fit)
static int __cdecl M_vsnprintf(char* b, size_t n, const char* f, va_list ap) { return _vsnprintf(b, n, f, ap); }
static int __cdecl M_snprintf(char* b, size_t n, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = _vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
static int __cdecl M_vsnwprintf(wchar_t* b, size_t n, const wchar_t* f, va_list ap) { return _vsnwprintf(b, n, f, ap); }
static int __cdecl M_sscanf(const char* s, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsscanf(s, f, ap);
    va_end(ap);
    return r;
}
static time_t __cdecl M_time(time_t* t) { return time(t); }
static char* __cdecl M_ctime(const time_t* t) { return ctime(t); }
static wchar_t* __cdecl M_wctime(const time_t* t) { return _wctime(t); }

// data: old stdio tables (only touched by debug output; zeroed) and the multibyte maximum
static unsigned char g_iob[3 * 48];
static void* g_pioinfo[64];
static unsigned char g_badioinfo[64];
static int g_mbCurMax = 1;

struct CompatShim {
    const char* name;
    void* fn;
};
static const CompatShim kMsvcrt[] = {
    {"??2@YAPEAX_K@Z", (void*)M_new}, {"??_U@YAPEAX_K@Z", (void*)M_new},
    {"??3@YAXPEAX@Z", (void*)M_delete}, {"??_V@YAXPEAX@Z", (void*)M_delete},
    {"?terminate@@YAXXZ", (void*)M_terminate}, {"??1type_info@@UEAA@XZ", (void*)M_typeinfo_dtor},
    {"??0exception@@QEAA@AEBQEBDH@Z", (void*)E_ctor_msg}, {"??0exception@@QEAA@AEBV0@@Z", (void*)E_ctor_copy},
    {"??1exception@@UEAA@XZ", (void*)E_dtor}, {"?what@exception@@UEBAPEBDXZ", (void*)E_what},
    {"_XcptFilter", (void*)M_XcptFilter}, {"__dllonexit", (void*)M_dllonexit}, {"_onexit", (void*)M_onexit},
    {"_lock", (void*)M_lock}, {"_unlock", (void*)M_unlock}, {"_amsg_exit", (void*)M_amsg_exit},
    {"_vsnprintf", (void*)M_vsnprintf}, {"_snprintf", (void*)M_snprintf}, {"_vsnwprintf", (void*)M_vsnwprintf},
    {"sscanf", (void*)M_sscanf}, {"time", (void*)M_time}, {"ctime", (void*)M_ctime}, {"_wctime", (void*)M_wctime},
    {"_iob", (void*)g_iob}, {"__pioinfo", (void*)g_pioinfo}, {"__badioinfo", (void*)g_badioinfo},
    {"__mb_cur_max", (void*)&g_mbCurMax},
};

void* FindMsvcrtShim(const char* dll, const char* func) {
    if (_stricmp(dll, "msvcrt.dll") || !func) return nullptr;
    if (!strcmp(func, "_setjmp")) {   // msvcrt's _setjmp(jmp_buf, frame) is ucrt's __intrinsic_setjmp (not wrapped)
        HMODULE u = GetModuleHandleW(L"ucrtbase.dll");
        return u ? (void*)GetProcAddress(u, "__intrinsic_setjmp") : nullptr;
    }
    for (const CompatShim& s : kMsvcrt)
        if (!strcmp(s.name, func)) return s.fn;
    return nullptr;
}

}  // namespace arcade
