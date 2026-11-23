# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# winshot.py <process name> <out.png>: a picture of that process's biggest visible window only (PrintWindow with
# full content), never the rest of the desktop
import ctypes, ctypes.wintypes as w, sys
from PIL import Image

u = ctypes.WinDLL("user32", use_last_error=True)
g = ctypes.WinDLL("gdi32")
k = ctypes.WinDLL("kernel32")
psapi = ctypes.WinDLL("psapi")
u.GetWindowDC.restype = w.HDC
g.CreateCompatibleDC.restype = w.HDC
g.CreateCompatibleBitmap.restype = w.HBITMAP
g.SelectObject.restype = w.HGDIOBJ
g.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
g.CreateCompatibleDC.argtypes = [w.HDC]
g.CreateCompatibleBitmap.argtypes = [w.HDC, ctypes.c_int, ctypes.c_int]
g.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT]
u.GetWindowDC.argtypes = [w.HWND]
u.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
u.GetClientRect.argtypes = [w.HWND, ctypes.c_void_p]
k.OpenProcess.restype = w.HANDLE
want = sys.argv[1].lower()


def proc_name(pid):
    h = k.OpenProcess(0x1000, False, pid)
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(260)
    size = w.DWORD(260)
    k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
    k.CloseHandle(h)
    return buf.value.split("\\")[-1].lower()


best = [None, 0]


@ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
def each(hwnd, _):
    if not u.IsWindowVisible(hwnd):
        return True
    pid = w.DWORD()
    u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if proc_name(pid.value) != want:
        return True
    r = w.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    area = r.right * r.bottom
    if area > best[1]:
        best[0], best[1] = hwnd, area
    return True


u.EnumWindows(each, 0)
if not best[0]:
    sys.exit("no window of " + want)
hwnd = best[0]
r = w.RECT()
u.GetClientRect(hwnd, ctypes.byref(r))
W, H = r.right, r.bottom
wdc = u.GetWindowDC(hwnd)
mdc = g.CreateCompatibleDC(wdc)
bmp = g.CreateCompatibleBitmap(wdc, W, H)
g.SelectObject(mdc, bmp)
ok = u.PrintWindow(hwnd, mdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT


class BIH(ctypes.Structure):
    _fields_ = [("biSize", w.DWORD), ("biWidth", w.LONG), ("biHeight", w.LONG), ("biPlanes", w.WORD),
                ("biBitCount", w.WORD), ("biCompression", w.DWORD), ("biSizeImage", w.DWORD),
                ("biXPelsPerMeter", w.LONG), ("biYPelsPerMeter", w.LONG), ("biClrUsed", w.DWORD),
                ("biClrImportant", w.DWORD)]


bih = BIH(ctypes.sizeof(BIH), W, -H, 1, 32, 0, 0, 0, 0, 0, 0)
buf = ctypes.create_string_buffer(W * H * 4)
g.GetDIBits(mdc, bmp, 0, H, buf, ctypes.byref(bih), 0)
Image.frombuffer("RGB", (W, H), buf, "raw", "BGRX", 0, 1).save(sys.argv[2])
print("window %dx%d, PrintWindow %d" % (W, H, ok))
