# one-off edit: 32-bit thread blocks in one reserved area of ours, at low32(TEB64) + a fixed shift
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")

shift = '''// THE THREAD BLOCK AREA. In 32-bit mode a GS access lands at the low 32 bits of the thread's 64-bit TEB plus the
// address in the instruction (wrapping at 4 GB). Windows keeps memory of its own around there (thread stacks on the
// Xbox), so every thread block goes into one 64 MB area we reserve below 2 GB instead: the address in each rewritten
// instruction (FsToGs) and the bridge's lookup get g_tebShift added, chosen so the loader thread's block sits in the
// middle of the area. Threads' TEBs lie close together, so theirs land in the area too.
static uint8_t* g_tebArea = nullptr;
static uint32_t g_tebShift = 0;
static const size_t kTebArea = 64u << 20;
static bool TebAreaReady() {
    if (g_tebArea) return true;
    for (uintptr_t at = 0x08000000; at + kTebArea < 0x80000000 && !g_tebArea; at += 0x01000000)
        g_tebArea = (uint8_t*)VirtualAlloc((void*)at, kTebArea, MEM_RESERVE, PAGE_READWRITE);
    if (!g_tebArea) return false;
    uint32_t low = (uint32_t)(uintptr_t)NtCurrentTeb();
    g_tebShift = (uint32_t)((uintptr_t)g_tebArea + kTebArea / 2) - low;
    g_tebShift &= ~0xFFFu;   // whole pages
    Log("32-bit thread block area %p (+%zu MB), shift 0x%08x", g_tebArea, kTebArea >> 20, g_tebShift);
    return true;
}
static uint8_t* Teb32Of() { return (uint8_t*)(uintptr_t)(uint32_t)((uint32_t)(uintptr_t)NtCurrentTeb() + g_tebShift); }

'''
if "g_tebShift" not in s:
    s = s.replace("static State* St() { return (State*)((uintptr_t)(uint32_t)(uintptr_t)NtCurrentTeb() + 0x800); }",
                  shift + "static State* St() { return (State*)(Teb32Of() + 0x800); }", 1)
    # switch code: add the shift (32-bit add zero-extends) instead of "mov eax,eax" / "mov ecx,ecx"
    s = s.replace("        e({0x89, 0xC0});                                              // mov eax, eax\n",
                  "        e({0x05}); d(g_tebShift);                                    // add eax, shift (zero-extends rax)\n", 1)
    s = s.replace("        e({0x89, 0xC9});                                              // mov ecx, ecx\n",
                  "        e({0x81, 0xC1}); d(g_tebShift);                              // add ecx, shift (zero-extends rcx)\n", 1)
    s = s.replace("    g_arena = AllocBelow4G(0x200000);\n",
                  "    if (!TebAreaReady()) return;\n    g_arena = AllocBelow4G(0x200000);\n", 1)
    # rewrite: prefix and shifted address
    s = s.replace("""        if (disp >= 0x1000) continue;
        code[i] = 0x65;
        n++;""", """        if (disp >= 0x1000) continue;
        code[i] = 0x65;
        disp += g_tebShift;
        memcpy(code + i + (op >= 0xA0 && op <= 0xA3 ? 2 : 3), &disp, 4);
        n++;""", 1)
    # MakeTeb32: our area, a page per thread
    s = s.replace("    uint8_t* want = (uint8_t*)(uintptr_t)(uint32_t)(uintptr_t)NtCurrentTeb();",
                  "    uint8_t* want = Teb32Of();\n"
                  "    if (want < g_tebArea || want + 0x1000 > g_tebArea + kTebArea) {\n"
                  "        Log(\"32-bit: thread block %p outside the area (TEB %p)\", want, NtCurrentTeb());\n"
                  "        return nullptr;\n"
                  "    }\n"
                  "#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP\n"
                  "    uint8_t* t = (uint8_t*)VirtualAllocFromApp(want, 0x1000, MEM_COMMIT, PAGE_READWRITE);\n"
                  "#else\n"
                  "    uint8_t* t = (uint8_t*)VirtualAlloc(want, 0x1000, MEM_COMMIT, PAGE_READWRITE);\n"
                  "#endif\n"
                  "    if (t != want) {\n"
                  "        Log(\"32-bit: thread block %p not committed (error %lu)\", want, GetLastError());\n"
                  "        return nullptr;\n"
                  "    }\n"
                  "#if 0   // (granule placement, before the thread block area)", 1)
    s = s.replace("    memset(t, 0, 0x1000);", "#endif\n    memset(t, 0, 0x1000);", 1)
    # the "d" lambda must exist before stateToRax/stateToRcx use it
p.write_text(s, encoding="utf-8")
print("ok", s.count("g_tebShift"))
