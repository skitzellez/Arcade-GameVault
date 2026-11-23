# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
"""A tiny debugger for the PC loader: runs a command under the Windows debug API and prints the first access
violation (or other crash): exception code, address, code segment (0x23 = 32-bit code), registers, and bytes at the
instruction.   python minidbg.py <exe> [args...]"""
import ctypes
import ctypes.wintypes as W
import subprocess
import sys

k = ctypes.windll.kernel32
DEBUG_ONLY_THIS_PROCESS = 2
EXCEPTION_DEBUG_EVENT, EXIT_PROCESS_DEBUG_EVENT, CREATE_PROCESS_DEBUG_EVENT = 1, 5, 3
DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001


class SI(ctypes.Structure):
    _fields_ = [("cb", W.DWORD), ("r", ctypes.c_void_p * 2), ("t", W.LPWSTR), ("x", W.DWORD * 8),
                ("f", W.WORD * 2), ("r2", ctypes.c_void_p), ("h", W.HANDLE * 3)]


class PI(ctypes.Structure):
    _fields_ = [("hProcess", W.HANDLE), ("hThread", W.HANDLE), ("pid", W.DWORD), ("tid", W.DWORD)]


class EXREC(ctypes.Structure):
    _fields_ = [("code", W.DWORD), ("flags", W.DWORD), ("rec", ctypes.c_void_p), ("addr", ctypes.c_void_p),
                ("n", W.DWORD), ("pad", W.DWORD), ("info", ctypes.c_uint64 * 15)]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("code", W.DWORD), ("pid", W.DWORD), ("tid", W.DWORD), ("pad", W.DWORD),
                ("rec", EXREC), ("first", W.DWORD), ("more", ctypes.c_byte * 64)]


# CONTEXT (x64) is 1232 bytes, 16-byte aligned; offsets of what we print
ctx_buf = (ctypes.c_byte * (1232 + 16))()
addr = ctypes.addressof(ctx_buf)
ctx_addr = (addr + 15) & ~15
OFF = {"ContextFlags": 0x30, "SegCs": 0x38, "Rax": 0x78, "Rcx": 0x80, "Rdx": 0x88, "Rbx": 0x90, "Rsp": 0x98,
       "Rbp": 0xA0, "Rsi": 0xA8, "Rdi": 0xB0, "Rip": 0xF8}


def ctx_get(name, size=8):
    return int.from_bytes(ctypes.string_at(ctx_addr + OFF[name], size), "little")


cmd = subprocess.list2cmdline(sys.argv[1:])
si = SI()
si.cb = ctypes.sizeof(SI)
pi = PI()
if not k.CreateProcessW(None, ctypes.create_unicode_buffer(cmd), None, None, False, DEBUG_ONLY_THIS_PROCESS, None, None,
                        ctypes.byref(si), ctypes.byref(pi)):
    sys.exit("CreateProcess failed %d" % k.GetLastError())
ev = DEBUG_EVENT()
k.OpenThread.restype = W.HANDLE
while True:
    k.WaitForDebugEvent(ctypes.byref(ev), 0xFFFFFFFF)
    cont = DBG_CONTINUE
    if ev.code == EXCEPTION_DEBUG_EVENT:
        c = ev.rec.code
        if c in (0x80000003, 0x4000001F, 0x406D1388, 0x40010006, 0x4001000A):   # breakpoints, debug prints
            pass
        elif c == 0xE06D7363:   # C++ exception: let it be handled
            cont = DBG_EXCEPTION_NOT_HANDLED
        else:
            th = k.OpenThread(0x001F03FF, False, ev.tid)
            ctypes.memmove(ctx_addr + OFF["ContextFlags"], (0x0010001F).to_bytes(4, "little"), 4)   # CONTEXT_ALL-ish
            k.GetThreadContext(W.HANDLE(th), ctypes.c_void_p(ctx_addr))
            rip = ctx_get("Rip")
            buf = (ctypes.c_ubyte * 16)()
            got = ctypes.c_size_t()
            k.ReadProcessMemory(pi.hProcess, ctypes.c_void_p(rip), buf, 16, ctypes.byref(got))
            print("EXCEPTION 0x%08x (first chance %d) at %#x cs=%#x" % (c, ev.first, ev.rec.addr or 0, ctx_get("SegCs", 2)))
            if ev.rec.n >= 2:
                print("  access: %s %#x" % ("write" if ev.rec.info[0] == 1 else "read" if ev.rec.info[0] == 0 else "exec",
                                            ev.rec.info[1]))
            print("  " + " ".join("%s=%#x" % (r, ctx_get(r)) for r in ("Rax", "Rcx", "Rdx", "Rbx", "Rsp", "Rbp", "Rsi", "Rdi", "Rip")))
            print("  bytes at rip: " + bytes(buf[:got.value]).hex(" "))
            segs = {n: int.from_bytes(ctypes.string_at(ctx_addr + o, 2), "little") for n, o in
                    (("cs", 0x38), ("ds", 0x3A), ("es", 0x3C), ("fs", 0x3E), ("gs", 0x40), ("ss", 0x42))}
            print("  segments: " + " ".join("%s=%#x" % kv for kv in segs.items()) +
                  "  info: %s" % [hex(ev.rec.info[i]) for i in range(min(ev.rec.n, 3))] + "  thread %d" % ev.tid)
            if ev.rec.n >= 0 and (ctx_get("SegCs", 2) == 0x23):
                esp = ctx_get("Rsp") & 0xFFFFFFFF
                sb = (ctypes.c_uint32 * 8)()
                k.ReadProcessMemory(pi.hProcess, ctypes.c_void_p(esp), sb, 32, ctypes.byref(got))
                print("  32-bit stack: " + " ".join("%08x" % v for v in sb))
            k.TerminateProcess(pi.hProcess, 99)
            cont = DBG_CONTINUE
    elif ev.code == EXIT_PROCESS_DEBUG_EVENT:
        k.ContinueDebugEvent(ev.pid, ev.tid, DBG_CONTINUE)
        break
    k.ContinueDebugEvent(ev.pid, ev.tid, cont)
