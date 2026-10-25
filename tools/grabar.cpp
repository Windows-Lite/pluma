// grabar.exe <seconds> <out folder> [width]
// Records the screen at its refresh rate with Desktop Duplication, on the graphics card that drives the screen (on a
// laptop with two, ffmpeg's ddagrab picks the other and fails), and writes each frame, scaled down, as
// <out>\NNNN-<ms>.png: to see a transition as the user sees it, frame by frame. Waits for any key in the console
// only if asked to start with "espera".
// Build: cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE grabar.cpp
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "user32.lib")

struct Frame { double ms; std::vector<BYTE> px; };
static double now_ms() { static LARGE_INTEGER f, t0; LARGE_INTEGER t; if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); } QueryPerformanceCounter(&t); return (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart; }

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { fprintf(stderr, "grabar <seconds> <out folder> [width]\n"); return 2; }
    double secs = _wtof(argv[1]);
    std::wstring out = argv[2];
    int W = argc > 3 ? _wtoi(argv[3]) : 640;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitialize(NULL);
    IDXGIFactory1 *fac = NULL;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    IDXGIAdapter1 *ad = NULL; IDXGIOutput *outp = NULL;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
        if (SUCCEEDED(ad->EnumOutputs(0, &outp))) break;   // the card the screen hangs from
        ad->Release(); ad = NULL;
    }
    if (!ad || !outp) { fprintf(stderr, "no screen\n"); return 1; }
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    if (FAILED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) { fprintf(stderr, "no device\n"); return 1; }
    IDXGIOutput1 *o1 = NULL; outp->QueryInterface(IID_PPV_ARGS(&o1));
    IDXGIOutputDuplication *dup = NULL;
    HRESULT hr = o1->DuplicateOutput(dev, &dup);
    if (FAILED(hr)) { fprintf(stderr, "DuplicateOutput: 0x%08lx\n", hr); return 1; }
    DXGI_OUTDUPL_DESC dd; dup->GetDesc(&dd);
    int SW = dd.ModeDesc.Width, SH = dd.ModeDesc.Height, H = W * SH / SW;
    ID3D11Texture2D *stage = NULL;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = SW; td.Height = SH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, NULL, &stage);
    std::vector<Frame> frames;
    frames.reserve((size_t)(secs * 160));
    printf("grabando %.1f s de %dx%d...\n", secs, SW, SH);
    fflush(stdout);
    double t0 = now_ms();
    while (now_ms() - t0 < secs * 1000) {
        DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = NULL;
        hr = dup->AcquireNextFrame(50, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;
        ID3D11Texture2D *tex = NULL;
        res->QueryInterface(IID_PPV_ARGS(&tex));
        if (fi.LastPresentTime.QuadPart && tex) {
            ctx->CopyResource(stage, tex);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                Frame f; f.ms = now_ms() - t0; f.px.resize((size_t)W * H * 4);
                for (int y = 0; y < H; y++) {
                    const BYTE *row = (const BYTE *)m.pData + (size_t)(y * SH / H) * m.RowPitch;
                    BYTE *d = f.px.data() + (size_t)y * W * 4;
                    for (int x = 0; x < W; x++) { memcpy(d + x * 4, row + (size_t)(x * SW / W) * 4, 4); d[x * 4 + 3] = 255; }
                }
                ctx->Unmap(stage, 0);
                frames.push_back(std::move(f));
            }
        }
        if (tex) tex->Release();
        res->Release();
        dup->ReleaseFrame();
    }
    printf("%zu fotogramas; escribiendo...\n", frames.size());
    fflush(stdout);
    CreateDirectoryW(out.c_str(), NULL);
    IWICImagingFactory *wic = NULL;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (size_t i = 0; i < frames.size(); i++) {
        wchar_t name[64]; swprintf_s(name, L"\\%04zu-%05d.png", i, (int)frames[i].ms);
        IWICBitmap *b = NULL; IWICStream *st = NULL; IWICBitmapEncoder *enc = NULL; IWICBitmapFrameEncode *fr = NULL;
        wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppBGRA, W * 4, W * H * 4, frames[i].px.data(), &b);
        wic->CreateStream(&st); st->InitializeFromFilename((out + name).c_str(), GENERIC_WRITE);
        wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc); enc->Initialize(st, WICBitmapEncoderNoCache);
        enc->CreateNewFrame(&fr, NULL); fr->Initialize(NULL); fr->SetSize(W, H);
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA; fr->SetPixelFormat(&pf);
        fr->WriteSource(b, NULL); fr->Commit(); enc->Commit();
        fr->Release(); enc->Release(); st->Release(); b->Release();
    }
    printf("hecho: %ls\n", out.c_str());
    return 0;
}
