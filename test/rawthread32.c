// 32-bit raw thread test (no C runtime): a thread that returns 5; the program exits with it
#include <windows.h>
static DWORD WINAPI Work(LPVOID p) { OutputDebugStringA("rawthread32: in the thread"); return (DWORD)(UINT_PTR)p + 5; }
void __stdcall Start(void) {
    DWORD tid = 0, code = 0;
    HANDLE h = CreateThread(NULL, 0, Work, (LPVOID)10, 0, &tid);
    OutputDebugStringA(h ? "rawthread32: thread started" : "rawthread32: CreateThread failed");
    WaitForSingleObject(h, INFINITE);
    GetExitCodeThread(h, &code);
    ExitProcess(code);
}
