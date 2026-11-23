// 32-bit program using its own DLL (testdll32.dll in its folder)
#include <windows.h>
__declspec(dllimport) int __stdcall TestAdd(int a, int b);
void __stdcall Start(void) { OutputDebugStringA("usedll32: calling the DLL"); ExitProcess((UINT)TestAdd(20, 3)); }
