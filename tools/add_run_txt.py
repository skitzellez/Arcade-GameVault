# one-off edit: uwp_main runs one program from a game's folder named in LocalState\run.txt
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "uwp_main.cpp"
s = p.read_text(encoding="utf-8")
old = s[s.index('void RunFolder(const std::wstring& dir) {'):s.index('struct App : implements')]
new = r'''// runs one program: its folder is where its own DLLs are and the current folder (games read data relative to it)
void RunOne(const std::wstring& dir, const std::wstring& file, const std::wstring& args) {
    std::wstring path = dir + L"\\" + file;
    char name[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, name, MAX_PATH, nullptr, nullptr);
    arcade::Log("=== %s", name);
    auto bytes = ReadAll(path);
    if (bytes.empty()) {
        arcade::Log("could not read it");
        return;
    }
    SetCurrentDirectoryW(dir.c_str());
    arcade::SetProgramFolder32(dir);   // the program's own 32-bit DLLs are beside it
    arcade::AddDllFolder64(dir);       // and its 64-bit ones; then ours (DXVK's d3d9.dll)
    arcade::AddDllFolder64(g_pkg + L"\\x64");
    {
        auto* img32 = new arcade::LoadedImage32;   // a 32-bit program: through the 32-bit bridge, on this thread
        if (arcade::MapProgram32(bytes, *img32)) {
            Run32Guarded(img32);
            return;
        }
        delete img32;
    }
    auto* img = new arcade::LoadedImage;   // kept: the program's memory stays mapped
    arcade::ChooseProfile(path);
    if (!arcade::MapProgram(bytes, *img)) return;
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

'''
s = s.replace(old, new, 1)
a = r'''            RunFolder(pkg + L"\\programs");
            RunFolder(local + L"\\programs");'''
assert a in s, "runfolder calls"
s = s.replace(a, r'''            // LocalState\run.txt: one program in a game's folder (path under LocalState; arguments on line 2)
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
            }''', 1)
p.write_text(s, encoding="utf-8")
print("ok")
