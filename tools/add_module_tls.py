# one-off edit: thread-local data for 64-bit modules we map (TlsAlloc slot per module, gs:[0x58] reads redirected)
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "peload.cpp"
s = p.read_text(encoding="utf-8")


def rep(a, b):
    global s
    assert a in s, a[:70]
    s = s.replace(a, b, 1)


tls_code = r'''// --- THREAD-LOCAL DATA (__declspec(thread) / thread_local) of a module we map. Windows finds a module's block at
// TEB.ThreadLocalStoragePointer[module's TLS index] (code: mov reg, gs:[0x58]; mov reg, [reg + index*8]); that
// array belongs to Windows' own loader and has no room for our modules. So each module of ours with thread data
// gets a TlsAlloc slot, its "gs:[0x58]" reads are pointed at the slot (TEB.TlsSlots[slot], gs:[0x1480 + slot*8]),
// and its index is 0: the slot holds a one-entry array with the thread's block. A thread that has no block yet reads
// address 0 there; ModuleTlsFault makes the block on the spot and the read is done again.
struct TlsModule {
    uint8_t *lo, *hi;
    DWORD slot;
    std::vector<uint8_t> tmpl;   // the initial data (copied before the module's code runs)
    size_t zeroFill;
};
static std::vector<TlsModule*> g_tlsModules;
static SRWLOCK g_tlsLock = SRWLOCK_INIT;
static void* ThreadTlsFor(const TlsModule& t) {
    size_t size = t.tmpl.size() + t.zeroFill;
    auto block = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 64);
    auto arr = (void**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
    if (!block || !arr) return nullptr;
    memcpy(block, t.tmpl.data(), t.tmpl.size());
    arr[0] = block;
    TlsSetValue(t.slot, arr);
    return arr;
}
static LONG CALLBACK ModuleTlsFault(PEXCEPTION_POINTERS e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* rip = (uint8_t*)e->ContextRecord->Rip;
    TlsModule* t = nullptr;
    AcquireSRWLockShared(&g_tlsLock);
    for (TlsModule* m : g_tlsModules)
        if (rip >= m->lo && rip < m->hi) t = m;
    ReleaseSRWLockShared(&g_tlsLock);
    if (!t || TlsGetValue(t->slot)) return EXCEPTION_CONTINUE_SEARCH;
    // the read: [REX] 8B modrm (not a register operand) [sib]; the register holding the empty slot is its base
    uint8_t* p = rip;
    uint8_t rex = 0;
    if ((*p & 0xF0) == 0x40) rex = *p++;
    if (*p++ != 0x8B) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t modrm = *p++;
    if ((modrm & 0xC0) == 0xC0) return EXCEPTION_CONTINUE_SEARCH;
    int base = (modrm & 7) == 4 ? (*p & 7) : (modrm & 7);
    base |= (rex & 1) << 3;
    DWORD64* regs = &e->ContextRecord->Rax;   // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15, in encoding order
    if (regs[base] != 0) return EXCEPTION_CONTINUE_SEARCH;
    void* arr = ThreadTlsFor(*t);
    if (!arr) return EXCEPTION_CONTINUE_SEARCH;
    regs[base] = (DWORD64)arr;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static bool IsModuleTlsFault(PEXCEPTION_POINTERS e) {   // (the crash log stays quiet about these)
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return false;
    uint8_t* rip = (uint8_t*)e->ContextRecord->Rip;
    bool in = false;
    AcquireSRWLockShared(&g_tlsLock);
    for (TlsModule* m : g_tlsModules)
        if (rip >= m->lo && rip < m->hi && !TlsGetValue(m->slot)) in = true;
    ReleaseSRWLockShared(&g_tlsLock);
    return in;
}
static void SetupModuleTls(uint8_t* base, IMAGE_NT_HEADERS64* nt) {
    const auto& td = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!td.Size) return;
    auto tls = (IMAGE_TLS_DIRECTORY64*)(base + td.VirtualAddress);
    if (tls->AddressOfIndex) *(DWORD*)tls->AddressOfIndex = 0;
    size_t raw = (size_t)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    if (!raw && !tls->SizeOfZeroFill) return;
    auto t = new TlsModule;
    t->lo = base;
    t->hi = base + nt->OptionalHeader.SizeOfImage;
    t->tmpl.assign((uint8_t*)tls->StartAddressOfRawData, (uint8_t*)tls->StartAddressOfRawData + raw);
    t->zeroFill = tls->SizeOfZeroFill;
    t->slot = TlsAlloc();
    if (t->slot == TLS_OUT_OF_INDEXES || t->slot >= 64) {
        Log("thread-local data: no TLS slot below 64 (got %lu)", t->slot);
        delete t;
        return;
    }
    // point every "mov reg, gs:[0x58]" in the code at the slot
    uint32_t disp = 0x1480 + t->slot * 8;
    int sites = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* c = base + sec[i].VirtualAddress;
        size_t n = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        for (size_t k = 0; k + 9 <= n; k++) {
            if (c[k] != 0x65 || (c[k + 1] & 0xF0) != 0x40 || c[k + 2] != 0x8B) continue;
            if ((c[k + 3] & 0xC7) != 0x04 || c[k + 4] != 0x25) continue;
            if (c[k + 5] != 0x58 || c[k + 6] || c[k + 7] || c[k + 8]) continue;
            memcpy(c + k + 5, &disp, 4);
            sites++;
        }
    }
    AcquireSRWLockExclusive(&g_tlsLock);
    g_tlsModules.push_back(t);
    ReleaseSRWLockExclusive(&g_tlsLock);
    static bool handler = false;
    if (!handler) {   // not declared for apps, but kernelbase has it
        typedef PVOID(WINAPI * add_t)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        auto add = (add_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "AddVectoredExceptionHandler");
        handler = add && add(1, ModuleTlsFault) != nullptr;
    }
    ThreadTlsFor(*t);   // this thread's block now (the module's DllMain / entry point runs here)
    Log("thread-local data: %zu bytes, slot %lu, %d code sites", raw + (size_t)tls->SizeOfZeroFill, t->slot, sites);
}

bool MapProgram(const std::vector<uint8_t>& file, LoadedImage& out) {'''

rep('bool MapProgram(const std::vector<uint8_t>& file, LoadedImage& out) {', tls_code)
rep('    // section protections\n', '    SetupModuleTls(base, nt);\n    // section protections\n')
rep('''    if (tls->AddressOfIndex) *(DWORD*)tls->AddressOfIndex = 0;
    if (!tls->AddressOfCallBacks) return;''', '''    if (!tls->AddressOfCallBacks) return;''')
rep('''            auto tls = (IMAGE_TLS_DIRECTORY64*)d->img.tls;
            if (tls->EndAddressOfRawData > tls->StartAddressOfRawData || tls->SizeOfZeroFill)
                Log("%s has thread-local data (%llu bytes): not set up yet", n.c_str(),
                    (unsigned long long)(tls->EndAddressOfRawData - tls->StartAddressOfRawData + tls->SizeOfZeroFill));
            RunTlsCallbacks''', '''            RunTlsCallbacks''')
rep('''return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* at''', '''return EXCEPTION_CONTINUE_SEARCH;
    if (IsModuleTlsFault(e)) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* at''')
p.write_text(s, encoding="utf-8")
print("ok")
