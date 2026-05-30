// hw_protected_target.cpp - simulator window that matches Citrix's protection stack as closely as possible:
//   1. WS_EX_NOREDIRECTIONBITMAP window style (no DWM redirection bitmap, swap-chain only)
//   2. D3D11 swap chain with DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED (=1024)
//   3. SetWindowDisplayAffinity(hwnd, WDA_MONITOR=0x1) after window is shown
//   4. Present loop at ~60 FPS rendering a magenta-with-green-border test pattern
//
// If the frame_grabber hook captures the magenta content from this target,
// we have empirical confidence that Citrix's session window (same stack) is also capturable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <stdio.h>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static HWND  g_hwnd      = nullptr;
static ComPtr<ID3D11Device>        g_device;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<IDXGISwapChain1>     g_swap;
static ComPtr<ID3D11RenderTargetView> g_rtv;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static bool CreateSwapChain(int w, int h, UINT flags) {
    D3D_FEATURE_LEVEL got;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
        g_device.GetAddressOf(), &got, g_ctx.GetAddressOf());
    if (FAILED(hr)) { wprintf(L"[!] D3D11CreateDevice 0x%lx\n", hr); return false; }

    ComPtr<IDXGIDevice> dxgi;
    g_device.As(&dxgi);
    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = flags;  // DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED = 1024

    hr = factory->CreateSwapChainForHwnd(g_device.Get(), g_hwnd, &desc, nullptr, nullptr, g_swap.GetAddressOf());
    if (FAILED(hr)) {
        wprintf(L"[!] CreateSwapChainForHwnd with flags=0x%x failed: 0x%lx\n", flags, hr);
        return false;
    }
    wprintf(L"[+] swap chain created, flags=0x%x (HW_PROTECTED=%d)\n", flags, (flags & 1024) ? 1 : 0);

    ComPtr<ID3D11Texture2D> back;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back.Get(), nullptr, g_rtv.GetAddressOf());
    return true;
}

static void RenderFrame(float t) {
    // Magenta fill (R=1, G=0, B=1, A=1) - matches our target_wda magenta for visual recognition.
    // Brightness oscillates so we can see frame updates aren't frozen.
    float v = 0.7f + 0.3f * (float)sin(t * 2.0);
    if (v < 0) v = -v; if (v > 1) v = 1;
    float color[4] = { v, 0.0f, v, 1.0f };
    g_ctx->ClearRenderTargetView(g_rtv.Get(), color);
    g_swap->Present(1, 0);
}

int wmain(int argc, wchar_t** argv) {
    bool useHwProtected = true;
    int  W = 1280, H = 720;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--no-hw-protected")) useHwProtected = false;
        else if (!wcscmp(argv[i], L"--size") && i + 2 < argc) {
            W = _wtoi(argv[++i]); H = _wtoi(argv[++i]);
        }
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HwProtectedTarget";
    RegisterClassExW(&wc);

    // WS_EX_NOREDIRECTIONBITMAP = 0x00200000 - skips DWM redirection bitmap, uses swap chain output only.
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;
    g_hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"HW_PROTECTED_TARGET",
        WS_OVERLAPPEDWINDOW, 200, 200, W, H, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { wprintf(L"[!] CreateWindowExW failed: %lu\n", GetLastError()); return 1; }
    wprintf(L"[+] window created hwnd=0x%p style=0x%lx\n", g_hwnd, exStyle);

    UINT flags = useHwProtected ? DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED : 0;
    if (!CreateSwapChain(W, H, flags)) {
        wprintf(L"[!] swap chain creation failed - retrying without HW_PROTECTED\n");
        if (!CreateSwapChain(W, H, 0)) { wprintf(L"[!] even plain swap chain failed\n"); return 2; }
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // After Show, apply WDA_MONITOR - matches Citrix's behavior
    BOOL aff = SetWindowDisplayAffinity(g_hwnd, WDA_MONITOR);
    wprintf(L"[+] SetWindowDisplayAffinity(WDA_MONITOR=0x1) = %d\n", aff);
    DWORD curAff = 0;
    GetWindowDisplayAffinity(g_hwnd, &curAff);
    wprintf(L"[+] confirmed affinity = 0x%lx\n", curAff);

    wprintf(L"[*] entering Present loop (Ctrl+C to exit)\n");
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        } else {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            float t = (float)(now.QuadPart - start.QuadPart) / (float)freq.QuadPart;
            RenderFrame(t);
        }
    }
    return 0;
}
