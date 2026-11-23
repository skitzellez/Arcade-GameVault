# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# PC testing: prints OutputDebugString messages (D3D12 debug layer, ...) of every process, like DebugView.
# dbwin.py <seconds>   (start it, then run the program within that time)
import ctypes, ctypes.wintypes as w, sys, time

k = ctypes.WinDLL("kernel32", use_last_error=True)
k.CreateEventW.restype = w.HANDLE
k.CreateFileMappingW.restype = w.HANDLE
k.MapViewOfFile.restype = ctypes.c_void_p
k.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]
ready = k.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
data = k.CreateEventW(None, False, False, "DBWIN_DATA_READY")
mapping = k.CreateFileMappingW(w.HANDLE(-1), None, 4, 0, 4096, "DBWIN_BUFFER")
view = k.MapViewOfFile(mapping, 4, 0, 0, 4096)
if not (ready and data and mapping and view):
    sys.exit("DBWIN setup failed %d" % ctypes.get_last_error())
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 60)
k.SetEvent(ready)
while time.time() < end:
    if k.WaitForSingleObject(data, 200) == 0:
        pid = ctypes.c_uint32.from_address(view).value
        msg = ctypes.string_at(view + 4).decode("utf-8", "replace").rstrip()
        print("[%d] %s" % (pid, msg), flush=True)
        k.SetEvent(ready)
