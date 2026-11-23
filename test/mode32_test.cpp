// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// 32-bit bridge, step 0: can a 64-bit process run 32-bit x86 code directly? The CPU runs 32-bit code when the code
// segment is the 32-bit one (selector 0x23 on 64-bit Windows, the one WoW64 uses) and back with 0x33. This test
// builds, in memory below 4 GB: a 64-bit entry that moves to a stack below 4 GB and far-returns into 32-bit code;
// 32-bit code that computes, writes memory and reads FS:[0x18] (the 32-bit thread block pointer, if the system set
// one); and a far jump back to 64-bit code that restores the stack and returns the result.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <initializer_list>

int main() {
    bool fsReady = false;
    uint8_t* t32base = nullptr;
    char buf[200];
    uint8_t* low = nullptr;
    for (uintptr_t at = 0x10000000; at < 0x70000000 && !low; at += 0x01000000)
        low = (uint8_t*)VirtualAlloc((void*)at, 0x40000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    sprintf_s(buf, "mode32: memory below 4 GB at %p", low);
    OutputDebugStringA(buf);
    if (!low || (uintptr_t)low > 0xFFF00000) return 1;
    uint8_t* code64 = low;              // 64-bit entry and return
    uint8_t* code32 = low + 0x1000;     // 32-bit code
    uint32_t* data = (uint32_t*)(low + 0x2000);
    uint8_t* stackTop = low + 0x3F000;  // 32-bit stack, grows down
    uint64_t* saved = (uint64_t*)(low + 0x2100);
    {   // where 32-bit code would find its thread block: the 64-bit TEB + 0x2000 (as WoW64 lays it out)
        uint8_t* teb = (uint8_t*)NtCurrentTeb();
        MEMORY_BASIC_INFORMATION mi = {};
        VirtualQuery(teb + 0x2000, &mi, sizeof mi);
        sprintf_s(buf, "mode32: TEB %p, TEB+0x2000 state 0x%lx protect 0x%lx", teb, mi.State, mi.Protect);
        OutputDebugStringA(buf);
        // a 32-bit thread block of our own there: committed, NtTib.Self (+0x18) pointing at itself, an empty
        // SEH chain (+0x00 = -1), stack limits (+0x04 top, +0x08 bottom)
        // in 32-bit mode FS's base is only 32 bits: the low half of TEB+0x2000
        // GS keeps the 64-bit TEB as its base across thread switches; in 32-bit mode a GS access lands at the low
        // 32 bits of TEB + offset: the 32-bit thread block goes there
        uint8_t* want = (uint8_t*)(uintptr_t)(uint32_t)(uintptr_t)teb;
        uint8_t* t32 = (uint8_t*)VirtualAlloc((void*)((uintptr_t)want & ~0xFFFull), 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (t32) t32 = want;
        t32base = t32;
        if (t32 && (uintptr_t)t32 < 0x100000000ull) {
            *(uint32_t*)(t32 + 0x00) = 0xFFFFFFFF;
            *(uint32_t*)(t32 + 0x04) = (uint32_t)(uintptr_t)stackTop;
            *(uint32_t*)(t32 + 0x08) = (uint32_t)(uintptr_t)(low + 0x4000);
            *(uint32_t*)(t32 + 0x18) = (uint32_t)(uintptr_t)t32;
            fsReady = true;
        }
        sprintf_s(buf, "mode32: own 32-bit thread block at %p (%s)", t32, t32 && (uintptr_t)t32 < 0x100000000ull ? "below 4 GB, used" : "not usable");
        OutputDebugStringA(buf);
    }

    size_t n = 0;
    auto e64 = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) code64[n++] = x; };
    auto i64 = [&](uint64_t v) { memcpy(code64 + n, &v, 8); n += 8; };
    // --- 64-bit entry: save callee-saved registers, stack switch, far return into 32-bit code
    e64({0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});   // push rbx rbp rsi rdi r12-r15
    e64({0x48, 0xB8}); i64((uint64_t)saved);                                            // mov rax, saved
    e64({0x48, 0x89, 0x20});                                                            // mov [rax], rsp
    e64({0x48, 0xBC}); i64((uint64_t)stackTop);                                         // mov rsp, stackTop
    e64({0x6A, 0x23});                                                                  // push 0x23 (32-bit code segment)
    e64({0x48, 0xB8}); i64((uint64_t)code32);                                           // mov rax, code32
    e64({0x50});                                                                        // push rax
    e64({0x48, 0xCB});                                                                  // retfq -> 32-bit mode
    size_t back = n;                                                                    // --- back in 64-bit mode
    e64({0x48, 0xB9}); i64((uint64_t)saved);                                            // mov rcx, saved
    e64({0x48, 0x8B, 0x21});                                                            // mov rsp, [rcx]
    e64({0x48, 0xB9}); i64((uint64_t)&data[4]);                                         // mov rcx, &data[4]
    e64({0x8B, 0x01});                                                                  // mov eax, [rcx]
    e64({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B});   // pop r15-r12 rdi rsi rbp rbx
    e64({0xC3});                                                                        // ret (eax = result)
    // --- 32-bit code
    size_t m = 0;
    auto e32 = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) code32[m++] = x; };
    auto i32 = [&](uint32_t v) { memcpy(code32 + m, &v, 4); m += 4; };
    e32({0xB8}); i32(40);                                       // mov eax, 40
    e32({0x05}); i32(2);                                        // add eax, 2
    e32({0xA3}); i32((uint32_t)(uintptr_t)&data[0]);            // mov [data0], eax
    e32({0x54});                                                // push esp (32-bit push: 4 bytes)
    e32({0x58});                                                // pop eax
    e32({0xA3}); i32((uint32_t)(uintptr_t)&data[1]);            // mov [data1], eax   (the 32-bit stack pointer)
    if (fsReady) {   // the 32-bit thread block: read 2 billion times (thread switches happen); stop at a wrong value
        e32({0x65, 0xA1}); i32(0x18);                           // mov eax, gs:[0x18]  (TEB32 self pointer)
        e32({0xA3}); i32((uint32_t)(uintptr_t)&data[2]);        // mov [data2], eax
        e32({0xB9}); i32(2000000000u);                          // mov ecx, 2e9
        size_t loop = m;
        e32({0x65, 0x8B, 0x15}); i32(0x18);                     // mov edx, gs:[0x18]
        e32({0x39, 0xC2});                                      // cmp edx, eax
        e32({0x75, 0x03});                                      // jne +3 (out, ecx = reads left)
        e32({0x49});                                            // dec ecx
        e32({0x75}); code32[m] = (uint8_t)(int8_t)(loop - (m + 1)); m++;   // jnz loop
        e32({0x89, 0x0D}); i32((uint32_t)(uintptr_t)&data[5]);  // mov [data5], ecx  (0 = all reads right)
    }
    e32({0x8C, 0xE0});                                          // mov eax, fs
    e32({0xA3}); i32((uint32_t)(uintptr_t)&data[3]);            // mov [data3], eax
    e32({0xC7, 0x05}); i32((uint32_t)(uintptr_t)&data[4]); i32(0x32B17E57);   // mov dword [data4], result
    e32({0xEA}); i32((uint32_t)(uintptr_t)(code64 + back));     // jmp far 0x33:back
    code32[m++] = 0x33; code32[m++] = 0x00;
    FlushInstructionCache(GetCurrentProcess(), low, 0x3000);
    data[2] = 0xDEADBEEF;
    data[5] = 0xFFFFFFFF;
    typedef uint32_t (*fn_t)();
    uint32_t r = 0;
    __try {
        r = ((fn_t)code64)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sprintf_s(buf, "mode32: exception 0x%08lx in the switch", GetExceptionCode());
        OutputDebugStringA(buf);
        return 2;
    }
    sprintf_s(buf, "mode32: back in 64-bit, result 0x%08x, 32-bit code computed %u, 32-bit stack %08x, fs:[18h] %08x, fs selector %04x, wrong reads left %u",
              r, data[0], data[1], data[2], data[3], data[5]);
    OutputDebugStringA(buf);
    return r == 0x32B17E57 && data[0] == 42 ? 0 : 3;
}
