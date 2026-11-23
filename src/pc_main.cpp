// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
// PC test driver: ArcadeLoaderPC.exe <program.exe> [arguments]
#include "peload.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iterator>
static void LogOut(const char* s) { printf("%s\n", s); fflush(stdout); }
// Mesa Dozen (an Xbox build, used on the PC for testing) asks the host for its window: none on a PC, so its swap
// chains fail cleanly and a program still draws (frames read back by the program)
namespace arcade {
void SetGameScreen(int, int) {}   // the PC: the game's own window size
}
extern "C" __declspec(dllexport) void* MesaUwpHostWindow(int* width, int* height) {
    if (width) *width = 1280;
    if (height) *height = 720;
    return nullptr;
}
// PC testing: Direct3D 12 from the Agility SDK in out\D3D12 (its debug layer, no Windows "Graphics Tools" needed)
extern "C" __declspec(dllexport) extern const UINT D3D12SDKVersion = 619;   // = Mesa's headers (Agility 1.619.x)
extern "C" __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    arcade::SetLog(LogOut);
    if (GetEnvironmentVariableA("ARCADE_TRACE", nullptr, 0)) arcade::SetTrace(true);
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    {   // the program's folder: its own DLLs are loaded from there
        std::wstring dir = argv[1];
        size_t k = dir.find_last_of(L"/\\");
        arcade::SetProgramFolder32(k == std::wstring::npos ? L".\\" : dir.substr(0, k + 1));
        arcade::AddDllFolder64(k == std::wstring::npos ? L".\\" : dir.substr(0, k + 1));
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring own = self;
        own = own.substr(0, own.find_last_of(L"\\") + 1) + L"x64";   // our DLLs (DXVK's d3d9.dll)
        arcade::AddDllFolder64(own);
        arcade::SetOurFolder32(own.substr(0, own.size() - 3) + L"x86dll");
        arcade::SetProfilesFolder(own.substr(0, own.size() - 3) + L"tpprofiles");
    }
    arcade::ChooseProfile(argv[1]);
    static arcade::LoadedImage32 img32;
    if (arcade::MapProgram32(bytes, img32)) {   // a 32-bit program: through the bridge
        arcade::Setup32ForGame(argv[1]);
        arcade::Run32(img32);
        LogOut("loader: back in the loader after the 32-bit program");
        return 0;
    }
    static arcade::LoadedImage img;
    arcade::ChooseProfile(argv[1]);
    if (!arcade::MapProgram(bytes, img)) return 1;
    arcade::ApplyProfilePatches(img.base, img.size, argv[1]);
    std::wstring args;
    for (int i = 2; i < argc; i++) args += (i > 2 ? L" " : L"") + std::wstring(argv[i]);
    arcade::SetProgram(&img, argv[1], args);
    arcade::Run(img, true);
    LogOut("loader: back in the loader after the program");
    return 0;
}
