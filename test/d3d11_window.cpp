// test program like a PC arcade game: a window + Direct3D 11 swap chain, clears to changing colours for ~4 s
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ArcadeTest";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"ArcadeTest", L"Arcade test", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 1280;
    sd.BufferDesc.Height = 720;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    char buf[128];
    sprintf_s(buf, "D3D11CreateDeviceAndSwapChain: 0x%08lx", (unsigned long)hr);
    OutputDebugStringA(buf);
    if (FAILED(hr)) return 1;
    ID3D11Texture2D* back = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    DWORD start = GetTickCount();
    int frames = 0;
    MSG msg;
    while (GetTickCount() - start < 4000) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        float t = (GetTickCount() - start) / 4000.0f;
        float c[4] = {t, 0.2f, 1.0f - t, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, c);
        hr = sc->Present(1, 0);
        if (FAILED(hr)) { sprintf_s(buf, "Present failed 0x%08lx", (unsigned long)hr); OutputDebugStringA(buf); break; }
        frames++;
    }
    sprintf_s(buf, "drew %d frames", frames);
    OutputDebugStringA(buf);
    return 0;
}
