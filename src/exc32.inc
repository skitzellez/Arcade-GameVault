// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
// Included into peload32.cpp. 32-BIT EXCEPTIONS raised by the program (RaiseException: C++ throw, the C runtime's
// own) are dispatched the way 32-bit Windows does it, in 32-bit code: the bridged RaiseException puts an exception
// record and context on the 32-bit stack and sends the program into a small 32-bit dispatcher (BuildDispatch32),
// which walks the handler chain from the thread block's offset 0 (FS:[0]; records { previous, handler }) and calls
// each handler with (record, frame, context, dispatcher context). A C++ catch unwinds (RtlUnwind, bridged: frames
// above the target get their handler called with the unwinding flag and are unlinked) and jumps to the code after
// the catch, leaving nothing behind on the 64-bit side. A handler answering "continue execution" goes back to the
// code after RaiseException; nobody handling it ends the program.
// Not yet: hardware faults in 32-bit code (an access violation there still ends the program).

static const uint32_t kChainEnd = 0xFFFFFFFF;
static uint32_t* Chain32() { return (uint32_t*)Teb32Of(); }   // thread block offset 0: ExceptionList

// a 32-bit EXCEPTION_RECORD and CONTEXT in a block on the 32-bit stack below 'esp'
struct Exc32Frame {
    uint32_t rec, ctx, dc;
};
static Exc32Frame BuildExc32(uint32_t at, uint32_t code, uint32_t flags, uint32_t nargs, const uint32_t* args, uint32_t esp,
                             uint32_t eip) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    memset(p, 0, 0x380);
    uint32_t* r = (uint32_t*)p;   // EXCEPTION_RECORD: code, flags, nested record, address, n, info[15]
    r[0] = code;
    r[1] = flags;
    r[3] = eip;
    r[4] = nargs > 15 ? 15 : nargs;
    for (uint32_t i = 0; i < r[4]; i++) r[5 + i] = args[i];
    uint8_t* c = p + 0x60;        // CONTEXT (x86): Ebp 0xB4, Eip 0xB8, SegCs 0xBC, EFlags 0xC0, Esp 0xC4, SegSs 0xC8
    *(uint32_t*)(c + 0x00) = 0x10007;
    *(uint32_t*)(c + 0xB8) = eip;
    *(uint32_t*)(c + 0xBC) = 0x23;
    *(uint32_t*)(c + 0xC4) = esp;
    *(uint32_t*)(c + 0xC8) = 0x2B;
    return {at, at + 0x60, at + 0x340};
}

// the dispatcher, 32-bit code; on entry [esp] = record, +4 context, +8 dispatcher context, +12 return eip,
// +16 return esp (where a "continue execution" goes back to)
static uint32_t BuildDispatch32(uint8_t* c) {
    size_t n = 0;
    auto e = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) c[n++] = x; };
    auto d = [&](uint32_t v) { memcpy(c + n, &v, 4); n += 4; };
    e({0x8B, 0x34, 0x24});                 // mov esi, [esp]      record
    e({0x8B, 0x7C, 0x24, 0x04});           // mov edi, [esp+4]    context
    e({0x8B, 0x6C, 0x24, 0x08});           // mov ebp, [esp+8]    dispatcher context
    e({0x65, 0x8B, 0x1D}); d(g_tebShift);  // mov ebx, gs:[shift] (the chain: thread block offset 0)
    size_t loop = n;
    e({0x83, 0xFB, 0xFF});                 // cmp ebx, -1
    e({0x74, 0x00}); size_t j1 = n - 1;    // je unhandled
    e({0x85, 0xDB});                       // test ebx, ebx
    e({0x74, 0x00}); size_t j2 = n - 1;    // je unhandled
    e({0x55, 0x57, 0x53, 0x56});           // push ebp, edi, ebx, esi  (dc, context, frame, record)
    e({0xFF, 0x53, 0x04});                 // call [ebx+4]  (the handler)
    e({0x83, 0xC4, 0x10});                 // add esp, 16
    e({0x85, 0xC0});                       // test eax, eax
    e({0x74, 0x00}); size_t j3 = n - 1;    // je continue (ExceptionContinueExecution)
    e({0x8B, 0x1B});                       // mov ebx, [ebx]  (next record)
    e({0xEB}); c[n] = (uint8_t)(int8_t)((int)loop - (int)(n + 1)); n++;   // jmp loop
    size_t cont = n;
    e({0x8B, 0x44, 0x24, 0x0C});           // mov eax, [esp+12]
    e({0x8B, 0x64, 0x24, 0x10});           // mov esp, [esp+16]
    e({0xFF, 0xE0});                       // jmp eax
    size_t unhandled = n;
    e({0x56});                             // push esi (record)
    e({0xB8}); d(g_unhandledThunk);        // mov eax, Unhandled32 thunk
    e({0xFF, 0xD0});                       // call eax (does not come back)
    c[j1] = (uint8_t)(unhandled - (j1 + 1));
    c[j2] = (uint8_t)(unhandled - (j2 + 1));
    c[j3] = (uint8_t)(cont - (j3 + 1));
    return (uint32_t)n;
}

static uint64_t B_Unhandled32(uint64_t rec) {
    uint32_t code = rec ? ((uint32_t*)(uintptr_t)rec)[0] : 0;
    Log("32-bit exception 0x%08x: no handler took it, the program ends", code);
    if (code == 0xE06D7363 && rec) {   // a C++ throw: its type (and an exception's text, when it has one)
        const uint32_t* r = (const uint32_t*)(uintptr_t)rec;   // EXCEPTION_RECORD32: info[0] magic, [1] object, [2] ThrowInfo
        uint32_t obj = r[5 + 1], ti = r[5 + 2];
        if (r[4] != 3 && r[6] == 3) {   // (a 64-bit record: parameters at +32, 8 bytes each)
            const uint64_t* q = (const uint64_t*)(uintptr_t)(rec + 32);
            obj = (uint32_t)q[1], ti = (uint32_t)q[2];
        }
        Log("  record: params %u / %u, object %08x, throw info %08x", r[4], r[6], obj, ti);
        __try {
            uint32_t cta = ti ? ((const uint32_t*)(uintptr_t)ti)[3] : 0;            // ThrowInfo.pCatchableTypeArray
            uint32_t ct = cta ? ((const uint32_t*)(uintptr_t)cta)[1] : 0;           // first CatchableType
            uint32_t td = ct ? ((const uint32_t*)(uintptr_t)ct)[1] : 0;             // its TypeDescriptor
            const char* name = td ? (const char*)(uintptr_t)(td + 8) : "?";
            Log("  C++ type %s%s", name, !strcmp(name, ".I") || !strcmp(name, ".H") ? " (a number)" : "");
            if (obj && (!strcmp(name, ".I") || !strcmp(name, ".H") || !strcmp(name, ".K") || !strcmp(name, ".J")))
                Log("  value: %u (0x%08x)", *(const uint32_t*)(uintptr_t)obj, *(const uint32_t*)(uintptr_t)obj);
            if (obj) {
                uint32_t what = ((const uint32_t*)(uintptr_t)obj)[1];   // std::exception: (vtable, _What)
                if (what > 0x10000 && what < 0x80000000) Log("  text: %.200s", (const char*)(uintptr_t)what);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("  (its details could not be read)");
        }
    }
    St()->exiting = 1;
    St()->exitCode = (int)code;
    return 0;
}

static uint64_t B_RaiseException(uint64_t code, uint64_t flags, uint64_t nargs, uint64_t args) {
    State* st = St();
    uint32_t esp = st->esp32, eip = ((uint32_t*)(uintptr_t)esp)[0];
    uint32_t back = esp + 4 + 16;   // the caller's stack after RaiseException (stdcall, 4 arguments)
    uint32_t a[15] = {};
    for (uint32_t i = 0; i < (uint32_t)nargs && i < 15; i++) a[i] = ((uint32_t*)(uintptr_t)args)[i];
    uint32_t block = (esp - 0x400) & ~15u;
    Exc32Frame f = BuildExc32(block, (uint32_t)code, (uint32_t)flags, (uint32_t)nargs, a, esp, eip);
    uint32_t* top = (uint32_t*)(uintptr_t)(block - 32);   // the dispatcher's arguments
    top[0] = f.rec, top[1] = f.ctx, top[2] = f.dc, top[3] = eip, top[4] = back;
    st->redirectEip = g_dispatch32;
    st->redirectEsp = block - 32;
    return 0;
}

static uint64_t B_RtlUnwind(uint64_t target, uint64_t, uint64_t rec, uint64_t) {
    State* st = St();
    uint32_t esp = st->esp32, eip = ((uint32_t*)(uintptr_t)esp)[0];
    uint32_t block = (esp - 0x400) & ~15u;
    Exc32Frame f = BuildExc32(block, 0xC0000027 /*STATUS_UNWIND*/, 0, 0, nullptr, esp, eip);
    uint32_t r32 = rec ? (uint32_t)rec : f.rec;
    uint32_t* r = (uint32_t*)(uintptr_t)r32;
    r[1] |= target ? 0x2 : 0x6;   // EXCEPTION_UNWINDING (| EXIT_UNWIND without a target)
    uint32_t frame = *Chain32();
    while (frame && frame != kChainEnd && frame != (uint32_t)target) {
        uint32_t handler = ((uint32_t*)(uintptr_t)frame)[1];
        uint32_t args4[4] = {r32, frame, f.ctx, f.dc};
        Call32(handler, args4, 4);   // unwind handlers come back
        frame = ((uint32_t*)(uintptr_t)frame)[0];
        *Chain32() = frame;          // unlinked
    }
    r[1] &= ~0x6u;
    return 0;
}
