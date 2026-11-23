// test program without the C runtime: imports kernel32 + user32 only
#include <windows.h>
void Start(void) {
    char buf[128];
    ULONGLONG t = GetTickCount64();
    wsprintfA(buf, "hello from the loaded program, tick %u, cmdline %s", (unsigned)(t & 0xffffff), GetCommandLineA());
    OutputDebugStringA(buf);
    MessageBoxW(NULL, L"MessageBoxW reached the stand-in", L"Arcade loader test", MB_OK);
    HMODULE self = GetModuleHandleW(NULL);
    wsprintfA(buf, "own module handle %p", self);
    OutputDebugStringA(buf);
    ExitProcess(7);
}
