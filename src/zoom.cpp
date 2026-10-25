// Pluma: into fullscreen and out with a zoom, as phones and Macs do. The screen is photographed, the photo goes over
// everything, and its picture grows from where it was to fill the screen (or shrinks back to its place) while the
// rest goes dark; the window changes underneath, out of sight, and once it has drawn itself the photo fades out.
// The animation is DirectComposition's: Windows itself moves it at the screen's refresh rate, whatever Pluma's window
// is doing. (A fade through black looked "tosca, no fluida", 25/10; changed at once, the page was seen laid out
// twice.)
#include "pluma.h"
#include <atomic>
#include <thread>
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

static const UINT WM_APP_ZOOM = WM_APP + 30;   // to the window: wp 1 the photo is up, change now; wp 2 gone
static const double kZoomMs = 240, kFadeMs = 120, kSettleMaxMs = 260;
static std::atomic<bool> g_busy{ false }, g_want{ false }, g_applied{ false }, g_settled{ false };
static RECT g_after;   // the media area once the window has changed (screen coordinates); written before g_applied
static std::function<void()> g_change;   // the change itself, on the window's thread
static bool g_for_fullscreen;

static RECT media_area() {
    // where the page or the player is, in screen coordinates (all of the window below the bar)
    RECT rc; GetClientRect(g_win, &rc);
    rc.top = (LONG)ui_band_height();
    MapWindowPoints(g_win, NULL, (POINT *)&rc, 2);
    return rc;
}
static D2D1_RECT_F fit(double aspect, RECT r) {
    // the picture inside an area, as a player shows it (black bars where it does not fill)
    double w = r.right - r.left, h = r.bottom - r.top;
    double pw = w, ph = w / aspect;
    if (ph > h) { ph = h; pw = h * aspect; }
    double x = r.left + (w - pw) / 2, y = r.top + (h - ph) / 2;
    return { (float)x, (float)y, (float)(x + pw), (float)(y + ph) };
}
static ComPtr<IDXGIAdapter1> screen_card(RECT mon) {
    // the card the screen hangs from (Windows' list of displays: DXGI shows it as the process's card on a laptop
    // with two); the photo is composed there, with no copying between cards
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
    POINT c = { (mon.left + mon.right) / 2, (mon.top + mon.bottom) / 2 };
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
    std::wstring name;
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++, dd.cb = sizeof(dd))
        if (!wcscmp(dd.DeviceName, mi.szDevice)) { name = dd.DeviceString; break; }
    ComPtr<IDXGIFactory1> f;
    ComPtr<IDXGIAdapter1> a;
    if (name.empty() || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(a->GetDesc1(&ad)) && name == ad.Description) return a;
    }
    return nullptr;
}
// a cubic curve for a DirectComposition animation: from a to b in d ms, easing out (1 - (1 - t)^3)
static ComPtr<IDCompositionAnimation> ease(IDCompositionDevice *dev, float a, float b, double ms) {
    ComPtr<IDCompositionAnimation> an;
    if (FAILED(dev->CreateAnimation(&an))) return nullptr;
    double d = ms / 1000.0, k = b - a;
    an->AddCubic(0, a, (float)(3 * k / d), (float)(-3 * k / (d * d)), (float)(k / (d * d * d)));
    an->End(d, b);
    return an;
}
static bool fill_surface(IDCompositionDevice *dev, ID3D11DeviceContext *ctx, UINT w, UINT h, const BYTE *px, UINT pitch,
                         ComPtr<IDCompositionSurface> &out) {
    if (FAILED(dev->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &out))) return false;
    ComPtr<ID3D11Texture2D> tex;
    POINT off;
    if (FAILED(out->BeginDraw(NULL, IID_PPV_ARGS(&tex), &off))) return false;
    D3D11_BOX box = { (UINT)off.x, (UINT)off.y, 0, (UINT)off.x + w, (UINT)off.y + h, 1 };
    ctx->UpdateSubresource(tex.Get(), 0, &box, px, pitch, 0);
    out->EndDraw();
    return true;
}
// enter: the picture grows over a photo of what was there, darkening; else it shrinks over black. mon: the area the
// photo covers (the screen, or the page area); from: where the picture is now, in screen coordinates
static void zoom_run(bool enter, RECT mon, D2D1_RECT_F from, double aspect) {
    double began = now_ms();
    int W = mon.right - mon.left, H = mon.bottom - mon.top;
    bool ok = false;
    HWND h = NULL;
    // the photo of the screen as it is now (the window's controls included)
    std::vector<BYTE> shot((size_t)W * H * 4);
    {
        HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(mdc, bmp);
        BitBlt(mdc, 0, 0, W, H, sdc, mon.left, mon.top, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        memcpy(shot.data(), bits, shot.size());
        SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL, sdc);
    }
    ComPtr<ID3D11Device> d3d; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDCompositionDevice> dev; ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, back, dark, pic;
    ComPtr<IDCompositionSurface> s_back, s_dark, s_pic;
    ComPtr<IDCompositionEffectGroup> fx_dark, fx_root;
    ComPtr<IDCompositionScaleTransform> sc; ComPtr<IDCompositionTranslateTransform> tr;
    // the picture as it is on the screen now: a piece of the photo
    int px0 = std::max(0, (int)from.left - (int)mon.left), py0 = std::max(0, (int)from.top - (int)mon.top);
    int pw = std::min(W - px0, (int)(from.right - from.left)), ph = std::min(H - py0, (int)(from.bottom - from.top));
    ComPtr<IDXGIAdapter1> card = screen_card(mon);
    do {
        if (pw <= 0 || ph <= 0) break;
        if (FAILED(D3D11CreateDevice(card.Get(), card ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     NULL, 0, D3D11_SDK_VERSION, &d3d, NULL, &ctx))) break;
        if (FAILED(d3d.As(&dxgi)) || FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dev)))) break;
        h = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            L"PlumaZoom", L"", WS_POPUP, mon.left, mon.top, W, H, NULL, NULL, g_inst, NULL);
        if (!h || FAILED(dev->CreateTargetForHwnd(h, TRUE, &target))) break;
        BYTE black[8 * 8 * 4] = {};
        if (!fill_surface(dev.Get(), ctx.Get(), W, H, shot.data(), W * 4, s_back) ||
            !fill_surface(dev.Get(), ctx.Get(), 8, 8, black, 32, s_dark) ||
            !fill_surface(dev.Get(), ctx.Get(), pw, ph, shot.data() + ((size_t)py0 * W + px0) * 4, W * 4, s_pic)) break;
        dev->CreateVisual(&root); dev->CreateVisual(&back); dev->CreateVisual(&dark); dev->CreateVisual(&pic);
        back->SetContent(s_back.Get()); dark->SetContent(s_dark.Get()); pic->SetContent(s_pic.Get());
        // the dark layer: 8x8 black, stretched over the screen
        ComPtr<IDCompositionScaleTransform> sc_dark; dev->CreateScaleTransform(&sc_dark);
        sc_dark->SetScaleX(W / 8.0f); sc_dark->SetScaleY(H / 8.0f);
        dark->SetTransform(sc_dark.Get());
        dev->CreateEffectGroup(&fx_dark); dev->CreateEffectGroup(&fx_root);
        fx_dark->SetOpacity(enter ? 0.0f : 1.0f);
        dark->SetEffect(fx_dark.Get());
        // the picture, where it is now
        dev->CreateScaleTransform(&sc); dev->CreateTranslateTransform(&tr);
        float s0 = (from.right - from.left) / pw;
        sc->SetScaleX(s0); sc->SetScaleY(s0);
        tr->SetOffsetX(from.left - mon.left); tr->SetOffsetY(from.top - mon.top);
        IDCompositionTransform *pair[] = { sc.Get(), tr.Get() };
        ComPtr<IDCompositionTransform> group;
        dev->CreateTransformGroup(pair, 2, &group);
        pic->SetTransform(group.Get());
        root->SetEffect(fx_root.Get());
        if (enter) root->AddVisual(back.Get(), FALSE, NULL);   // (going in: the photo of the window behind, darkening)
        root->AddVisual(dark.Get(), TRUE, NULL);
        root->AddVisual(pic.Get(), TRUE, dark.Get());
        target->SetRoot(root.Get());
        dev->Commit();
        dev->WaitForCommitCompletion();
        ShowWindow(h, SW_SHOWNOACTIVATE);
        DwmFlush();
        DwmFlush();
        ok = true;
    } while (false);
    // the window changes now, under the photo (or at once, if there is no photo)
    PostMessageW(g_win, WM_APP_ZOOM, 1, 0);
    if (ok) {
        double t = now_ms();
        while (!g_applied && now_ms() - t < 400) Sleep(2);
        D2D1_RECT_F to = fit(aspect, g_after);
        float s1 = (to.right - to.left) / pw;
        // the zoom
        sc->SetScaleX(ease(dev.Get(), (from.right - from.left) / pw, s1, kZoomMs).Get());
        sc->SetScaleY(ease(dev.Get(), (from.right - from.left) / pw, s1, kZoomMs).Get());
        tr->SetOffsetX(ease(dev.Get(), from.left - mon.left, to.left - mon.left, kZoomMs).Get());
        tr->SetOffsetY(ease(dev.Get(), from.top - mon.top, to.top - mon.top, kZoomMs).Get());
        if (enter) fx_dark->SetOpacity(ease(dev.Get(), 0, 1, kZoomMs).Get());
        dev->Commit();
        double t0 = now_ms();
        Sleep((DWORD)kZoomMs);
        // then, once the player draws at its new size (a page gets the zoom's time), the photo fades out
        while (!g_settled && now_ms() - t0 < kZoomMs + kSettleMaxMs) Sleep(4);
        fx_root->SetOpacity(ease(dev.Get(), 1, 0, kFadeMs).Get());
        dev->Commit();
        Sleep((DWORD)kFadeMs + 20);
    }
    if (g_dev) log_line(L"zoom: " + std::to_wstring((int)(now_ms() - began)) + L" ms" + (ok ? L"" : L" (sin foto: cambio directo)"));
    if (h) DestroyWindow(h);
    PostMessageW(g_win, WM_APP_ZOOM, 2, 0);
}
static void zoom_go(bool grow, RECT area, D2D1_RECT_F from, double aspect, std::function<void()> change, bool for_fullscreen) {
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g_inst; wc.lpszClassName = L"PlumaZoom";
        registered = RegisterClassExW(&wc) != 0;
    }
    g_change = change;
    g_for_fullscreen = for_fullscreen;
    g_applied = g_settled = false;
    g_busy = true;
    // and not Windows' own maximize animation when the window goes back: it ran on after the change
    BOOL off = TRUE;
    DwmSetWindowAttribute(g_win, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    std::thread(zoom_run, grow, area, from, aspect).detach();
}
static double picture_aspect(RECT area) {
    // the picture's shape: the video's in Pluma's player; else the page area's own (it simply grows)
    double aspect = video_aspect();
    return aspect > 0 ? aspect : (double)(area.right - area.left) / std::max<LONG>(1, area.bottom - area.top);
}
void zoom_start(bool on) {
    // fullscreen in or out
    g_want = on;
    RECT mon = fullscreen_rect(), area = media_area();
    double aspect = picture_aspect(area);
    zoom_go(on, mon, fit(aspect, on ? area : mon), aspect, [] { fullscreen_now(g_want); }, true);
}
bool zoom_into_player(RECT page_player, std::function<void()> change) {
    // a YouTube video going from its page into Pluma's player: it grows from its place in the page to fill the
    // page area (in sight, so the change is understood: it came late and all at once, "se pone así", 25/10)
    if (g_busy || GetForegroundWindow() != g_win || IsIconic(g_win)) return false;
    RECT area = media_area();
    double aspect = picture_aspect(area);
    D2D1_RECT_F from = fit(aspect, page_player);
    zoom_go(true, area, from, aspect, change, false);
    return true;
}
bool zoom_busy() { return g_busy; }
void zoom_want(bool on) { g_want = on; }
void zoom_message(WPARAM wp) {
    // on the window's thread: the change itself, under the photo; and the end
    if (wp == 1) {
        if (g_change) g_change();
        g_after = media_area();
        g_settled = video_settled();
        g_applied = true;
    } else {
        g_busy = false;
        BOOL off = FALSE;
        DwmSetWindowAttribute(g_win, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        g_change = nullptr;
        if (g_for_fullscreen && g_fullscreen != g_want) fullscreen_now(g_want);   // asked again meanwhile
    }
}
void video_resized() { if (g_applied) g_settled = video_settled(); }
