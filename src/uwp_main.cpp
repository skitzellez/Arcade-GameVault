// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
// Xbox app: runs each test program shipped in the package's "programs" folder (and any .exe in LocalState\programs)
// through the loader and writes LocalState\arcade_log.txt (fetched through the Device Portal).
#include "peload.h"
#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <cstdio>
#include <xinput.h>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
namespace arcade { extern bool g_sample32; }   // peload32.cpp: the thread sampler (sample.txt)

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {
std::mutex g_lock;
std::wstring g_logPath;
void LogOut(const char* s) {
    std::lock_guard<std::mutex> l(g_lock);
    HANDLE h = CreateFile2FromAppW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(h, s, (DWORD)strlen(s), &n, nullptr);
    WriteFile(h, "\r\n", 2, &n, nullptr);
    CloseHandle(h);
}
std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> v;
    if (arcade::NetFsReadAll(path, v)) return v;   // (a game played from the NAS)
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return v;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    v.resize((size_t)sz.QuadPart);
    DWORD n = 0;
    ReadFile(h, v.data(), (DWORD)v.size(), &n, nullptr);
    CloseHandle(h);
    return v;
}
void RunGuarded(arcade::LoadedImage* img) {
    __try {
        arcade::Run(*img, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        arcade::Log("crashed: exception 0x%08lx", GetExceptionCode());
    }
}
void Run32Guarded(arcade::LoadedImage32* img) {
    __try {
        arcade::Run32(*img);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        arcade::Log("32-bit program crashed: exception 0x%08lx", GetExceptionCode());
    }
}
// Game Vault's link: gamevault-arcade:?cmd=<the game's .exe, whole path>&launchOnExit=<link opened when it ends>
static std::wstring g_cmd, g_onExit, g_root, g_cred;
static std::atomic<bool> g_activated{false};
static std::wstring Unquote(std::wstring v) {
    while (!v.empty() && (v.front() == L'"' || v.front() == L' ')) v.erase(v.begin());
    while (!v.empty() && (v.back() == L'"' || v.back() == L' ')) v.pop_back();
    return v;
}
std::wstring g_only;   // LocalState\only.txt: run only the programs whose file name has this text (testing)
std::wstring g_pkg;
// runs one program: its folder is where its own DLLs are and the current folder (games read data relative to it)
// start options a game needs that TeknoParrot gives it (its .bat / profile): when the link gives none
static std::wstring DefaultArgs(const std::wstring& file) {
    if (!_wcsicmp(file.c_str(), L"AMContra.exe")) return L"-nosecurity -full";   // Contra: Evolution (contra.bat)
    return L"";
}
void RunOne(const std::wstring& dir, const std::wstring& file, const std::wstring& argsIn) {
    std::wstring args = argsIn.empty() ? DefaultArgs(file) : argsIn;
    std::wstring path = dir + L"\\" + file;
    char name[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, name, MAX_PATH, nullptr, nullptr);
    arcade::Log("=== %s", name);
    auto bytes = ReadAll(path);
    if (bytes.empty()) {
        arcade::Log("could not read it");
        return;
    }
    if (void* cd = arcade::FindNetFsShim("kernel32.dll", "SetCurrentDirectoryW"))   // (a game on the NAS: its folder)
        ((BOOL(WINAPI*)(LPCWSTR))cd)(dir.c_str());
    else
        SetCurrentDirectoryW(dir.c_str());
    arcade::SetProfilesFolder(g_pkg + L"\\tpprofiles");   // TeknoParrot game profiles (controls)
    arcade::ChooseProfile(path);       // the game's arcade hardware stand-ins (by its file)
    arcade::SetProgramFolder32(dir);   // the program's own 32-bit DLLs are beside it
    arcade::SetOurFolder32(g_pkg + L"\\x86");   // ours (d3d8to9, DirectInput/DirectSound stand-ins) first
    arcade::AddDllFolder64(dir);       // and its 64-bit ones; then ours (DXVK's d3d9.dll)
    arcade::AddDllFolder64(g_pkg + L"\\x64");
    {
        auto* img32 = new arcade::LoadedImage32;   // a 32-bit program: through the 32-bit bridge, on this thread
        if (arcade::MapProgram32(bytes, *img32)) {
            arcade::SetProgram(nullptr, path, args);   // its path and command line (GetModuleFileName, GetCommandLine)
            arcade::Setup32ForGame(path);
            Run32Guarded(img32);
            return;
        }
        delete img32;
    }
    auto* img = new arcade::LoadedImage;   // kept: the program's memory stays mapped
    arcade::ChooseProfile(path);
    if (!arcade::MapProgram(bytes, *img)) return;
    arcade::ApplyProfilePatches(img->base, img->size, path);
    arcade::SetProgram(img, path, args);
    RunGuarded(img);
}
void RunFolder(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExFromAppW((dir + L"\\*.exe").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!g_only.empty()) {
            std::wstring low = fd.cFileName;
            for (auto& c : low) c = towlower(c);
            if (low.find(g_only) == std::wstring::npos) continue;
        }
        RunOne(dir, fd.cFileName, L"from-xbox");
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// the Xbox's pads (Windows.Gaming.Input; XInput sees none from this app on the console) as XInput states
static bool ReadPad(unsigned index, void* out) {
    using namespace Windows::Gaming::Input;
    auto pads = Gamepad::Gamepads();
    if (index >= pads.Size()) return false;
    GamepadReading g = pads.GetAt(index).GetCurrentReading();
    auto b = (uint32_t)g.Buttons;
    WORD w = 0;
    if (b & (uint32_t)GamepadButtons::DPadUp) w |= XINPUT_GAMEPAD_DPAD_UP;
    if (b & (uint32_t)GamepadButtons::DPadDown) w |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (b & (uint32_t)GamepadButtons::DPadLeft) w |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (b & (uint32_t)GamepadButtons::DPadRight) w |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (b & (uint32_t)GamepadButtons::Menu) w |= XINPUT_GAMEPAD_START;
    if (b & (uint32_t)GamepadButtons::View) w |= XINPUT_GAMEPAD_BACK;
    if (b & (uint32_t)GamepadButtons::LeftThumbstick) w |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (b & (uint32_t)GamepadButtons::RightThumbstick) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (b & (uint32_t)GamepadButtons::LeftShoulder) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (b & (uint32_t)GamepadButtons::RightShoulder) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (b & (uint32_t)GamepadButtons::A) w |= XINPUT_GAMEPAD_A;
    if (b & (uint32_t)GamepadButtons::B) w |= XINPUT_GAMEPAD_B;
    if (b & (uint32_t)GamepadButtons::X) w |= XINPUT_GAMEPAD_X;
    if (b & (uint32_t)GamepadButtons::Y) w |= XINPUT_GAMEPAD_Y;
    auto axis = [](double v) { return (SHORT)(v >= 1.0 ? 32767 : v <= -1.0 ? -32768 : v * 32767.0); };
    XINPUT_STATE* s = (XINPUT_STATE*)out;
    static DWORD packet = 0;
    s->dwPacketNumber = ++packet;
    s->Gamepad.wButtons = w;
    s->Gamepad.bLeftTrigger = (BYTE)(g.LeftTrigger * 255.0);
    s->Gamepad.bRightTrigger = (BYTE)(g.RightTrigger * 255.0);
    s->Gamepad.sThumbLX = axis(g.LeftThumbstickX);
    s->Gamepad.sThumbLY = axis(g.LeftThumbstickY);
    s->Gamepad.sThumbRX = axis(g.RightThumbstickX);
    s->Gamepad.sThumbRY = axis(g.RightThumbstickY);
    return true;
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const& view) {
        view.Activated([](auto&&, Windows::ApplicationModel::Activation::IActivatedEventArgs const& a) {
            using namespace Windows::ApplicationModel::Activation;
            if (a.Kind() == ActivationKind::Protocol) {
                auto uri = a.as<ProtocolActivatedEventArgs>().Uri();
                try {
                    auto q = Windows::Foundation::WwwFormUrlDecoder(uri.Query());
                    for (auto const& e : q) {
                        if (e.Name() == L"cmd") g_cmd = Unquote(e.Value().c_str());
                        if (e.Name() == L"launchOnExit") g_onExit = e.Value().c_str();
                        if (e.Name() == L"root") g_root = Unquote(e.Value().c_str());
                        if (e.Name() == L"netcred") g_cred = Unquote(e.Value().c_str());
                    }
                } catch (...) {
                }
            }
            g_activated = true;
            CoreWindow::GetForCurrentThread().Activate();
        });
    }
    void Load(hstring const&) {}
    void Uninitialize() {}
    void SetWindow(CoreWindow const& w) {
        auto b = w.Bounds();
        // the swap chain is stretched to the whole screen whatever its size
        arcade::SetCoreWindow(reinterpret_cast<::IUnknown*>(winrt::get_abi(w)), 1920, 1080);
        arcade::Log("screen %.0fx%.0f", b.Width, b.Height);
        // B on an Xbox pad is the system's "back": unhandled, it closes the app. The game gets B as a button
        SystemNavigationManager::GetForCurrentView().BackRequested(
            [](auto&&, BackRequestedEventArgs const& e) { e.Handled(true); });
    }
    void Run() {
        std::wstring local = Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
        g_logPath = local + L"\\arcade_log.txt";
        DeleteFileFromAppW(g_logPath.c_str());
        arcade::SetLog(LogOut);
        {
            auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
            arcade::Log("Game Vault arcade loader test %u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision);
        }
        std::thread([] {   // (testing) memory use every 5 s while a game runs: the app's limit covers graphics too
            uint64_t peak = 0;
            for (;;) {
                Sleep(5000);
                uint64_t now = Windows::System::MemoryManager::AppMemoryUsage();
                if (now > peak) peak = now;
                arcade::Log("memory: %llu MB in use, peak %llu MB (game allocator holds %lld MB, peak %lld; files mapped %lld MB)", (unsigned long long)(now >> 20),
                            (unsigned long long)(peak >> 20), (long long)(arcade::TbbHeldBytes() >> 20), (long long)(arcade::TbbPeakBytes() >> 20), (long long)(arcade::FileBackedBytes() >> 20));
            }
        }).detach();
        {   // Direct3D 12 (Dozen, under DXVK) needs app type Game on an Xbox; an App gets about 1 GB
            uint64_t limit = Windows::System::MemoryManager::AppMemoryUsageLimit();
            arcade::Log("memory limit %llu MB (%s)", (unsigned long long)(limit >> 20), limit >= (2048ull << 20) ? "Game" : "App");
            // (testing) memtally.txt in LocalState: memory counted by module, logged every 10 s
            if (!ReadAll(local + L"/memtally.txt").empty()) {
                arcade::SetMemTally(true);
                arcade::Log("memory tally on (memtally.txt)");
            }
            // (experiment) debugmode.txt in LocalState: ask Windows to put this package in debugging mode, where
            // Microsoft says the 1 GB / 5 GB limits do not apply (takes effect at the next start)
            if (!ReadAll(local + L"\\debugmode.txt").empty()) {
                static const GUID clsid = {0xB1AEC16F, 0x2383, 0x4852, {0xB0, 0xE9, 0x8F, 0x0B, 0x1D, 0xC6, 0x6B, 0x4D}};
                static const GUID iid = {0xF27C3930, 0x8029, 0x4AD1, {0x94, 0xE3, 0x3D, 0xBA, 0x41, 0x78, 0x10, 0xC1}};
                MULTI_QI qi = {&iid, nullptr, 0};
                HRESULT hr = CoCreateInstanceFromApp(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER, nullptr, 1, &qi);
                arcade::Log("debugging mode: PackageDebugSettings 0x%08lx / 0x%08lx", (unsigned long)hr, (unsigned long)qi.hr);
                if (SUCCEEDED(hr) && SUCCEEDED(qi.hr) && qi.pItf) {
                    std::wstring full = Windows::ApplicationModel::Package::Current().Id().FullName().c_str();
                    typedef HRESULT(STDMETHODCALLTYPE * enable_t)(void*, LPCWSTR, LPCWSTR, LPWSTR);
                    HRESULT e = ((enable_t)(*(void***)qi.pItf)[3])(qi.pItf, full.c_str(), nullptr, nullptr);
                    arcade::Log("debugging mode: EnableDebugging(%ls) 0x%08lx", full.c_str(), (unsigned long)e);
                    qi.pItf->Release();
                }
            }
        }
        // Mesa's own messages (Dozen) into LocalState\mesa_log.txt
        _wputenv_s(L"MESA_LOG", L"file");
        _wputenv_s(L"DZN_LOG_ERRORS", L"1");   // (testing) the reason of each Vulkan error
        {   // (testing) stderr into LocalState\stderr.txt: Mesa's DZN_DEBUG prints go there (one C runtime for all)
            FILE* f = nullptr;
            _wfreopen_s(&f, (local + L"/stderr.txt").c_str(), L"w", stderr);
            if (f) setvbuf(stderr, nullptr, _IONBF, 0);
        }
        {   // (testing) dzn-debug.txt in LocalState: Mesa Dozen's DZN_DEBUG flags (nir, dxil, ...)
            auto flags = ReadAll(local + L"/dzn-debug.txt");
            std::wstring w;
            for (uint8_t c : flags)
                if (c > ' ') w += (wchar_t)c;
            if (!w.empty()) {
                _wputenv_s(L"DZN_DEBUG", w.c_str());
                arcade::Log("DZN_DEBUG=%ls (dzn-debug.txt)", w.c_str());
            }
        }
        _wputenv_s(L"MESA_LOG_FILE", (local + L"\\mesa_log.txt").c_str());
        {
            // keeps xinputuap.dll in the process for the games' XInput calls (peload.cpp RealDllFor)
            XINPUT_STATE st = {};
            DWORD r = XInputGetState(0, &st);
            arcade::Log("XInput pad 1: %s", r == ERROR_SUCCESS ? "connected" : "not connected");
            // the pads come from Windows.Gaming.Input (the list fills as pads are seen)
            Windows::Gaming::Input::Gamepad::GamepadAdded([](auto&&, auto&&) {
                arcade::Log("pad added (%u in all)", Windows::Gaming::Input::Gamepad::Gamepads().Size());
            });
            arcade::SetPadReader(ReadPad);
            if (GetFileAttributesW((local + L"\\jvsprobe.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {   // (testing)
                _wputenv_s(L"ARCADE_JVS_PROBE", L"1");
                arcade::Log("JVS probe: A/B/X = whole player byte 1/2/3, Y = system byte");
            }
        }
        arcade::InstallDxgiHooks();
        arcade::SetShotPath(local + L"\\frame.bmp");
        // trace.txt in LocalState: every import call is logged the first times (finding what ends a game)
        {
            HANDLE t = CreateFile2FromAppW((local + L"\\trace.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            if (t != INVALID_HANDLE_VALUE) {
                CloseHandle(t);
                auto tr = ReadAll(local + L"\\trace.txt");
                if (std::string(tr.begin(), tr.end()).find("all") != std::string::npos)   // every call, not only the first ones
                    SetEnvironmentVariableA("ARCADE_TRACE_ALL", "1");
                arcade::SetTrace(true);
                arcade::Log("tracing import calls (trace.txt)");
            }
        }
        {   // sample.txt in LocalState: the 32-bit threads' places every 5 s (where a game waits)
            HANDLE sm = CreateFile2FromAppW((local + L"\\sample.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            if (sm != INVALID_HANDLE_VALUE) {
                CloseHandle(sm);
                arcade::g_sample32 = true;
            }
        }
        std::wstring pkg = Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
        g_pkg = pkg;
        {
            auto only = ReadAll(local + L"\\only.txt");
            for (uint8_t c : only)
                if (c > ' ') g_only += (wchar_t)towlower(c);
            if (!g_only.empty()) arcade::Log("only programs named *%ls* (only.txt)", g_only.c_str());
        }
        // for programs and DXVK: where to write (LocalState); DXVK's log and its pipeline cache go there
        SetEnvironmentVariableW(L"ARCADE_LOCAL", local.c_str());
        SetEnvironmentVariableW(L"DXVK_LOG_PATH", local.c_str());
        SetEnvironmentVariableW(L"DXVK_STATE_CACHE_PATH", local.c_str());
        SetEnvironmentVariableW(L"DXVK_LOG_LEVEL", L"info");
        // DXVK's low-memory mode (our DXVK): graphics and the game share the app's ~5 GB on an Xbox
        SetEnvironmentVariableW(L"DXVK_GV_LOWMEM_MB", L"1024");
        {   // testing: shaders.txt in LocalState dumps DXVK's shaders (SPIR-V) into LocalState\shaders
            auto want = ReadAll(local + L"\\shaders.txt");
            if (!want.empty()) {
                CreateDirectoryFromAppW((local + L"\\shaders").c_str(), nullptr);
                SetEnvironmentVariableW(L"DXVK_SHADER_DUMP_PATH", (local + L"\\shaders").c_str());
                arcade::Log("DXVK shaders are dumped to LocalState\\shaders (shaders.txt)");
            }
        }
        {   // the activation (Game Vault's link) comes with the first window events
            auto disp = CoreWindow::GetForCurrentThread().Dispatcher();
            for (int i = 0; i < 300 && !g_activated; i++) {
                disp.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                Sleep(10);
            }
        }
        if (!g_cmd.empty()) arcade::Log("started by a link: %ls", g_cmd.c_str());
        std::thread([pkg, local] {
            if (!g_cmd.empty()) {   // Game Vault's game; then back to Game Vault
                std::wstring w = g_cmd;
                if (!_wcsnicmp(w.c_str(), L"smb://", 6)) {   // played straight from the NAS (netfs.cpp)
                    std::wstring exe;
                    std::string err;
                    if (arcade::NetFsMount(w, g_root, g_cred, local, exe, err)) w = exe;
                    else arcade::Log("NAS: %s", err.c_str());
                }
                for (auto& c : w)
                    if (c == L'/') c = L'\\';
                size_t k = w.find_last_of(L'\\');
                RunOne(k == std::wstring::npos ? local : w.substr(0, k), k == std::wstring::npos ? w : w.substr(k + 1), L"");
                arcade::Log("the game ended: back to %ls", g_onExit.empty() ? L"(nothing)" : g_onExit.c_str());
                if (!g_onExit.empty()) {
                    try {
                        Windows::System::Launcher::LaunchUriAsync(Windows::Foundation::Uri(g_onExit)).get();
                    } catch (...) {
                    }
                }
                Sleep(500);
                CoreApplication::Exit();
                // the game's own threads can keep the app alive (then the console cannot update it for this user):
                // gone for sure
                Sleep(1500);
                TerminateProcess(GetCurrentProcess(), 0);
                return;
            }
            // LocalState\run.txt: one program in a game's folder (path under LocalState; arguments on line 2)
            auto run = ReadAll(local + L"\\run.txt");
            if (!run.empty()) {
                std::string text(run.begin(), run.end());
                size_t nl = text.find_first_of("\r\n");
                std::string rel = text.substr(0, nl), args;
                if (nl != std::string::npos) {
                    size_t a = text.find_first_not_of("\r\n", nl);
                    if (a != std::string::npos) args = text.substr(a, text.find_first_of("\r\n", a) - a);
                }
                std::wstring w(rel.begin(), rel.end()), wa(args.begin(), args.end());
                for (auto& c : w)
                    if (c == L'/') c = L'\\';
                size_t k = w.find_last_of(L'\\');
                std::wstring dir = local + (k == std::wstring::npos ? L"" : L"\\" + w.substr(0, k));
                RunOne(dir, k == std::wstring::npos ? w : w.substr(k + 1), wa);
            } else {
                RunFolder(pkg + L"\\programs");
                RunFolder(local + L"\\programs");
            }
            arcade::Log("DONE");
            if (!run.empty()) {   // one program run: the app closes (an app left open blocks its next install)
                Sleep(500);
                TerminateProcess(GetCurrentProcess(), 0);
            }
        }).detach();   // the UI thread goes on to handle window events: else the splash screen never goes away and
                       // covers everything the game draws
        CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    }
};
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    init_apartment();
    CoreApplication::Run(make<App>());
    return 0;
}
