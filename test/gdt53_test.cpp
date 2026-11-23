// what base does the 32-bit FS selector (0x53) give after a reload? (64-bit code: mov fs,0x53 then RDFSBASE)
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
int main() {
    uint8_t* code = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    // mov ax, 0x53 ; mov fs, ax ; rdfsbase rax ; push rax ; mov ax, 0 ... keep: rax = base ; ret
    const uint8_t c[] = {0x66, 0xB8, 0x53, 0x00, 0x8E, 0xE0, 0xF3, 0x48, 0x0F, 0xAE, 0xC0, 0xC3};
    memcpy(code, c, sizeof c);
    char buf[200];
    for (int i = 0; i < 3; i++) {
        uint64_t base = ((uint64_t(*)())code)();
        sprintf_s(buf, "gdt53: base after reload %llx, TEB %p, TEB+0x2000 %p", base, NtCurrentTeb(), (char*)NtCurrentTeb() + 0x2000);
        OutputDebugStringA(buf);
        Sleep(50);
    }
    return 0;
}
