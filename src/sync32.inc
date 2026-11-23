// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
// Included into peload32.cpp. Windows structures whose size differs in a 32-bit program (pointers are 4 bytes there):
// the 32-bit structure keeps, in its first 4 bytes, a pointer (below 4 GB) to the real 64-bit one, made on first use.

static void* Real64(volatile uint32_t* slot, size_t size, void (*init)(void*)) {
    uint32_t v = *slot;
    if (v) return (void*)(uintptr_t)v;
    void* p = HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, size);
    if (init) init(p);
    uint32_t mine = (uint32_t)(uintptr_t)p;
    uint32_t was = (uint32_t)InterlockedCompareExchange((volatile LONG*)slot, (LONG)mine, 0);
    if (was) {   // another thread made it first
        HeapFree(LowHeap(), 0, p);
        return (void*)(uintptr_t)was;
    }
    return p;
}
// CRITICAL_SECTION: 24 bytes in 32-bit code; DebugInfo (the first field) holds our pointer
static void InitCs(void* p) { InitializeCriticalSection((CRITICAL_SECTION*)p); }
static CRITICAL_SECTION* Cs(uint64_t cs32) {
    return (CRITICAL_SECTION*)Real64((volatile uint32_t*)(uintptr_t)cs32, sizeof(CRITICAL_SECTION), InitCs);
}
static uint64_t B_InitializeCriticalSection(uint64_t cs) {
    memset((void*)(uintptr_t)cs, 0, 24);
    ((uint32_t*)(uintptr_t)cs)[1] = 0xFFFFFFFF;   // LockCount -1, as a real one reads
    Cs(cs);
    return 0;
}
static uint64_t B_InitializeCriticalSectionAndSpinCount(uint64_t cs, uint64_t) {
    B_InitializeCriticalSection(cs);
    return TRUE;
}
static uint64_t B_InitializeCriticalSectionEx(uint64_t cs, uint64_t, uint64_t) {
    B_InitializeCriticalSection(cs);
    return TRUE;
}
static uint64_t B_EnterCriticalSection(uint64_t cs) {
    EnterCriticalSection(Cs(cs));
    return 0;
}
static uint64_t B_LeaveCriticalSection(uint64_t cs) {
    LeaveCriticalSection(Cs(cs));
    return 0;
}
static uint64_t B_TryEnterCriticalSection(uint64_t cs) { return TryEnterCriticalSection(Cs(cs)); }
// kept, not freed: the C runtime leaves a lock after deleting it at exit, which Windows tolerates (a few bytes)
static uint64_t B_DeleteCriticalSection(uint64_t) { return 0; }
// SRWLOCK / CONDITION_VARIABLE: 4 bytes in 32-bit code (zero = not used yet, which also is their initial state)
static SRWLOCK* Srw(uint64_t p) { return (SRWLOCK*)Real64((volatile uint32_t*)(uintptr_t)p, sizeof(SRWLOCK), nullptr); }
static CONDITION_VARIABLE* Cv(uint64_t p) {
    return (CONDITION_VARIABLE*)Real64((volatile uint32_t*)(uintptr_t)p, sizeof(CONDITION_VARIABLE), nullptr);
}
static uint64_t B_InitializeSRWLock(uint64_t p) {
    *(uint32_t*)(uintptr_t)p = 0;
    return 0;
}
static uint64_t B_AcquireSRWLockExclusive(uint64_t p) {
    AcquireSRWLockExclusive(Srw(p));
    return 0;
}
static uint64_t B_AcquireSRWLockShared(uint64_t p) {
    AcquireSRWLockShared(Srw(p));
    return 0;
}
static uint64_t B_ReleaseSRWLockExclusive(uint64_t p) {
    ReleaseSRWLockExclusive(Srw(p));
    return 0;
}
static uint64_t B_ReleaseSRWLockShared(uint64_t p) {
    ReleaseSRWLockShared(Srw(p));
    return 0;
}
static uint64_t B_TryAcquireSRWLockExclusive(uint64_t p) { return TryAcquireSRWLockExclusive(Srw(p)); }
static uint64_t B_TryAcquireSRWLockShared(uint64_t p) { return TryAcquireSRWLockShared(Srw(p)); }
static uint64_t B_InitializeConditionVariable(uint64_t p) {
    *(uint32_t*)(uintptr_t)p = 0;
    return 0;
}
static uint64_t B_SleepConditionVariableCS(uint64_t cv, uint64_t cs, uint64_t ms) {
    return SleepConditionVariableCS(Cv(cv), Cs(cs), (DWORD)ms);
}
static uint64_t B_SleepConditionVariableSRW(uint64_t cv, uint64_t l, uint64_t ms, uint64_t f) {
    return SleepConditionVariableSRW(Cv(cv), Srw(l), (DWORD)ms, (ULONG)f);
}
static uint64_t B_WakeConditionVariable(uint64_t cv) {
    WakeConditionVariable(Cv(cv));
    return 0;
}
static uint64_t B_WakeAllConditionVariable(uint64_t cv) {
    WakeAllConditionVariable(Cv(cv));
    return 0;
}
// SLIST (8 bytes in 32-bit code: Next pointer, then depth and sequence): a plain list under one lock
static SRWLOCK g_slist = SRWLOCK_INIT;
static uint64_t B_InitializeSListHead(uint64_t h) {
    memset((void*)(uintptr_t)h, 0, 8);
    return 0;
}
static uint64_t B_InterlockedPushEntrySList(uint64_t h, uint64_t e) {
    AcquireSRWLockExclusive(&g_slist);
    uint32_t* head = (uint32_t*)(uintptr_t)h;
    uint32_t old = head[0];
    *(uint32_t*)(uintptr_t)e = old;
    head[0] = (uint32_t)e;
    ((uint16_t*)head)[2]++;   // depth
    ReleaseSRWLockExclusive(&g_slist);
    return old;
}
static uint64_t B_InterlockedPopEntrySList(uint64_t h) {
    AcquireSRWLockExclusive(&g_slist);
    uint32_t* head = (uint32_t*)(uintptr_t)h;
    uint32_t top = head[0];
    if (top) {
        head[0] = *(uint32_t*)(uintptr_t)top;
        ((uint16_t*)head)[2]--;
    }
    ReleaseSRWLockExclusive(&g_slist);
    return top;
}
static uint64_t B_InterlockedFlushSList(uint64_t h) {
    AcquireSRWLockExclusive(&g_slist);
    uint32_t* head = (uint32_t*)(uintptr_t)h;
    uint32_t top = head[0];
    head[0] = 0;
    ((uint16_t*)head)[2] = 0;
    ReleaseSRWLockExclusive(&g_slist);
    return top;
}
static uint64_t B_QueryDepthSList(uint64_t h) { return ((uint16_t*)(uintptr_t)h)[2]; }
// fiber storage: the clean-up callback is 32-bit code, which 64-bit Windows cannot call (dropped for now)
static uint64_t B_FlsAlloc(uint64_t) { return FlsAlloc(nullptr); }
// STARTUPINFO: 68 bytes in 32-bit code
static uint64_t B_GetStartupInfo(uint64_t si) {
    memset((void*)(uintptr_t)si, 0, 68);
    *(uint32_t*)(uintptr_t)si = 68;
    return 0;
}
// environment block: the 64-bit one may be above 4 GB; a copy below
static uint64_t B_GetEnvironmentStringsW() {
    wchar_t* e = GetEnvironmentStringsW();
    size_t n = 0;
    while (e[n] || e[n + 1]) n++;
    n += 2;
    wchar_t* low = (wchar_t*)HeapAlloc(LowHeap(), 0, n * 2);
    memcpy(low, e, n * 2);
    FreeEnvironmentStringsW(e);
    return (uint64_t)low;
}
static uint64_t B_GetEnvironmentStringsA() {   // the wide block, converted (embedded nulls included)
    wchar_t* e = GetEnvironmentStringsW();
    size_t n = 0;
    while (e[n] || e[n + 1]) n++;
    n += 2;
    int bytes = WideCharToMultiByte(CP_ACP, 0, e, (int)n, nullptr, 0, nullptr, nullptr);
    char* low = (char*)HeapAlloc(LowHeap(), 0, bytes);
    WideCharToMultiByte(CP_ACP, 0, e, (int)n, low, bytes, nullptr, nullptr);
    FreeEnvironmentStringsW(e);
    return (uint64_t)low;
}
static uint64_t B_FreeEnvironmentStrings(uint64_t p) {
    if (p) HeapFree(LowHeap(), 0, (void*)(uintptr_t)p);
    return TRUE;
}
// EncodePointer / DecodePointer: the 64-bit ones give 64-bit values, of which 32-bit code keeps half; a pointer below
// 4 GB stays as it is here (its own XOR with a constant, so a zero still encodes to non-zero as Windows does)
static uint64_t B_EncodePointer(uint64_t p) { return (uint32_t)p ^ 0x5A17C0DEu; }
static uint64_t B_DecodePointer(uint64_t p) { return (uint32_t)p ^ 0x5A17C0DEu; }
