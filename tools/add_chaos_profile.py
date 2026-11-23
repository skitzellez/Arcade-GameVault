# one-off edit: profiles matched by exe name + CRC-32; Chaos Breaker (Taito Type X, JVS on COM2, Taito stick layout);
# profile chosen before 32-bit programs are mapped too
from pathlib import Path

root = Path(__file__).resolve().parent.parent


def edit(rel, pairs):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert a in s, (rel, a[:60])
        s = s.replace(a, b, 1)
    p.write_text(s, encoding="utf-8")


edit("src/arcade_io.cpp", [
    ('''    bool hasp;                 // HASP dongle stand-in
    bool padAlways;            // XInputGetState: always a connected pad
};''', '''    bool hasp;                 // HASP dongle stand-in
    bool padAlways;            // XInputGetState: always a connected pad
    uint32_t crc;              // CRC-32 of the exe (0: any), for exes with a common name (game.exe)
    bool taitoStick;           // Taito Type X stick panel (TeknoParrot "TaitoStick": 2 players x 2 switch bytes)
};'''),
    ('''     "namco ltd.;NA-JV;Ver4.00;JPN,Multipurpose.", 0x31, 0x31, 0x31, 0x18, true, true},
};''', '''     "namco ltd.;NA-JV;Ver4.00;JPN,Multipurpose.", 0x31, 0x31, 0x31, 0x18, true, true, 0, false},
    // Chaos Breaker (Taito Type X, 2004): JVS on COM2, TeknoParrot's TaitoTypeXGeneric settings
    {L"game.exe", "Chaos Breaker", "COM2", false, "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10", 0x30, 0x10,
     0x13, 0x18, false, false, 0x09616171, true},
};
static uint32_t Crc32File(const std::wstring& path) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
#else
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
#endif
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint32_t crc = 0xFFFFFFFF;
    static uint8_t buf[65536];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n)
        for (DWORD i = 0; i < n; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    CloseHandle(h);
    return ~crc;
}'''),
    ('''    g_profile = nullptr;
    for (const Profile& p : kProfiles)
        if (!_wcsicmp(p.exe, name.c_str())) g_profile = &p;''', '''    g_profile = nullptr;
    uint32_t crc = 0;
    for (const Profile& p : kProfiles) {
        if (_wcsicmp(p.exe, name.c_str())) continue;
        if (p.crc) {
            if (!crc) crc = Crc32File(exePath);
            if (crc != p.crc) continue;
        }
        g_profile = &p;
    }'''),
    # Taito stick: features, switches, 0x70
    ('''    case 0x14: {   // features
        report();''', '''    case 0x14: {   // features
        report();
        if (p.taitoStick) {   // (TeknoParrot JvsGetSlaveFeatures, TaitoStick)
            const uint8_t f[] = {0x01, 0x02, 0x10, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            r.insert(r.end(), f, f + sizeof f);
            return 1;
        }'''),
    ('''        JvsSwitches(left > 1 ? c[1] : 2, left > 2 ? c[2] : 2, r);
        return 3;''', '''        JvsSwitches(left > 1 ? c[1] : 2, p.taitoStick ? 2 : (left > 2 ? c[2] : 2), r);   // Taito: 2 bytes each
        return 3;'''),
    ('''    case 0x70:   // Namco custom
        if (!p.namco || left < 2) break;''', '''    case 0x70:   // Namco custom (Taito: a 2-byte command with no reply)
        if (p.taitoStick) return 2;
        if (!p.namco || left < 2) break;'''),
])
edit("src/uwp_main.cpp", [
    ('''    arcade::SetProgramFolder32(dir);   // the program's own 32-bit DLLs are beside it''',
     '''    arcade::ChooseProfile(path);       // the game's arcade hardware stand-ins (by its file)
    arcade::SetProgramFolder32(dir);   // the program's own 32-bit DLLs are beside it'''),
])
edit("src/pc_main.cpp", [
    ('''    static arcade::LoadedImage32 img32;''', '''    arcade::ChooseProfile(argv[1]);
    static arcade::LoadedImage32 img32;'''),
])
print("ok")
