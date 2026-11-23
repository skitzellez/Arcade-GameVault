# one-off edit: a 32-bit PEB and the program's TLS block in the 32-bit thread block (peload32.cpp)
from pathlib import Path

root = Path(__file__).resolve().parent.parent
h = root / "src" / "peload.h"
s = h.read_text(encoding="utf-8")
if "tlsDir" not in s:
    s = s.replace("    uint32_t base = 0, size = 0, entry = 0;\n    std::vector<std::string> missing;\n};",
                  "    uint32_t base = 0, size = 0, entry = 0;\n    uint32_t tlsDir = 0;   // its IMAGE_TLS_DIRECTORY32, 0 if none\n"
                  "    std::vector<std::string> missing;\n};", 1)
    h.write_text(s, encoding="utf-8")

p = root / "src" / "peload32.cpp"
s = p.read_text(encoding="utf-8")
if "tlsDir =" not in s:
    s = s.replace("    out.entry = out.base + oh.AddressOfEntryPoint;",
                  "    out.entry = out.base + oh.AddressOfEntryPoint;\n"
                  "    if (oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)\n"
                  "        out.tlsDir = out.base + oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress;", 1)
old_start = "static uint8_t* MakeTeb32(uint32_t stackTop, uint32_t stackBottom) {"
new_block = '''// the 32-bit process block (PEB): 32-bit code reads it straight from FS:[0x30]
static uint8_t* g_peb32 = nullptr;
static uint8_t* MakePeb32() {
    if (g_peb32) return g_peb32;
    g_peb32 = (uint8_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 0x1000);
    if (!g_peb32) return nullptr;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    OSVERSIONINFOW ov = {sizeof ov};
    typedef LONG(NTAPI * ver_t)(OSVERSIONINFOW*);
    if (auto rv = (ver_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) rv(&ov);
    *(uint32_t*)(g_peb32 + 0x08) = g_img32->base;                          // ImageBaseAddress
    *(uint32_t*)(g_peb32 + 0x18) = (uint32_t)(uintptr_t)LowHeap();         // ProcessHeap
    *(uint32_t*)(g_peb32 + 0x64) = si.dwNumberOfProcessors;                // NumberOfProcessors
    *(uint32_t*)(g_peb32 + 0xA4) = ov.dwMajorVersion;                      // OSMajorVersion
    *(uint32_t*)(g_peb32 + 0xA8) = ov.dwMinorVersion;                      // OSMinorVersion
    *(uint16_t*)(g_peb32 + 0xAC) = (uint16_t)ov.dwBuildNumber;             // OSBuildNumber
    *(uint32_t*)(g_peb32 + 0xB0) = 2;                                      // OSPlatformId (NT)
    *(uint32_t*)(g_peb32 + 0x88) = 1;                                      // NumberOfHeaps
    return g_peb32;
}
// the program's thread-local data (__declspec(thread), C++ magic statics): one block copied from its template,
// its address in an array at TEB+0x2C (ThreadLocalStoragePointer), index 0 written to the program's _tls_index
static uint32_t MakeTls32() {
    if (!g_img32->tlsDir) return 0;
    auto d = (const IMAGE_TLS_DIRECTORY32*)(uintptr_t)g_img32->tlsDir;
    uint32_t tmpl = d->StartAddressOfRawData, len = d->EndAddressOfRawData - d->StartAddressOfRawData;
    uint8_t* block = (uint8_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, len + d->SizeOfZeroFill + 16);
    uint32_t* arr = (uint32_t*)HeapAlloc(LowHeap(), HEAP_ZERO_MEMORY, 64 * 4);
    if (!block || !arr) return 0;
    memcpy(block, (const void*)(uintptr_t)tmpl, len);
    arr[0] = (uint32_t)(uintptr_t)block;
    if (d->AddressOfIndex) *(uint32_t*)(uintptr_t)d->AddressOfIndex = 0;
    if (d->AddressOfCallBacks && *(const uint32_t*)(uintptr_t)d->AddressOfCallBacks)
        Log("32-bit: the program has TLS callbacks (not run yet)");
    return (uint32_t)(uintptr_t)arr;
}

'''
if "MakePeb32" not in s:
    s = s.replace(old_start, new_block + old_start, 1)
    s = s.replace("    *(uint32_t*)(t + 0x24) = GetCurrentThreadId();\n",
                  "    *(uint32_t*)(t + 0x24) = GetCurrentThreadId();\n"
                  "    *(uint32_t*)(t + 0x2C) = MakeTls32();                     // ThreadLocalStoragePointer\n"
                  "    *(uint32_t*)(t + 0x30) = (uint32_t)(uintptr_t)MakePeb32(); // ProcessEnvironmentBlock\n", 1)
p.write_text(s, encoding="utf-8")
print("ok", "MakePeb32" in s, "t + 0x30" in s)
