# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# battlepod_flat.py <SWArcGame.upk in> <out>: Star Wars Battle Pod on a TV instead of the dome. The game's script
# (SWArcDomeGame.PostBeginPlay) asks for the dome view and a 3520x1980 back buffer; this turns the view to NONE
# (flat), the dome back buffer switch off, and the default back buffer to 1280x720. Same size, so it is a byte patch.
import struct, sys

d = bytearray(open(sys.argv[1], "rb").read())
f0, fs = 0xD4BF9, 279   # SWArcDomeGame.PostBeginPlay (decompressed SWArcGame.upk)


def patch(old, new):
    blob = bytes(d[f0:f0 + fs])
    assert blob.count(old) == 1, old.hex()
    i = f0 + blob.find(old)
    d[i:i + len(new)] = new


patch(bytes.fromhex("1be00e0000000000002402"), bytes.fromhex("1be00e0000000000002400"))  # SetDomeViewType(DOME->NONE)
patch(bytes.fromhex("1bd90e00000000000027"), bytes.fromhex("1bd90e00000000000028"))      # SetDomeBackbufferSw(false)
o = 0xD5B56   # Default__SWArcDomeGame.DefaultBackbufferSize (Vector2D)
assert d[o:o + 8] == bytes.fromhex("00005c450080f744")
d[o:o + 8] = struct.pack("<ff", 1280, 720)
open(sys.argv[2], "wb").write(d)
print("ok")
