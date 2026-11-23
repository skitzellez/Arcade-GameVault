# one-off edit: a sampler (LocalState\sample.txt) that logs every 32-bit thread's position every 5 s: where a game
# waits (Battle Gear 4's loading thread stalls with no call into the loader)
from pathlib import Path

p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\peload32.cpp")
s = p.read_text(encoding="utf-8")
code = r'''// --- the sampler (testing): each 32-bit thread's place every 5 s, with a few return addresses from its stack
static std::mutex g_threadsLock;
static std::vector<std::pair<DWORD, HANDLE>> g_threads32;
static void NoteThread32() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard<std::mutex> l(g_threadsLock);
    g_threads32.push_back({GetCurrentThreadId(), h});
}
bool g_sample32 = false;
static DWORD WINAPI Sampler32(void*) {
    for (int round = 0; round < 60; round++) {
        Sleep(5000);
        std::vector<std::pair<DWORD, HANDLE>> list;
        {
            std::lock_guard<std::mutex> l(g_threadsLock);
            list = g_threads32;
        }
        for (auto& [tid, h] : list) {
            if (SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &c)) {
                char more[160] = "";
                if (c.SegCs == 0x23) {
                    const uint32_t* sp = (const uint32_t*)(uintptr_t)(uint32_t)c.Rsp;
                    MEMORY_BASIC_INFORMATION mbi;
                    int n = 0;
                    if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
                        for (int i = 0; i < 64 && n < 6 && (const uint8_t*)(sp + i) < (const uint8_t*)mbi.BaseAddress + mbi.RegionSize; i++)
                            if (sp[i] >= 0x401000 && sp[i] < 0x1000000) n += sprintf(more + strlen(more), " %08x", sp[i]) ? 1 : 0;
                }
                Log("sample %d: thread %lu %s eip %08llx esp %08llx eax %08llx ecx %08llx |%s", round, tid,
                    c.SegCs == 0x23 ? "32-bit" : "64-bit", c.Rip, c.Rsp & 0xFFFFFFFF, c.Rax & 0xFFFFFFFF, c.Rcx & 0xFFFFFFFF, more);
            }
            ResumeThread(h);
        }
    }
    return 0;
}
void StartSampler32() {
    static bool started = false;
    if (started || !g_sample32) return;
    started = true;
    CloseHandle(CreateThread(nullptr, 0, Sampler32, nullptr, 0, nullptr));
    Log("sampler on (sample.txt): thread positions every 5 s");
}
'''
anchor = "static DWORD WINAPI Thread32Main(void* p) {"
assert s.count(anchor) == 1
s = s.replace(anchor, code + anchor)
a = '''    St()->teb32 = (uint64_t)teb;
    St()->stackTop = top;
    Log("32-bit thread %lu: thread block %p, state %p, stack top %08x", GetCurrentThreadId(), teb, St(), top);'''
assert s.count(a) == 1
s = s.replace(a, a + "\n    NoteThread32();")
a = '''    uint8_t* teb = MakeTeb32(stackTop, stackBottom);
    if (!teb) {
        Log("32-bit: no thread block");
        return -1;'''
assert s.count(a) == 1
s = s.replace(a, '''    NoteThread32();
    StartSampler32();
''' + a)
p.write_text(s, encoding="utf-8")

u = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\uwp_main.cpp")
t = u.read_text(encoding="utf-8")
a = '''        std::wstring pkg = Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();'''
assert t.count(a) == 1
t = t.replace(a, '''        {   // sample.txt in LocalState: the 32-bit threads' places every 5 s (where a game waits)
            HANDLE sm = CreateFile2FromAppW((local + L"\\sample.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            if (sm != INVALID_HANDLE_VALUE) {
                CloseHandle(sm);
                extern bool g_sample32;
                arcade::g_sample32 = true;
            }
        }
''' + a)
u.write_text(t, encoding="utf-8")
print("ok")
