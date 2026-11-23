# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# memmap.py <process name>: committed private memory of a running process, biggest blocks first (allocation base,
# size), to see where a game's memory goes
import ctypes, ctypes.wintypes as w, sys, collections

k = ctypes.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", w.DWORD),
                ("PartitionId", w.WORD), ("RegionSize", ctypes.c_size_t), ("State", w.DWORD), ("Protect", w.DWORD),
                ("Type", w.DWORD)]


import subprocess
pid = int(subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-Process %s).Id" % sys.argv[1]],
                         capture_output=True, text=True).stdout.split()[0])
h = k.OpenProcess(0x0410, False, pid)
k.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
addr = 0
blocks = collections.Counter()
total = 0
mbi = MBI()
while k.VirtualQueryEx(h, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
    if mbi.State == 0x1000 and mbi.Type == 0x20000:   # committed, private
        blocks[mbi.AllocationBase or 0] += mbi.RegionSize
        total += mbi.RegionSize
    addr = (mbi.BaseAddress or 0) + mbi.RegionSize
    if addr >= 1 << 47:
        break
print("private committed: %d MB in %d allocations" % (total >> 20, len(blocks)))
sizes = collections.Counter()
for b, s in blocks.items():
    sizes[(s >> 20)] += 1
for b, s in blocks.most_common(15):
    print("  %016x  %6d MB" % (b, s >> 20))
small = sum(s for s in blocks.values() if s < (16 << 20))
print("  in allocations under 16 MB: %d MB" % (small >> 20))
