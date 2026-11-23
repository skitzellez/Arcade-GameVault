// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
//
// Sega Europa-R games (32-bit). Ford Racing Full Blown: its I/O board is a USB HID device; the patches are
// OpenParrot's (Functions/Games/Europa-R/FordRacing.cpp, GPL-3.0): the game's HID set-up is replaced by one that
// says "a JOYSTICK is there", its input ReadFile reads our 8-byte report (TeknoParrot's EuropaRPipeHandler layout:
// [1],[2] wheel, [3] gas, [5] brake, [6] 1 gear up / 2 gear down / 4 start / 8 view, [7] 0x0C), and the force
// feedback port writes (inpout32 Out32) go nowhere. The controls come from the auto mapper (TeknoParrot's FR.xml).

#include "peload.h"
#include <windows.h>
#include <cstring>
#include <vector>

namespace arcade {

static void Write32(uint32_t at, const void* bytes, size_t n) {
    DWORD old;
    VirtualProtect((void*)(uintptr_t)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)(uintptr_t)at, bytes, n);
    VirtualProtect((void*)(uintptr_t)at, n, old, &old);
}
static void Jmp32(uint32_t at, uint32_t to) {
    uint8_t j[5] = {0xE9};
    int32_t rel = (int32_t)(to - (at + 5));
    memcpy(j + 1, &rel, 4);
    Write32(at, j, 5);
}
static bool Bytes32(uint32_t at, const char* hex) {   // the game's code there is what we expect
    for (size_t i = 0; hex[i * 2]; i++) {
        unsigned v;
        sscanf_s(hex + i * 2, "%2x", &v);
        if (((const uint8_t*)(uintptr_t)at)[i] != (uint8_t)v) return false;
    }
    return true;
}

// --- Ford Racing ---------------------------------------------------------------------------------------------------
static uint64_t Ford_ReadInput(uint64_t, uint64_t buf, uint64_t n, uint64_t done, uint64_t) {
    uint8_t r[8] = {};
    PadIn p = CurrentPad();
    std::vector<uint8_t> sw, an;
    ControlsSwitches(p, 1, 2, sw);   // player 1: 2 bytes
    ControlsAnalog(p, 3, an);        // wheel, gas, brake (2 bytes each, high first)
    r[1] = r[2] = an[0];
    r[3] = an[2];
    r[5] = an[4];
    if (sw[0] & 0x02) r[6] |= 0x01;   // Button 1: shift up
    if (sw[0] & 0x01) r[6] |= 0x02;   // Button 2: shift down
    if (sw[0] & 0x80) r[6] |= 0x04;   // Start
    if (sw[1] & 0x80) r[6] |= 0x08;   // Button 3: view change
    r[7] = 0x0C;
    uint32_t want = (uint32_t)n < 8 ? (uint32_t)n : 8;
    memcpy((void*)(uintptr_t)(uint32_t)buf, r, want);
    if (done) *(uint32_t*)(uintptr_t)(uint32_t)done = want;
    Sleep(15);   // the board answers about this often
    return TRUE;
}
static uint64_t Ford_ControlInit() {
    uint32_t a[3] = {0x4DE030, 0, 0};
    Call32(0x4E0803, a, 3);                    // the game's input thread (_beginthread)
    *(uint32_t*)(uintptr_t)0x6B02A0 = 1;       // input set up
    *(uint32_t*)(uintptr_t)0x6B0290 = 1;       // one device
    static char* name = nullptr;
    if (!name) {
        name = (char*)Alloc32(16);
        strcpy_s(name, 16, "JOYSTICK");
    }
    *(uint32_t*)(uintptr_t)0x6A9AF4 = (uint32_t)(uintptr_t)name;
    *(uint32_t*)(uintptr_t)0x6A9A74 = (uint32_t)(uintptr_t)CreateMutexW(nullptr, FALSE, nullptr);
    Log("Ford Racing: controls set up (OpenParrot's way: one JOYSTICK)");
    return 1;
}
static uint64_t Ford_Ffb(uint64_t, uint64_t) { return 0; }

static void FordRacing() {
    if (!Bytes32(0x4DE003, "ff150cf14e00") || !Bytes32(0x4DE1F0, "83ec1857") || !Bytes32(0x467C9A, "ff2530f04e00")) {
        Log("Ford Racing: not the version the patches are for, controls left alone");
        return;
    }
    uint32_t* slot = (uint32_t*)Alloc32(4);
    *slot = BridgeFunction32((void*)Ford_ReadInput, 5, "Ford Racing input (ReadFile)");
    uint32_t at = (uint32_t)(uintptr_t)slot;
    Write32(0x4DE005, &at, 4);
    Jmp32(0x4DE1F0, BridgeFunction32((void*)Ford_ControlInit, 0, "Ford Racing control set-up"));
    Jmp32(0x467C9A, BridgeFunction32((void*)Ford_Ffb, 2, "Ford Racing force feedback"));
    if (Bytes32(0x468032, "cc")) {   // a debug break at the start of its log function (a cabinet has no debugger)
        uint8_t nop = 0x90;
        Write32(0x468032, &nop, 1);
    }
    Log("Ford Racing: I/O board patches in (OpenParrot)");
}

uint32_t BridgeThunk32(const char* name, void* fn, int args, bool cdecl_);   // peload32.cpp
// Caladrius AC: Sega amLib's messages (its print function +0x1572b0: function, line, format, ...) into the log
static uint64_t AmLibPrint(uint64_t func, uint64_t line, uint64_t fmt, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    static int told = 0;
    if (told++ > 300) return 0;
    char msg[512] = "";
    const char* f = (uint32_t)fmt ? (const char*)(uintptr_t)(uint32_t)fmt : "";
    _snprintf_s(msg, _TRUNCATE, f, (uintptr_t)(uint32_t)a0, (uintptr_t)(uint32_t)a1, (uintptr_t)(uint32_t)a2, (uintptr_t)(uint32_t)a3,
                (uintptr_t)(uint32_t)a4);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == 10 || msg[n - 1] == 13)) msg[--n] = 0;
    Log("amLib %s:%u: %s", (uint32_t)func ? (const char*)(uintptr_t)(uint32_t)func : "?", (uint32_t)line, msg);
    return 0;
}
static void Patch32Jump(uint32_t at, uint32_t to) {
    DWORD old;
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)(to - (at + 5));
    VirtualProtect(p, 5, old, &old);
}
void Setup32ForGame(const std::wstring& exePath) {
    std::wstring name = exePath.substr(exePath.find_last_of(L"\\/") + 1);
    if (!_wcsicmp(name.c_str(), L"fordracing.exe")) FordRacing();
    // Dragon Dance (NESiCAxLive): applying display settings frees its effect object (+0x3cbfa4) and recreates the
    // device, whose first frame draws before the object is back while its "created" flag is still set (method call on
    // NULL at +0x1174b): the frame checks the object itself. (aderumier/windowsarcadeloader dragon-dance.yaml, GPL-3.0)
    if (!_wcsicmp(name.c_str(), L"game_liong.exe")) {
        static const uint8_t was[15] = {0xA0, 0xA8, 0xBF, 0x7C, 0x00, 0x84, 0xC0, 0x8B, 0x0D, 0xA4, 0xBF, 0x7C, 0x00, 0x75, 0x12};
        static const uint8_t now[15] = {0x8B, 0x0D, 0xA4, 0xBF, 0x7C, 0x00, 0x85, 0xC9, 0x75, 0x17, 0xEB, 0x20, 0x90, 0x90, 0x90};
        uint8_t* p = (uint8_t*)(uintptr_t)0x40F804;
        if (!memcmp(p, was, 15)) {
            DWORD old;
            VirtualProtect(p, 15, PAGE_EXECUTE_READWRITE, &old);
            memcpy(p, now, 15);
            VirtualProtect(p, 15, old, &old);
            Log("Dragon Dance: frame checks its effect object (crash at +0x1174b)");
            CloseHandle(CreateThread(nullptr, 0, [](void*) -> DWORD {   // (testing) the effect object and its flag
                uint32_t was = 1, wasF = 2;
                for (int i = 0; i < 3000; i++) {
                    uint32_t o = *(volatile uint32_t*)0x7cbfa4, f = *(volatile uint8_t*)0x7cbfa8;
                    if ((o != 0) != (was != 0) || f != wasF) Log("Dragon Dance: effect object %08x, flag %u (%d ms)", o, f, i * 20);
                    was = o, wasF = f;
                    Sleep(20);
                }
                return 0;
            }, nullptr, 0, nullptr));
        } else {
            Log("Dragon Dance: other build, effect object check not applied");
        }
    }
    if (!_wcsicmp(name.c_str(), L"SILVER_AMP_R_ROM.exe") && *(const uint32_t*)0x5572b0 == 0x0C244C8B) {   // mov ecx, [esp+0xc]
        Patch32Jump(0x5572b0, BridgeThunk32("amLib print", (void*)AmLibPrint, 8, true));
        Log("Caladrius: amLib's messages logged");
        static const uint32_t kLevels[] = {0x6d7410, 0x6d7418, 0x6d7858, 0x6d7c40, 0x6e3f90, 0x6e4220, 0x6e422c, 0x6e4238, 0x6e423c, 0x6e4240,
                                           0x6e4b60, 0x6e5068, 0x6e50f0, 0x6e5668, 0x6e5ba0, 0x6e8ff8, 0x6e9210, 0x6e9218, 0x6e9320, 0x6ed300};
        for (uint32_t v : kLevels) *(volatile int32_t*)(uintptr_t)v = 3;   // (testing) amLib modules' message levels: errors and notes
        {   // (EXPERIMENT) start-up goes on past its two save-record checks, to amLib's setup
            uint8_t* p1 = (uint8_t*)(uintptr_t)0x413f86;
            uint8_t* p2 = (uint8_t*)(uintptr_t)0x413fc1;
            DWORD old;
            if (p1[0] == 0x74 && p2[0] == 0x75) {
                VirtualProtect((void*)0x413f80, 0x50, PAGE_EXECUTE_READWRITE, &old);
                p1[0] = 0xEB;
                p2[0] = 0x90, p2[1] = 0x90;
                VirtualProtect((void*)0x413f80, 0x50, old, &old);
                Log("Caladrius: (experiment) start-up past the save-record checks");
            }
        }
        CloseHandle(CreateThread(nullptr, 0, [](void*) -> DWORD {   // (testing) the input module's flag, as it changes
            uint32_t was = 0xFFFFFFFF;
            for (int i = 0; i < 6000; i++) {
                uint32_t now = *(volatile uint32_t*)0x6d7860;
                if (now != was) Log("Caladrius: input module flag %u -> %u (%d ms)", was, now, i * 10);
                was = now;
                Sleep(10);
            }
            return 0;
        }, nullptr, 0, nullptr));
    }
}

}  // namespace arcade
