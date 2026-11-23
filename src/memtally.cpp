// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// (testing) WHERE A GAME'S MEMORY GOES: with ARCADE_MEMTALLY set (PC) or memtally.txt in LocalState (Xbox), every
// VirtualAlloc commit and every big HeapAlloc (1 MB+) of the program and the DLLs we map is counted by the module
// that asked (its return address), and the ranking is logged every 10 s. An Xbox game app has ~5 GB in all.

#include "peload.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace arcade {

static std::mutex g_tallyLock;
static std::map<std::string, int64_t> g_bytes;            // module -> bytes committed now
static std::map<uintptr_t, std::pair<std::string, size_t>> g_blocks;   // address -> (module, size)
static bool g_tallyOn = false;

bool MemTallyOn() {
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableA("ARCADE_MEMTALLY", nullptr, 0) > 0;
    return on || g_tallyOn;
}
void SetMemTally(bool on) { g_tallyOn = on; }

static void Count(void* ret, uintptr_t at, size_t size) {
    std::string who = MappedImageName(ret);
    std::lock_guard<std::mutex> l(g_tallyLock);
    g_blocks[at] = {who, size};
    g_bytes[who] += (int64_t)size;
}
static void Uncount(uintptr_t at) {
    std::lock_guard<std::mutex> l(g_tallyLock);
    auto it = g_blocks.find(at);
    if (it == g_blocks.end()) return;
    g_bytes[it->second.first] -= (int64_t)it->second.second;
    g_blocks.erase(it);
}
static void Reporter() {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread([] {
        for (;;) {
            Sleep(10000);
            std::vector<std::pair<int64_t, std::string>> v;
            {
                std::lock_guard<std::mutex> l(g_tallyLock);
                for (auto& b : g_bytes) v.push_back({b.second, b.first});
            }
            std::sort(v.rbegin(), v.rend());
            std::string line = "memory by module (MB):";
            int64_t total = 0;
            for (size_t i = 0; i < v.size(); i++) {
                total += v[i].first;
                if (i < 10) line += " " + v[i].second + "=" + std::to_string(v[i].first >> 20);
            }
            Log("%s | counted %lld MB", line.c_str(), (long long)(total >> 20));
        }
    }).detach();
}

typedef LPVOID(WINAPI* VA_t)(LPVOID, SIZE_T, DWORD, DWORD);
static LPVOID WINAPI T_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    static VA_t real = (VA_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc");
    LPVOID p = real(addr, size, type, prot);
    if (p && (type & MEM_COMMIT)) {
        Reporter();
        // a commit inside a block reserved earlier is counted on its own (reserve-then-commit allocators)
        Count(_ReturnAddress(), (uintptr_t)p, size);
    }
    return p;
}
typedef BOOL(WINAPI* VF_t)(LPVOID, SIZE_T, DWORD);
static BOOL WINAPI T_VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    static VF_t real = (VF_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualFree");
    if (type & (MEM_RELEASE | MEM_DECOMMIT)) Uncount((uintptr_t)addr);
    return real(addr, size, type);
}
typedef LPVOID(WINAPI* HA_t)(HANDLE, DWORD, SIZE_T);
static LPVOID WINAPI T_HeapAlloc(HANDLE h, DWORD flags, SIZE_T size) {
    static HA_t real = (HA_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "HeapAlloc");
    if (!real) real = (HA_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAllocateHeap");
    LPVOID p = real(h, flags, size);
    if (p && size >= (1 << 20)) {
        Reporter();
        Count(_ReturnAddress(), (uintptr_t)p, size);
    }
    return p;
}
typedef BOOL(WINAPI* HF_t)(HANDLE, DWORD, LPVOID);
static BOOL WINAPI T_HeapFree(HANDLE h, DWORD flags, LPVOID p) {
    static HF_t real = (HF_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "HeapFree");
    if (!real) real = (HF_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlFreeHeap");
    if (p) Uncount((uintptr_t)p);
    return real(h, flags, p);
}

void MemTallyAdd(const char* who, uintptr_t key, size_t size) {
    Reporter();
    std::lock_guard<std::mutex> l(g_tallyLock);
    g_blocks[key] = {who, size};
    g_bytes[who] += (int64_t)size;
}
void MemTallyRemove(uintptr_t key) { Uncount(key); }

void* FindMemTallyShim(const char* dll, const char* func) {
    if (!MemTallyOn() || !func) return nullptr;
    if (_stricmp(dll, "kernel32.dll") && _stricmp(dll, "kernelbase.dll") && _strnicmp(dll, "api-ms-win-core-", 16))
        return nullptr;
    if (!strcmp(func, "VirtualAlloc")) return (void*)T_VirtualAlloc;
    if (!strcmp(func, "VirtualFree")) return (void*)T_VirtualFree;
    if (!strcmp(func, "HeapAlloc")) return (void*)T_HeapAlloc;
    if (!strcmp(func, "HeapFree")) return (void*)T_HeapFree;
    return nullptr;
}

}  // namespace arcade
