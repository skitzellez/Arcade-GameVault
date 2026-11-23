# one-off edit: 32-bit thread block reached through GS (kept across thread switches) instead of FS (reset to 0)
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")

patch_fn = '''// 32-bit code finds its thread block through FS. A 64-bit Windows thread keeps FS's base only until its next thread
// switch (then it is 0, and nothing can sit at address 0); GS's base is always the thread's 64-bit TEB, and in 32-bit
// mode a GS access lands at the low 32 bits of it. So the 32-bit thread block lives there (MakeTeb32), and each
// instruction that reads or writes the thread block through FS gets the GS prefix instead (0x64 -> 0x65, same
// length). Matched: FS-prefixed instructions with an absolute address below 0x1000 (the thread block's fields), the
// forms compilers emit (mov eax,fs:[x] / mov fs:[x],eax, and opcodes with a modrm of [disp32]).
static int FsToGs(uint8_t* code, size_t size) {
    static const uint8_t modrmOps[] = {0x8B, 0x89, 0x8A, 0x88, 0xFF, 0x8F, 0xC7, 0xC6, 0x83, 0x81, 0x80, 0x3B, 0x39,
                                       0x3A, 0x38, 0x03, 0x01, 0x2B, 0x29, 0x33, 0x31, 0x0B, 0x09, 0x23, 0x21, 0x85,
                                       0x84, 0x8D, 0x87, 0xF7};
    int n = 0;
    for (size_t i = 0; i + 7 <= size; i++) {
        if (code[i] != 0x64) continue;
        uint8_t op = code[i + 1];
        uint32_t disp;
        if (op >= 0xA0 && op <= 0xA3) {   // mov al/eax <-> moffs32
            memcpy(&disp, code + i + 2, 4);
        } else {
            bool known = false;
            for (uint8_t m : modrmOps) known |= op == m;
            if (!known || (code[i + 2] & 0xC7) != 0x05) continue;   // modrm: [disp32]
            memcpy(&disp, code + i + 3, 4);
        }
        if (disp >= 0x1000) continue;
        code[i] = 0x65;
        n++;
    }
    return n;
}

'''
if "static int FsToGs(" not in s:
    s = s.replace("static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll);",
                  patch_fn + "static bool MapImage32(const std::vector<uint8_t>& file, LoadedImage32& out, const std::string& self, bool isDll);", 1)
    # patch executable sections right before setting protections
    s = s.replace("""    LowProtect(g_arena, 0x40000, PAGE_EXECUTE_READWRITE);   // switch code + thunks; State is written at run time
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {""",
                  """    int fs = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            fs += FsToGs(base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
    if (fs) Log("32-bit %s: %d thread block accesses moved from FS to GS", self.c_str(), fs);
    LowProtect(g_arena, 0x40000, PAGE_EXECUTE_READWRITE);   // switch code + thunks; State is written at run time
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {""", 1)

# the thread block at the low 32 bits of the 64-bit TEB
s = s.replace("""static uint8_t* MakeTeb32(uint32_t stackTop, uint32_t stackBottom) {
    uint8_t* t = AllocBelow4G(0x2000, 0x30000000);
    if (!t) return nullptr;""", """static uint8_t* MakeTeb32(uint32_t stackTop, uint32_t stackBottom) {
    // where a GS access lands in 32-bit mode: the low 32 bits of this thread's 64-bit TEB (see FsToGs)
    uint8_t* want = (uint8_t*)(uintptr_t)(uint32_t)(uintptr_t)NtCurrentTeb();
    uint8_t* t = (uint8_t*)LowAlloc(want, 0x2000);
    if (t != want) {
        Log("32-bit: the thread block's place %p is taken (by memory below 4 GB of ours?)", want);
        return nullptr;
    }""", 1)

# no WRFSBASE any more (FS is not used by 32-bit code now; WRFSBASE may not be allowed on every system)
s = s.replace("""    e({0x48, 0x8B, 0x41, 0x20});                                                    // mov rax, [rcx].teb32
    e({0xF3, 0x48, 0x0F, 0xAE, 0xD0});                                              // wrfsbase rax (a thread switch may have reset FS)
""", "", 1)
s = s.replace("    unsigned long long oldFs = _readfsbase_u64();\n", "", 1)
s = s.replace("    _writefsbase_u64((uint64_t)teb);\n", "", 1)
s = s.replace("    _writefsbase_u64(oldFs);\n", "", 1)
p.write_text(s, encoding="utf-8")
print("ok", "FsToGs(base" in s, "wrfsbase" in s, "_writefsbase" in s)
