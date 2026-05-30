// frame_grabber.cpp - Hook IddCxSwapChainReleaseAndAcquireBuffer in WUDFHost
// to dump the first N IDXGIResource frames DWM hands to the IDD.
//
// Hypothesis under test: DWM forwards WDA_MONITOR-protected window pixels to
// IDD swap chains (the per-monitor display path), even though it filters them
// out of the capture path (BitBlt, DXGI OD, WGC). If true, frames written by
// this hook will contain the WDA window's real content.
//
// We use MinHook for the hot-path hook (the IddCx watchdog kills us if a
// single frame takes >10s, so the hook must return fast).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "minhook/include/MinHook.h"

// IddCx output buffer layout (IDARG_OUT_RELEASEANDACQUIREBUFFER).
// We mirror only the prefix we care about - pSurface at the very start of MetaData.
// Full definition is in iddcx.h; we only touch the first pointer.
struct IDDCX_METADATA_PREFIX {
    IDXGIResource* pSurface;
    // ... (other fields we don't touch)
};

// IDARG_OUT_RELEASEANDACQUIREBUFFER starts with the metadata struct.
// (Both v1 and v2 have MetaData at offset 0 in the output struct.)

// The Impl functions take a hidden first parameter: WDF DriverGlobals.
// Real signatures (per IddCx headers + observed in PDB):
//   v1: (PIDD_DRIVER_GLOBALS, IDDCX_SWAPCHAIN, PIDARG_OUT_RELEASEANDACQUIREBUFFER)
//   v2: (PIDD_DRIVER_GLOBALS, IDDCX_SWAPCHAIN, PIDARG_IN_RELEASEANDACQUIREBUFFER2, PIDARG_OUT_RELEASEANDACQUIREBUFFER2)
typedef HRESULT (WINAPI *PFN_IddCxSwapChainReleaseAndAcquireBuffer)(
    void* DriverGlobals,
    void* hSwapChain,
    void* pOut);
typedef HRESULT (WINAPI *PFN_IddCxSwapChainReleaseAndAcquireBuffer2)(
    void* DriverGlobals,
    void* hSwapChain,
    void* pIn,
    void* pOut);

static PFN_IddCxSwapChainReleaseAndAcquireBuffer  g_origV1 = nullptr;
static PFN_IddCxSwapChainReleaseAndAcquireBuffer2 g_origV2 = nullptr;
static volatile LONG g_frameCount = 0;
static const LONG kMaxFrames = 32;  // ring-buffer size: slot = idx % kMaxFrames
// We never stop capturing; we just overwrite the oldest slot. This way the
// caller can run multiple test scenarios in one injection session.

static void DumpStatus(const char* msg);  // fwd decl

// If cropX/W/Y/H are all zero, dumps the full surface. Otherwise dumps only the sub-rect.
static void DumpFrame(IDXGIResource* pSurface, LONG frameIdx,
                       int cropX = 0, int cropY = 0, int cropW = 0, int cropH = 0) {
    if (!pSurface) return;

    ID3D11Texture2D* srcTex = nullptr;
    if (FAILED(pSurface->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex)) || !srcTex) {
        return;
    }

    ID3D11Device* device = nullptr;
    srcTex->GetDevice(&device);
    if (!device) { srcTex->Release(); return; }

    D3D11_TEXTURE2D_DESC desc{};
    srcTex->GetDesc(&desc);

    // Clamp crop to surface bounds; if nothing to crop, take whole surface.
    bool doCrop = (cropW > 0 && cropH > 0);
    if (doCrop) {
        if (cropX < 0) { cropW += cropX; cropX = 0; }
        if (cropY < 0) { cropH += cropY; cropY = 0; }
        if ((UINT)(cropX + cropW) > desc.Width)  cropW = (int)desc.Width  - cropX;
        if ((UINT)(cropY + cropH) > desc.Height) cropH = (int)desc.Height - cropY;
        if (cropW <= 0 || cropH <= 0) { srcTex->Release(); device->Release(); return; }
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* stagingTex = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr) || !stagingTex) {
        device->Release();
        srcTex->Release();
        return;
    }

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex, srcTex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        CreateDirectoryA("C:\\VirtualDisplayDriver", NULL);
        CreateDirectoryA("C:\\VirtualDisplayDriver\\wda_frames", NULL);
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE,
            "C:\\VirtualDisplayDriver\\wda_frames\\frame_%02d.bgra", (int)frameIdx);
        HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            UINT32 outW = doCrop ? (UINT32)cropW : desc.Width;
            UINT32 outH = doCrop ? (UINT32)cropH : desc.Height;
            UINT32 outStride = outW * 4;  // 4 bytes per BGRA pixel - tightly packed in output
            UINT32 header[4] = { outW, outH, outStride, (UINT32)desc.Format };
            DWORD written = 0;
            WriteFile(hFile, header, sizeof(header), &written, NULL);
            if (doCrop) {
                // Write only the cropped sub-rect, tightly packed
                BYTE* src = (BYTE*)mapped.pData;
                for (int y = 0; y < cropH; y++) {
                    BYTE* row = src + (cropY + y) * mapped.RowPitch + cropX * 4;
                    WriteFile(hFile, row, outStride, &written, NULL);
                }
            } else {
                // No crop: write whole surface row-by-row (so output stride matches width*4)
                BYTE* src = (BYTE*)mapped.pData;
                for (UINT y = 0; y < desc.Height; y++) {
                    WriteFile(hFile, src + y * mapped.RowPitch, desc.Width * 4, &written, NULL);
                }
            }
            CloseHandle(hFile);
        }
        ctx->Unmap(stagingTex, 0);
    }
    ctx->Release();
    stagingTex->Release();
    device->Release();
    srcTex->Release();
}

static HRESULT WINAPI HookedV1(void* DriverGlobals, void* hSwapChain, void* pOut) {
    // PASSTHROUGH ONLY for now - log first 4 calls then stop
    LONG cur = g_frameCount;
    if (cur < kMaxFrames) {
        LONG idx = InterlockedIncrement(&g_frameCount) - 1;
        if (idx < kMaxFrames) {
            char buf[160];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v1 called idx=%ld globals=%p hsc=%p pOut=%p", idx, DriverGlobals, hSwapChain, pOut);
            DumpStatus(buf);
        }
    }
    return g_origV1(DriverGlobals, hSwapChain, pOut);
}

static const int kPSurfaceOffset = 32;

// Static crop config from C:\VirtualDisplayDriver\symbols\capture_config.txt:
//   crop_x=<int>  crop_y=<int>  crop_w=<int>  crop_h=<int>
// All-zero = capture full surface. Caller-side script computes these coords
// (taking DPI into account) and writes the file before injection.
struct CropConfig { int x, y, w, h; };
static CropConfig g_cfg = { 0, 0, 0, 0 };
static volatile LONG g_cfgLoaded = 0;

static void LoadCropConfig() {
    if (InterlockedCompareExchange(&g_cfgLoaded, 1, 0) != 0) return;
    HANDLE h = CreateFileA("C:\\VirtualDisplayDriver\\symbols\\capture_config.txt",
        GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1024] = {0};
    DWORD r = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &r, NULL);
    CloseHandle(h);
    char* p = buf;
    while (p && *p) {
        char* eol = strchr(p, '\n');
        if (eol) *eol = 0;
        size_t L = strlen(p);
        if (L > 0 && p[L-1] == '\r') p[L-1] = 0;
        if      (strncmp(p, "crop_x=", 7) == 0) g_cfg.x = atoi(p + 7);
        else if (strncmp(p, "crop_y=", 7) == 0) g_cfg.y = atoi(p + 7);
        else if (strncmp(p, "crop_w=", 7) == 0) g_cfg.w = atoi(p + 7);
        else if (strncmp(p, "crop_h=", 7) == 0) g_cfg.h = atoi(p + 7);
        if (!eol) break;
        p = eol + 1;
    }
    char dbg[200];
    _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
        "config: crop=(%d,%d %dx%d)", g_cfg.x, g_cfg.y, g_cfg.w, g_cfg.h);
    DumpStatus(dbg);
}

static HRESULT WINAPI HookedV2(void* DriverGlobals, void* hSwapChain, void* pIn, void* pOut) {
    HRESULT hr = g_origV2(DriverGlobals, hSwapChain, pIn, pOut);
    if (SUCCEEDED(hr) && pOut) {
        LoadCropConfig();
        LONG idx = InterlockedIncrement(&g_frameCount) - 1;
        LONG slot = idx % kMaxFrames;
        IDXGIResource* pSurface = *(IDXGIResource**)((uint8_t*)pOut + kPSurfaceOffset);
        if (pSurface) {
            __try {
                DumpFrame(pSurface, slot, g_cfg.x, g_cfg.y, g_cfg.w, g_cfg.h);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // swallow: we never want to crash the host
            }
        }
        if (idx < kMaxFrames) {
            char buf[200];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "v2 idx=%ld slot=%ld pSurface=%p", idx, slot, pSurface);
            DumpStatus(buf);
        }
    }
    return hr;
}

static void DumpStatus(const char* msg) {
    CreateDirectoryA("C:\\VirtualDisplayDriver", NULL);
    CreateDirectoryA("C:\\VirtualDisplayDriver\\proof", NULL);
    HANDLE hFile = CreateFileA("C:\\VirtualDisplayDriver\\proof\\hook_status.txt",
        GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(hFile, 0, NULL, FILE_END);
        DWORD w = 0;
        WriteFile(hFile, msg, (DWORD)strlen(msg), &w, NULL);
        WriteFile(hFile, "\r\n", 2, &w, NULL);
        CloseHandle(hFile);
    }
}

// Read offsets file: "v1=<decimal>\r\nv2=<decimal>\r\n"
static bool ReadOffsetsFile(uint64_t& v1Off, uint64_t& v2Off) {
    v1Off = 0; v2Off = 0;
    HANDLE h = CreateFileA("C:\\VirtualDisplayDriver\\symbols\\offsets.txt",
        GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[512] = {0};
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, NULL);
    CloseHandle(h);

    char* p = buf;
    while (p && *p) {
        char* eol = strchr(p, '\n');
        if (eol) *eol = 0;
        // trim \r
        size_t L = strlen(p);
        if (L > 0 && p[L-1] == '\r') p[L-1] = 0;
        if (strncmp(p, "v1=", 3) == 0) v1Off = _strtoui64(p + 3, NULL, 10);
        else if (strncmp(p, "v2=", 3) == 0) v2Off = _strtoui64(p + 3, NULL, 10);
        if (!eol) break;
        p = eol + 1;
    }
    return v1Off != 0 || v2Off != 0;
}

static DWORD WINAPI InitHookThread(LPVOID) {
    DumpStatus("hook thread started");

    HMODULE hIddCx = GetModuleHandleW(L"IddCx.dll");
    if (!hIddCx) {
        DumpStatus("IddCx.dll not loaded - giving up");
        return 1;
    }

    uint64_t v1Off = 0, v2Off = 0;
    if (!ReadOffsetsFile(v1Off, v2Off)) {
        DumpStatus("offsets.txt missing or empty - cannot hook");
        return 3;
    }
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "IddCx base=%p v1_off=0x%llx v2_off=0x%llx",
        hIddCx, (unsigned long long)v1Off, (unsigned long long)v2Off);
    DumpStatus(buf);

    if (MH_Initialize() != MH_OK) {
        DumpStatus("MH_Initialize failed");
        return 2;
    }

    LPVOID v1 = v1Off ? (LPVOID)((BYTE*)hIddCx + v1Off) : NULL;
    LPVOID v2 = v2Off ? (LPVOID)((BYTE*)hIddCx + v2Off) : NULL;

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v1_abs=%p v2_abs=%p", v1, v2);
    DumpStatus(buf);

    if (v1) {
        MH_STATUS s = MH_CreateHook(v1, &HookedV1, reinterpret_cast<LPVOID*>(&g_origV1));
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v1 MH_CreateHook=%d", (int)s);
        DumpStatus(buf);
        if (s == MH_OK) {
            s = MH_EnableHook(v1);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v1 MH_EnableHook=%d", (int)s);
            DumpStatus(buf);
        }
    }
    if (v2) {
        MH_STATUS s = MH_CreateHook(v2, &HookedV2, reinterpret_cast<LPVOID*>(&g_origV2));
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v2 MH_CreateHook=%d", (int)s);
        DumpStatus(buf);
        if (s == MH_OK) {
            s = MH_EnableHook(v2);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "v2 MH_EnableHook=%d", (int)s);
            DumpStatus(buf);
        }
    }
    return 0;
}

// Bare-minimum file write directly inside DllMain - if THIS doesn't appear,
// then DllMain itself never ran. Only uses kernel32 APIs.
static void DllMainTrace(const char* tag) {
    CreateDirectoryA("C:\\VirtualDisplayDriver", NULL);
    CreateDirectoryA("C:\\VirtualDisplayDriver\\proof", NULL);
    HANDLE hFile = CreateFileA("C:\\VirtualDisplayDriver\\proof\\dllmain_trace.txt",
        GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(hFile, 0, NULL, FILE_END);
        DWORD w = 0;
        WriteFile(hFile, tag, (DWORD)strlen(tag), &w, NULL);
        WriteFile(hFile, "\r\n", 2, &w, NULL);
        CloseHandle(hFile);
    }
}

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DllMainTrace("DllMain ATTACH");
        DisableThreadLibraryCalls(hMod);
        DllMainTrace("creating InitHookThread");
        HANDLE h = CreateThread(NULL, 0, InitHookThread, NULL, 0, NULL);
        if (h) {
            DllMainTrace("CreateThread OK");
            CloseHandle(h);
        } else {
            DllMainTrace("CreateThread FAILED");
        }
    }
    return TRUE;
}
