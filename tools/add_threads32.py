# one-off edit: per-thread bridge state (in the 32-bit thread block's 2nd page, found through GS) + 32-bit CreateThread
from pathlib import Path
import re

p = Path(__file__).resolve().parent.parent / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")

# --- State: per thread, at low32(TEB64) + 0x1000 ---------------------------------------------------------
s = s.replace("""    uint64_t teb32;        // +0x20: the 32-bit thread block, set again in FS before each return to 32-bit code
};
static State* g_st = nullptr;
static uint32_t g_stackTop32 = 0;
static bool g_exiting = false;
static int g_exitCode = 0;""", """    uint64_t teb32;        // +0x20: the 32-bit thread block
    uint32_t exiting;      // +0x28: ExitProcess / ExitThread asked: the next return goes to g_exit32
    int32_t exitCode;      // +0x2C
    uint32_t stackTop;     // +0x30: this thread's 32-bit stack top
};
// each thread's own: the second page of its 32-bit thread block, which sits at the low 32 bits of its 64-bit TEB
// (MakeTeb32); the bridge code finds it the same way, through GS
static State* St() { return (State*)((uintptr_t)(uint32_t)(uintptr_t)NtCurrentTeb() + 0x1000); }""", 1)
s = s.replace("""static uint64_t B_ExitProcess(uint64_t code) {
    g_exiting = true;
    g_exitCode = (int)code;
    return code;
}""", """static uint64_t B_ExitProcess(uint64_t code) {
    St()->exiting = 1;
    St()->exitCode = (int)code;
    return code;
}
static uint64_t B_ExitThread(uint64_t code) { return B_ExitProcess(code); }   // this thread's 32-bit code ends""", 1)
s = s.replace("g_st->", "St()->")
s = s.replace("if (g_exiting) St()->resumeEip", "if (St()->exiting) St()->resumeEip")
s = s.replace("        g_st && St()->index < g_fn.size() ? g_fn[St()->index].name.c_str() : \"-\");",
              "        St()->index < g_fn.size() ? g_fn[St()->index].name.c_str() : \"-\");")

# --- switch code: state address from GS instead of a fixed address -------------------------------------
s = s.replace("    g_st = (State*)(g_arena + 0x1000);\n", "", 1)
s = s.replace("    uint64_t st = (uint64_t)g_st;\n", """    // this thread's State: low 32 bits of the 64-bit TEB (gs:[0x30]) + 0x1000
    auto stateToRax = [&] {
        e({0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00});   // mov rax, gs:[0x30]
        e({0x89, 0xC0});                                              // mov eax, eax
        e({0x48, 0x05, 0x00, 0x10, 0x00, 0x00});                      // add rax, 0x1000
    };
    auto stateToRcx = [&] {
        e({0x65, 0x48, 0x8B, 0x0C, 0x25, 0x30, 0x00, 0x00, 0x00});   // mov rcx, gs:[0x30]
        e({0x89, 0xC9});                                              // mov ecx, ecx
        e({0x48, 0x81, 0xC1, 0x00, 0x10, 0x00, 0x00});                // add rcx, 0x1000
    };
""", 1)
s = s.replace("    e({0x48, 0xB8}); q(st);                                                         // mov rax, &state\n",
              "    stateToRax();\n")
s = s.replace("    e({0x48, 0xB9}); q(st);                                                         // mov rcx, &state\n",
              "    stateToRcx();\n")
# in enter64 rcx holds entry32: stateToRax only uses rax (fine). In the bridge eax holds the index before stateToRcx.

# --- Call32 / Run32 ----------------------------------------------------------------------------------
s = s.replace("""    State keep = *g_st;
    bool keepExit = g_exiting;
    uint32_t esp = keep.esp32 ? (keep.esp32 - 256) & ~15u : g_stackTop32;""", """    State keep = *St();
    uint32_t esp = keep.esp32 ? (keep.esp32 - 256) & ~15u : keep.stackTop;""", 1)
s = s.replace("""    g_exiting = false;
    typedef uint32_t (*enter_t)(uint64_t entry, uint64_t esp);
    uint32_t r = ((enter_t)g_enter64)(func, esp);
    *g_st = keep;
    g_exiting = keepExit;
    return r;""", """    St()->exiting = 0;
    typedef uint32_t (*enter_t)(uint64_t entry, uint64_t esp);
    uint32_t r = ((enter_t)g_enter64)(func, esp);
    if (St()->exiting) r = (uint32_t)St()->exitCode;
    *St() = keep;
    return r;""", 1)
s = s.replace("    *St() = keep;\n    g_exiting = keepExit;\n", "    *St() = keep;\n")
s = s.replace("    g_stackTop32 = stackTop;\n", "", 1)
s = s.replace("    g_exiting = false;\n    St()->teb32 = (uint64_t)teb;", "    St()->teb32 = (uint64_t)teb;\n    St()->stackTop = stackTop;", 1)
s = s.replace("""    if (g_exiting) Log("32-bit program ended (exit code %d)", g_exitCode);
    else Log("32-bit entry point returned %d", result);
    return g_exiting ? g_exitCode : result;""", """    if (St()->exiting) Log("32-bit program ended (exit code %d)", St()->exitCode);
    else Log("32-bit entry point returned %d", result);
    return St()->exiting ? St()->exitCode : result;""", 1)

# --- 32-bit CreateThread -----------------------------------------------------------------------------
threads = '''// --- threads a 32-bit program starts: a real thread, its own 32-bit stack and thread block, the start function
// called through Call32 with the parameter
struct Start32 {
    uint32_t func, param;
};
static DWORD WINAPI Thread32Main(void* p) {
    Start32 st = *(Start32*)p;
    delete (Start32*)p;
    uint8_t* stack = AllocBelow4G(1 << 20, 0x60000000);
    uint32_t top = stack ? (uint32_t)(uintptr_t)(stack + (1 << 20) - 16) : 0;
    uint8_t* teb = stack ? MakeTeb32(top, (uint32_t)(uintptr_t)stack) : nullptr;
    if (!teb) {
        Log("32-bit thread: no stack or thread block below 4 GB, not started");
        return 0;
    }
    St()->teb32 = (uint64_t)teb;
    St()->stackTop = top;
    uint32_t r = Call32(st.func, &st.param, 1);
    VirtualFree(stack, 0, MEM_RELEASE);
    return r;
}
static uint64_t B_CreateThread(uint64_t, uint64_t, uint64_t start, uint64_t param, uint64_t flags, uint64_t tid32) {
    auto st = new Start32{(uint32_t)start, (uint32_t)param};
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, Thread32Main, st, (DWORD)flags & CREATE_SUSPENDED, &tid);
    if (!h) {
        delete st;
        return 0;
    }
    if (tid32) *(uint32_t*)(uintptr_t)tid32 = tid;
    Log("32-bit program starts a thread at 0x%08x (thread %lu)", (uint32_t)start, tid);
    return (uint64_t)h;
}

'''
if "Thread32Main" not in s:
    s = s.replace("static void StartDlls32() {", threads + "static void StartDlls32() {", 1)
    s = s.replace("static uint64_t B_GetModuleHandleA2(uint64_t name);",
                  "static uint64_t B_GetModuleHandleA2(uint64_t name);\nstatic uint64_t B_CreateThread(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);\nuint32_t Call32(uint32_t func, const uint32_t* args, int n);\nstatic uint8_t* MakeTeb32(uint32_t stackTop, uint32_t stackBottom);", 1)
    s = s.replace('    {"kernel32.dll", "ExitProcess", 1, false, (void*)B_ExitProcess},',
                  '    {"kernel32.dll", "ExitProcess", 1, false, (void*)B_ExitProcess},\n'
                  '    {"kernel32.dll", "ExitThread", 1, false, (void*)B_ExitThread},\n'
                  '    {"kernel32.dll", "CreateThread", 6, false, (void*)B_CreateThread},', 1)
    s = s.replace("static uint64_t B_ExitProcess(uint64_t code);   // ends the 32-bit program (below)",
                  "static uint64_t B_ExitProcess(uint64_t code);   // ends the 32-bit program (below)\nstatic uint64_t B_ExitThread(uint64_t code);", 1)
p.write_text(s, encoding="utf-8")
left = [m.start() for m in re.finditer(r"g_st\b|g_exiting|g_exitCode|g_stackTop32", s)]
print("ok; leftovers:", len(left), [s[max(0,i-40):i+40].replace("\\n"," ") for i in left[:4]])
