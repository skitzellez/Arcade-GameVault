// test program like many PC games: device first, then the swap chain from the DXGI factory, then full screen
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    char buf[160];
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ArcadeTest2";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"ArcadeTest2", L"t", WS_POPUP, 0, 0, 1360, 768, nullptr, nullptr, inst, nullptr);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory* fac = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd);
    xd->GetAdapter(&ad);
    ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac);
    hr = fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1360; sd.BufferDesc.Height = 768;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = FALSE;
    IDXGISwapChain* sc = nullptr;
    hr = fac->CreateSwapChain(dev, &sd, &sc);
    sprintf_s(buf, "factory CreateSwapChain: 0x%08lx", (unsigned long)hr); OutputDebugStringA(buf);
    if (FAILED(hr)) return 1;
    hr = sc->SetFullscreenState(TRUE, nullptr);
    sprintf_s(buf, "SetFullscreenState(TRUE): 0x%08lx", (unsigned long)hr); OutputDebugStringA(buf);
    hr = sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    sprintf_s(buf, "ResizeBuffers: 0x%08lx", (unsigned long)hr); OutputDebugStringA(buf);
    ID3D11Texture2D* back = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    DWORD start = GetTickCount(); int frames = 0;
    while (GetTickCount() - start < 3000) {
        float c[4] = {0.1f, 0.8f, 0.2f, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, c);
        if (FAILED(sc->Present(1, 0))) break;
        frames++;
    }
    sprintf_s(buf, "factory path drew %d frames", frames); OutputDebugStringA(buf);
    return 0;
}
