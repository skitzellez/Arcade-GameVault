// can user code set its FS base (WRFSBASE)? needs CR4.FSGSBASE, which the OS decides
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <immintrin.h>
int main() {
    char buf[160];
    int info[4] = {};
    __cpuidex(info, 7, 0);
    bool cpu = (info[1] & 1) != 0;   // EBX bit 0: FSGSBASE
    unsigned long long before = 0, after = 0;
    DWORD code = 0;
    __try {
        before = _readfsbase_u64();
        _writefsbase_u64(0x12345000);
        after = _readfsbase_u64();
        _writefsbase_u64(before);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        code = GetExceptionCode();
    }
    sprintf_s(buf, "fsbase: cpu has FSGSBASE %d, exception 0x%08lx, before %llx, after %llx", cpu, code, before, after);
    OutputDebugStringA(buf);
    return 0;
}
