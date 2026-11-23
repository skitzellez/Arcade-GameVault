// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct IUnknown;

namespace arcade {

struct LoadedImage {
    uint8_t* base = nullptr;
    size_t size = 0;
    void* entry = nullptr;
    void* tls = nullptr;
    std::vector<std::string> missing;   // "dll!function" with no stand-in and no system function
};

struct LoadedImage32 {   // a 32-bit program (peload32.cpp): everything below 4 GB
    uint32_t base = 0, size = 0, entry = 0;
    uint32_t tlsDir = 0;   // its IMAGE_TLS_DIRECTORY32, 0 if none
    uint32_t exportDir = 0, exportSize = 0;
    std::vector<std::string> missing;
};
// maps a 32-bit Windows .exe below 4 GB with its imports bridged to 64-bit; Run32 runs it on this thread
bool MapProgram32(const std::vector<uint8_t>& file, LoadedImage32& out);
int Run32(const LoadedImage32& img);
// where the program's own 32-bit DLLs are (set before MapProgram32)
void SetProgramFolder32(const std::wstring& folder);
// where our own 32-bit DLLs are (looked at before the program's folder)
void SetOurFolder32(const std::wstring& folder);
// 64-bit code calling a 32-bit function (stdcall), during Run32
uint32_t Call32(uint32_t func, const uint32_t* args, int n);

void SetLog(void (*fn)(const char*));
// testing: log the first calls of every import (set before MapProgram)
void SetTrace(bool on);
void Log(const char* fmt, ...);

// maps a 64-bit Windows .exe (its bytes) into our memory, ready to run
bool MapProgram(const std::vector<uint8_t>& file, LoadedImage& out);
// runs its TLS callbacks and entry point on a new thread (16 MB stack)
bool Run(const LoadedImage& img, bool wait);
// ends the program's thread (its ExitProcess)
void ExitProgram(int code);

// 64-bit DLLs mapped by us (peload.cpp): folders searched, in order (the program's own, then ours)
void AddDllFolder64(const std::wstring& dir);
void* Dll64Load(const char* name);                        // loads it if found: its base (HMODULE), or null
void* Dll64Loaded(const char* name);                      // only if already loaded
bool Dll64Export(void* module, const char* func, void** out);   // false: not one of ours (func may be an ordinal)
bool Dll64Path(void* module, std::wstring& path);
// arcade_io.cpp: per-game arcade hardware stand-ins (JVS board, dongle); choose before mapping the program
void ChooseProfile(const std::wstring& exePath);
void* FindIoShim(const char* dll, const char* func);
const char* KnownOrdinalName(const char* dll, uint16_t ordinal);
// msvcrt_compat.cpp: old msvcrt.dll functions ucrtbase lacks
void* FindMsvcrtShim(const char* dll, const char* func);
// the base of the image (program or DLL) we mapped that holds this address, or null
void* MappedImageBase(const void* pc);
// the name of the module we mapped that holds this address ("program", a DLL's name), or "other"
int64_t TbbHeldBytes();   // tbbmalloc_standin.cpp (testing): what the game holds through it now / at most
int64_t TbbPeakBytes();
void* BigAllocMaybe(size_t size);   // filebacked.cpp
bool BigFree(void* p);
bool BigSize(const void* p, size_t* size);
bool BigRead(void* h, void* buf, unsigned long n, unsigned long* done, void* ov);   // ov: OVERLAPPED*
void SetFileBacked(bool on);
void BigPrepare(void* buf, unsigned long n);
bool BigLazyRead(void* h, void* buf, unsigned long n, unsigned long* done, void* ov);
int64_t FileBackedBytes();
void* FindXAudio27Shim(const char* dll, const char* func);   // xaudio27.cpp
void ApplyProfilePatches(uint8_t* base, size_t size, const std::wstring& exePath);   // arcade_io.cpp
void SetPadReader(bool (*reader)(unsigned index, void* xinputState));   // arcade_io.cpp: the Xbox pads
struct PadIn {   // one Xbox pad, XInput-style (controls.cpp)
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
    bool service;
};
bool ControlsLoad(const std::wstring& teknoParrotProfileXml);   // controls.cpp: the auto mapper
bool ControlsOn();
void ControlsSwitches(const PadIn& p, int players, int per, std::vector<uint8_t>& reply);
void ControlsAnalog(const PadIn& p, int channels, std::vector<uint8_t>& reply);
PadIn CurrentPad();   // arcade_io.cpp: pad 1 now
void Setup32ForGame(const std::wstring& exePath);   // europa.cpp: per-game 32-bit patches (after MapProgram32)
uint32_t BridgeFunction32(void* target, int args, const std::string& name);   // peload32.cpp
void* Alloc32(size_t size);   // peload32.cpp: memory below 2 GB
bool InProgram32();   // peload32.cpp: called from the 32-bit program right now
// netfs.cpp: a game played straight from the NAS (its folder is \\gvnas\game for the program)
bool NetFsMount(const std::wstring& exeUrl, const std::wstring& rootUrl, const std::wstring& credFile, const std::wstring& local,
                std::wstring& exeOut, std::string& error);
bool NetFsOn();
bool NetFsReadAll(const std::wstring& path, std::vector<uint8_t>& out);
bool NetFsHandle(void* h);
void NetFsAddRef(void* h);
uint64_t NetFsSize(void* h);
uint64_t NetFsPos(void* h);
size_t NetFsPread(void* h, uint64_t at, void* out, size_t n);
void* FindNetFsShim(const char* dll, const char* func);
void* FindIoShim(const char* dll, const char* func);
std::wstring PatchedFileFor(const std::wstring& path);   // arcade_io.cpp: a game's data file patched on the way in
void SetProfilesFolder(const std::wstring& dir);   // arcade_io.cpp: TeknoParrot game profiles shipped with us
void SetGameScreen(int width, int height);   // window_xbox.cpp (Xbox); the PC build: no-op
const char* MappedImageName(const void* pc);
// memtally.cpp (testing): memory counted by module
bool MemTallyOn();
void SetMemTally(bool on);
void* FindMemTallyShim(const char* dll, const char* func);
void MemTallyAdd(const char* who, uintptr_t key, size_t size);
void MemTallyRemove(uintptr_t key);
// tbbmalloc_standin.cpp: TBB 3.0 allocator -> process heap (Xbox; PC with ARCADE_TBB_STANDIN)
void* FindTbbShim(const char* dll, const char* func);
// shims.cpp
void* FindShim(const char* dll, const char* func);
void SetProgram(const LoadedImage* img, const std::wstring& path, const std::wstring& commandLine);

// window_xbox.cpp (Xbox build only): the game window is our CoreWindow
void SetCoreWindow(::IUnknown* w, int width, int height);
void* FindWindowShim(const char* dll, const char* func);
void InstallDxgiHooks();
void SetShotPath(const std::wstring& path);   // testing: where frame 600 is saved

}  // namespace arcade
