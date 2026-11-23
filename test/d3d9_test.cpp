// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader (TeknoParrot-style), GPL-3.0 like OpenParrot.
// Test program: a 64-bit Direct3D 9 game in miniature. Clears to a moving colour and draws a triangle with the
// fixed-function pipeline for 600 frames; frame 300 is read back and saved as d3d9_frame.bmp in the folder named by
// ARCADE_LOCAL (the loader's LocalState on the Xbox), so the picture can be checked without a screenshot.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cmath>
#pragma comment(lib, "d3d9.lib")

static void Say(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    OutputDebugStringA(b);
    printf("%s\n", b);
}

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
};

static void SaveFrame(IDirect3DDevice9* dev, int w, int h) {
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return Say("d3d9 test: no back buffer");
    D3DSURFACE_DESC d;
    back->GetDesc(&d);
    HRESULT hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(back, copy);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr)) hr = copy->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        Say("d3d9 test: frame read-back failed 0x%08lx", hr);
    } else {
        wchar_t dir[MAX_PATH] = L".";
        GetEnvironmentVariableW(L"ARCADE_LOCAL", dir, MAX_PATH);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\d3d9_frame.bmp", dir);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            ih.biSize = sizeof ih;
            ih.biWidth = d.Width;
            ih.biHeight = -(LONG)d.Height;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
            DWORD n;
            WriteFile(f, &fh, sizeof fh, &n, nullptr);
            WriteFile(f, &ih, sizeof ih, &n, nullptr);
            for (UINT y = 0; y < d.Height; y++) WriteFile(f, (BYTE*)lr.pBits + y * lr.Pitch, d.Width * 4, &n, nullptr);
            CloseHandle(f);
            Say("d3d9 test: frame saved (%ux%u, format %d)", d.Width, d.Height, (int)d.Format);
        } else {
            Say("d3d9 test: frame file not written (error %lu)", GetLastError());
        }
        copy->UnlockRect();
    }
    if (copy) copy->Release();
    back->Release();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    const int W = 1280, H = 720;
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"d3d9test";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, L"d3d9test", L"D3D9 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, nullptr,
                               nullptr, inst, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        Say("d3d9 test: Direct3DCreate9 failed");
        return 1;
    }
    D3DADAPTER_IDENTIFIER9 id = {};
    d3d->GetAdapterIdentifier(0, 0, &id);
    Say("d3d9 test: adapter \"%s\", %u modes", id.Description, d3d->GetAdapterModeCount(0, D3DFMT_X8R8G8B8));
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    Say("d3d9 test: CreateDevice 0x%08lx", hr);
    if (FAILED(hr)) return 2;
    DWORD start = GetTickCount();
    // D3D9TEST=clear: no triangle (finding what a driver refuses)
    char mode[16] = "";
    GetEnvironmentVariableA("D3D9TEST", mode, sizeof mode);
    bool clearOnly = !strcmp(mode, "clear");
    for (int frame = 0; frame < 600; frame++) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        float t = frame / 60.0f;
        BYTE r = (BYTE)(64 + 63 * sinf(t)), g = 32, b = (BYTE)(96 + 63 * cosf(t * 0.7f));
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(r, g, b), 1.0f, 0);
        dev->BeginScene();
        float cx = W / 2.0f, cy = H / 2.0f, s = 250.0f, a = t;
        Vtx v[3];
        for (int i = 0; i < 3; i++) {
            float ang = a + i * 2.0943951f;
            v[i] = {cx + s * cosf(ang), cy + s * sinf(ang), 0.5f, 1.0f,
                    i == 0 ? D3DCOLOR_XRGB(255, 40, 40) : i == 1 ? D3DCOLOR_XRGB(40, 255, 40) : D3DCOLOR_XRGB(40, 80, 255)};
        }
        if (!clearOnly) {
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
        }
        dev->EndScene();
        if (frame == 300) SaveFrame(dev, W, H);
        hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            Say("d3d9 test: Present failed 0x%08lx at frame %d", hr, frame);
            break;
        }
        if (frame == 599) Say("d3d9 test: 600 frames in %lu ms", GetTickCount() - start);
    }
    dev->Release();
    d3d->Release();
    Say("d3d9 test: done");
    return 0;
}
