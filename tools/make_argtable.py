# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
"""Writes src/argtable32.inc: for each function of the 32-bit Windows import libraries (Windows SDK, um/x86), its
DLL, name and the bytes of stack arguments, read from the stdcall names in the libraries (_GetTickCount@0,
_Sleep@4; a name without @N is cdecl). The 32-bit bridge (peload32.cpp) uses it to call the 64-bit function of the
same name with the arguments widened, for functions without a stand-in of their own.
   python make_argtable.py"""
import re
import struct
from pathlib import Path

SDK = Path(r"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x86")
LIBS = ["kernel32.Lib", "User32.Lib", "Gdi32.Lib", "WinMM.Lib", "AdvAPI32.Lib", "Ole32.Lib", "shell32.lib",
        "Version.Lib", "WS2_32.Lib", "dsound.lib", "dinput8.lib", "d3d9.lib", "Xinput9_1_0.lib"]


def short_imports(data):
    """(dll, name, decorated symbol) from the import library's short import members."""
    out = []
    pos = 8   # after "!<arch>\n"
    while pos + 60 <= len(data):
        size = int(data[pos + 48:pos + 58].decode().strip() or 0)
        body = data[pos + 60:pos + 60 + size]
        if len(body) >= 20 and body[0:4] == b"\x00\x00\xff\xff":   # IMPORT_OBJECT_HEADER
            name_type = (struct.unpack_from("<H", body, 18)[0] >> 2) & 7
            strings = body[20:].split(b"\x00")
            sym, dll = strings[0].decode("latin1"), strings[1].decode("latin1")
            out.append((dll, sym, name_type))
        pos += 60 + size + (size & 1)
    return out


rows = {}
for lib in LIBS:
    data = (SDK / lib).read_bytes()
    for dll, sym, name_type in short_imports(data):
        m = re.fullmatch(r"_?([A-Za-z_][\w]*)@(\d+)", sym)
        if m:
            name, nbytes, cdecl = m.group(1), int(m.group(2)), 0
        else:
            name, nbytes, cdecl = sym.lstrip("_"), 0, 1
        key = (dll.lower(), name)
        rows.setdefault(key, (nbytes, cdecl))
lines = ["/* made by tools/make_argtable.py from the Windows SDK's 32-bit import libraries: do not edit */"]
for (dll, name), (nbytes, cdecl) in sorted(rows.items()):
    lines.append('{"%s", "%s", %d, %d},' % (dll, name, nbytes, cdecl))
out = Path(__file__).resolve().parent.parent / "src" / "argtable32.inc"
out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
print(len(rows), "functions")
