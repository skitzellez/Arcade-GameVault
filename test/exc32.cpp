// 32-bit test program: C++ exceptions (throw / catch, destructors on the way) and a __try/__except
#include <windows.h>
#include <stdexcept>
#include <string>
#include <cstdio>
static int g_dtors = 0;
struct Guard { ~Guard() { g_dtors++; } };
static void Deep(int n) { Guard g; if (n == 0) throw std::runtime_error("thrown in 32-bit code"); Deep(n - 1); }
static int Seh() { __try { *(volatile int*)0 = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 7; } return 0; }
int main() {
    char buf[160]; std::string what;
    try { Deep(5); } catch (const std::exception& e) { what = e.what(); }
    sprintf_s(buf, "exc32: caught \"%s\", %d destructors ran", what.c_str(), g_dtors); OutputDebugStringA(buf);
    int r = 100 + g_dtors;
    if (getenv("EXC32_SEH")) { int s = Seh(); sprintf_s(buf, "exc32: __except gave %d", s); OutputDebugStringA(buf); r += s; }
    return r;
}
