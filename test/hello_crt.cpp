// test program with the C/C++ runtime (/MD): strings, exceptions, heap, exit()
#include <windows.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    std::vector<std::string> v;
    for (int i = 0; i < 1000; i++) v.push_back("item " + std::to_string(i));
    char buf[256];
    snprintf(buf, sizeof buf, "crt test: %zu items, last '%s', argc %d", v.size(), v.back().c_str(), argc);
    OutputDebugStringA(buf);
    try {
        throw std::runtime_error("thrown inside the loaded program");
    } catch (const std::exception& e) {
        snprintf(buf, sizeof buf, "caught: %s", e.what());
        OutputDebugStringA(buf);
    }
    exit(3);
}
