# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# disasm_at.py <exe/dll> <rva hex> [before] [after]: disassembles around an address (crash offsets from the loader's log),
# naming import calls (call [rip+x] into the import table)
import sys, pefile, capstone

pe = pefile.PE(sys.argv[1], fast_load=False)
rva = int(sys.argv[2], 16)
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x30
imports = {}
for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    for i in d.imports:
        imports[i.address - pe.OPTIONAL_HEADER.ImageBase] = "%s!%s" % (d.dll.decode(), (i.name or b"#%d" % i.ordinal).decode())
start = rva - before
data = pe.get_memory_mapped_image()[start:rva + after]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
for ins in md.disasm(data, start):
    note = ""
    if "rip" in ins.op_str and ins.disp:
        tgt = ins.address + ins.size + ins.disp
        note = "  ; [%x] %s" % (tgt, imports.get(tgt, ""))
    print("%s %8x  %-8s %s%s" % (">>" if ins.address == rva else "  ", ins.address, ins.mnemonic, ins.op_str, note))
