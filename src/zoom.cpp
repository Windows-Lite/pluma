// Pluma: into fullscreen and out with a zoom, as phones and Macs do: the video grows from its place in the page to
// fill the screen, or shrinks back into it. A window over everything, see-through but for what it draws, holds the
// frame on screen (taken from the player itself) and a black layer: going in, the frame grows and the black comes
// over the page, and once all is covered the window changes underneath; coming out, the window changes first, under
// frame and black, and then the frame shrinks into its place while the black clears off the page around it. The
// animation is DirectComposition's: Windows itself moves it at the screen's refresh rate, whatever Pluma, mpv or the
// page are doing. (Before, 25/10: at once, "tosco, laggy"; a fade through black, "tosca, no fluida"; the live video
// moved by mpv's margins, "super choppy"; a photo of the whole screen, which froze it for a third of a second while
// Windows read it back, and the sleeping page laid out again for the whole screen: "se laguea todo".)
#include "pluma.h"
#include <atomic>
#include <thread>
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

// to the window: wp 1 the frame is ready (lp 1: shown), show it (and coming out, change); wp 3 change (going in); 2 end
static const UINT WM_APP_ZOOM = WM_APP + 30;
static const double kZoomMs = 300, kFadeMs = 110, kSettleMaxMs = 300;
static std::atomic<bool> g_busy{ false }, g_want{ false }, g_shown{ false }, g_changed{ false }, g_settled{ false }, g_stop{ false };
static RECT g_after;                     // the player's window once changed (screen coordinates); written before g_changed
static HWND g_cover;                     // the zoom's window (the window's thread's: the mouse goes through it)
static bool g_enter;                     // this zoom goes into fullscreen
static std::atomic<HWND> g_zwin{ NULL };  // the window it zooms in (its thread's messages go there)

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
    // with two); the zoom is composed there, with no copying between cards
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
static void wait_for(std::atomic<bool> &flag, double ms) { double t = now_ms(); while (!flag && !g_stop && now_ms() - t < ms) Sleep(2); }
// enter: from the player's place (box) to the screen; out: from the screen to where the player is once changed
static void zoom_run(bool enter, RECT mon, RECT box, double aspect, void *client) {
    double began = now_ms();
    std::vector<BYTE> px;
    int fw = 0, fh = 0;
    bool ok = video_frame(client, mon.right - mon.left, px, fw, fh);
    video_client_done(client);
    double t_frame = now_ms();
    ComPtr<ID3D11Device> d3d; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDCompositionDevice> dev; ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, dark, pic;
    ComPtr<IDCompositionSurface> s_dark, s_pic;
    ComPtr<IDCompositionEffectGroup> fx_dark, fx_root;
    ComPtr<IDCompositionScaleTransform> sc; ComPtr<IDCompositionTranslateTransform> tr;
    int W = mon.right - mon.left, H = mon.bottom - mon.top;
    D2D1_RECT_F from = fit(aspect, enter ? box : mon);
    auto place = [&](D2D1_RECT_F r, bool animate) {
        float sx = (r.right - r.left) / fw, sy = (r.bottom - r.top) / fh, x = r.left - mon.left, y = r.top - mon.top;
        if (!animate) { sc->SetScaleX(sx); sc->SetScaleY(sy); tr->SetOffsetX(x); tr->SetOffsetY(y); return; }
        float sx0 = (from.right - from.left) / fw, sy0 = (from.bottom - from.top) / fh;
        sc->SetScaleX(ease(dev.Get(), sx0, sx, kZoomMs).Get()); sc->SetScaleY(ease(dev.Get(), sy0, sy, kZoomMs).Get());
        tr->SetOffsetX(ease(dev.Get(), from.left - mon.left, x, kZoomMs).Get()); tr->SetOffsetY(ease(dev.Get(), from.top - mon.top, y, kZoomMs).Get());
    };
    ComPtr<IDXGIAdapter1> card = ok ? screen_card(mon) : nullptr;
    do {
        if (!ok || !g_cover || g_stop) { ok = false; break; }
        ok = false;
        if (FAILED(D3D11CreateDevice(card.Get(), card ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     NULL, 0, D3D11_SDK_VERSION, &d3d, NULL, &ctx))) break;
        if (FAILED(d3d.As(&dxgi)) || FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dev)))) break;
        if (FAILED(dev->CreateTargetForHwnd(g_cover, TRUE, &target))) break;
        BYTE black[8 * 8 * 4] = {};
        if (!fill_surface(dev.Get(), ctx.Get(), 8, 8, black, 32, s_dark) || !fill_surface(dev.Get(), ctx.Get(), fw, fh, px.data(), fw * 4, s_pic)) break;
        px.clear(); px.shrink_to_fit();
        dev->CreateVisual(&root); dev->CreateVisual(&dark); dev->CreateVisual(&pic);
        dark->SetContent(s_dark.Get()); pic->SetContent(s_pic.Get());
        // the black: 8x8, stretched over the screen; see-through going in, full coming out
        ComPtr<IDCompositionScaleTransform> sc_dark; dev->CreateScaleTransform(&sc_dark);
        sc_dark->SetScaleX(W / 8.0f); sc_dark->SetScaleY(H / 8.0f);
        dark->SetTransform(sc_dark.Get());
        dev->CreateEffectGroup(&fx_dark); dev->CreateEffectGroup(&fx_root);
        fx_dark->SetOpacity(enter ? 0.0f : 1.0f);
        dark->SetEffect(fx_dark.Get());
        // the frame, where the video is now
        dev->CreateScaleTransform(&sc); dev->CreateTranslateTransform(&tr);
        place(from, false);
        IDCompositionTransform *pair[] = { sc.Get(), tr.Get() };
        ComPtr<IDCompositionTransform> group;
        dev->CreateTransformGroup(pair, 2, &group);
        pic->SetTransform(group.Get());
        pic->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        root->SetEffect(fx_root.Get());
        root->AddVisual(dark.Get(), TRUE, NULL);
        root->AddVisual(pic.Get(), TRUE, dark.Get());
        target->SetRoot(root.Get());
        dev->Commit();
        dev->WaitForCommitCompletion();
        ok = true;
    } while (false);
    double t_ready = now_ms();
    // the window shows it (and coming out, changes under it; with no frame, it simply changes)
    PostMessageW(g_zwin.load(), WM_APP_ZOOM, 1, ok ? 1 : 0);
    if (ok) {
        if (enter) {
            wait_for(g_shown, 500);
            place(fit(aspect, mon), true);
            fx_dark->SetOpacity(ease(dev.Get(), 0, 1, kZoomMs).Get());
            dev->Commit();
            double t0 = now_ms();
            while (!g_stop && now_ms() - t0 < kZoomMs + 16) Sleep(4);
            // all covered: the window goes fullscreen underneath
            PostMessageW(g_zwin.load(), WM_APP_ZOOM, 3, 0);
            wait_for(g_changed, 500);
            wait_for(g_settled, kSettleMaxMs);
        } else {
            wait_for(g_changed, 500);
            wait_for(g_settled, kSettleMaxMs);
            place(fit(aspect, g_after), true);
            fx_dark->SetOpacity(ease(dev.Get(), 1, 0, kZoomMs).Get());
            dev->Commit();
            double t0 = now_ms();
            while (!g_stop && now_ms() - t0 < kZoomMs + 16) Sleep(4);
        }
        // the frame gives way to the live video under it
        fx_root->SetOpacity(ease(dev.Get(), 1, 0, kFadeMs).Get());
        dev->Commit();
        Sleep((DWORD)kFadeMs + 20);
    }
    if (g_dev) log_line(L"zoom: " + std::to_wstring((int)(now_ms() - began)) + L" ms (fotograma " + std::to_wstring((int)(t_frame - began)) +
                        L", listo " + std::to_wstring((int)(t_ready - began)) + L")" + (ok ? L"" : L" (sin fotograma: cambio directo)"));
    target.Reset(); root.Reset(); dark.Reset(); pic.Reset(); dev.Reset();
    PostMessageW(g_zwin.load(), WM_APP_ZOOM, 2, 0);
}
static LRESULT CALLBACK cover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // the zoom lets the mouse through to what is under it (Pluma's own windows): its cursor, its clicks
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}
void zoom_start(bool on) {
    // fullscreen in (the video from its place in the page to the screen) or out (back there)
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = cover_proc; wc.hInstance = g_inst; wc.lpszClassName = L"PlumaZoom";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        registered = RegisterClassExW(&wc) != 0;
    }
    g_want = on;
    RECT mon = fullscreen_rect(), box = video_window_rect();
    double aspect = video_aspect();
    void *client = video_client();
    if (aspect <= 0 || !client || IsRectEmpty(&box)) { video_client_done(client); fullscreen_now(on); return; }
    g_shown = g_changed = g_settled = g_stop = false;
    g_enter = on;
    g_busy = true;
    g_zwin = g_win;
    g_cover = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PlumaZoom", L"", WS_POPUP,
                              mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top, NULL, NULL, g_inst, NULL);
    // and not Windows' own animations meanwhile: of the window (they ran on after the change), nor of the zoom's,
    // which faded in (the change was seen through it)
    BOOL off = TRUE;
    DwmSetWindowAttribute(g_win, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    if (g_cover) DwmSetWindowAttribute(g_cover, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    std::thread(zoom_run, on, mon, box, aspect, client).detach();
}
bool zoom_busy() { return g_busy; }
void zoom_want(bool on) { g_want = on; }
void zoom_stop() {
    // the window goes (closed to the tray): the zoom with it, and its thread ends at once
    if (!g_busy) return;
    g_stop = true;
    if (g_cover) ShowWindow(g_cover, SW_HIDE);
    // (its window closing, one of several: the zoom's end goes to another, or it would never come)
    if (g_zwin.load() == g_win)
        for (auto &w : g_windows) if (w->hwnd != g_win) { g_zwin = w->hwnd; break; }
}
static void change() {
    if (!g_stop) fullscreen_now(g_want);
    g_after = video_window_rect();
    g_settled = video_settled();
    g_changed = true;
}
void zoom_message(WPARAM wp, LPARAM lp) {
    // on the window's thread: the zoom shown, the change itself under it, and the end
    if (wp == 1) {
        if (lp && g_cover && !g_stop) {
            SetWindowPos(g_cover, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            DwmFlush();
        }
        g_shown = true;
        if (!lp || !g_enter) change();   // coming out (or with nothing to show): the window changes now, under it
    } else if (wp == 3) {
        change();
    } else {
        if (g_cover) { DestroyWindow(g_cover); g_cover = NULL; }
        g_busy = false;
        BOOL off = FALSE;
        DwmSetWindowAttribute(g_win, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        if (g_fullscreen != g_want && !g_stop) fullscreen_now(g_want);   // asked again meanwhile
        video_zoom_done();
    }
}
void video_resized() { if (g_changed) g_settled = video_settled(); }
