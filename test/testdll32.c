// 32-bit test DLL: DllMain says hello, exports a stdcall function
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) { if (why == DLL_PROCESS_ATTACH) OutputDebugStringA("testdll32: DllMain attach"); return TRUE; }
__declspec(dllexport) int __stdcall TestAdd(int a, int b) { return a + b + 1000; }
