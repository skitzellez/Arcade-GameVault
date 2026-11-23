# one-off edit: the loader started by Game Vault's link gamevault-arcade:?cmd=<game exe>&launchOnExit=<link>
from pathlib import Path
p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\uwp_main.cpp")
s = p.read_text(encoding="utf-8")
BS = chr(92)
def rep(a, b):
    global s
    assert s.count(a) == 1, a[:60]
    s = s.replace(a, b)
rep("#include <winrt/Windows.Foundation.h>\n", "#include <winrt/Windows.ApplicationModel.Activation.h>\n#include <winrt/Windows.Foundation.h>\n")
rep("std::wstring g_only;   // LocalState", """// Game Vault's link: gamevault-arcade:?cmd=<the game's .exe, whole path>&launchOnExit=<link opened when it ends>
static std::wstring g_cmd, g_onExit;
static std::atomic<bool> g_activated{false};
static std::wstring Unquote(std::wstring v) {
    while (!v.empty() && (v.front() == L'"' || v.front() == L' ')) v.erase(v.begin());
    while (!v.empty() && (v.back() == L'"' || v.back() == L' ')) v.pop_back();
    return v;
}
std::wstring g_only;   // LocalState""")
rep("""        view.Activated([](auto&&, auto&&) { CoreWindow::GetForCurrentThread().Activate(); });""",
"""        view.Activated([](auto&&, Windows::ApplicationModel::Activation::IActivatedEventArgs const& a) {
            using namespace Windows::ApplicationModel::Activation;
            if (a.Kind() == ActivationKind::Protocol) {
                auto uri = a.as<ProtocolActivatedEventArgs>().Uri();
                try {
                    auto q = Windows::Foundation::WwwFormUrlDecoder(uri.Query());
                    for (auto const& e : q) {
                        if (e.Name() == L"cmd") g_cmd = Unquote(e.Value().c_str());
                        if (e.Name() == L"launchOnExit") g_onExit = e.Value().c_str();
                    }
                } catch (...) {
                }
            }
            g_activated = true;
            CoreWindow::GetForCurrentThread().Activate();
        });""")
rep("""        std::thread([pkg, local] {
            // LocalState""", """        {   // the activation (Game Vault's link) comes with the first window events
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
                for (auto& c : w)
                    if (c == L'/') c = L'BSBS';
                size_t k = w.find_last_of(L'BSBS');
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
                return;
            }
            // LocalState""".replace("BSBS", BS + BS))
p.write_text(s, encoding="utf-8")
print("ok")
