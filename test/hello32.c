// 32-bit test program (no C runtime): kernel32 + user32 calls through the 32-bit bridge
#include <windows.h>
static int Work(int n) { int s = 0; for (int i = 1; i <= n; i++) s += i; return s; }
void __stdcall Start(void) {
    OutputDebugStringA("hello from 32-bit code");
    DWORD t = GetTickCount();
    MessageBoxA(NULL, "MessageBoxA from a 32-bit program", "32-bit test", MB_OK);
    HMODULE self = GetModuleHandleA(NULL);
    int s = Work(100);   // 5050
    OutputDebugStringA(self && t ? "own module handle and tick count: ok" : "own module handle or tick count: MISSING");
    ExitProcess((UINT)s);
}
