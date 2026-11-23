// 32-bit test program with the C/C++ runtime built in (/MT): heap, strings, containers, formatting
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    std::vector<std::string> v;
    for (int i = 0; i < 2000; i++) v.push_back("item " + std::to_string(i));
    char buf[256];
    snprintf(buf, sizeof buf, "crt32: %u items, last '%s', argc %d, malloc'd at %p", (unsigned)v.size(), v.back().c_str(), argc, malloc(64));
    OutputDebugStringA(buf);
    return 77;
}
