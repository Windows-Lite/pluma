// Pluma: into fullscreen and out, and a YouTube video from its page into Pluma's player, with the picture moving on
// screen as it plays. mpv itself draws the video in any box of its window (its margins), so the picture grows or
// shrinks live, every frame, with no photo standing still in its place. A photo of the screen only hides the moment
// the window changes size (a few frames): its picture goes as soon as the player draws at the new size, and what is
// around the picture (the bar, the taskbar, the page) fades out over the video as it grows, or in once it has shrunk.
// (Before, a photo of the picture grew instead: the video stood still for half a second, and between window and
// fullscreen it only grows a tenth: "no hay transiciones, tosco y laggy", 25/10. The photo's window, on a thread that
// read no messages, also showed Windows' busy cursor at every change.)
#include "pluma.h"
#include <atomic>
#include <thread>
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

static const UINT WM_APP_ZOOM = WM_APP + 30;   // to the window: wp 1 the photo is ready (lp 1: there is one), change now; wp 2 the end
static const double kGrowMs = 300, kShrinkMs = 260, kRevealMs = 170, kSettleMaxMs = 300;
static std::atomic<bool> g_busy{ false }, g_want{ false }, g_applied{ false }, g_settled{ false }, g_stop{ false };
static std::function<void()> g_change;   // the change itself, on the window's thread
static bool g_for_fullscreen;
static HWND g_cover;                     // the photo's window (the window's thread's: the mouse goes through it)
static void *g_client;                   // the zoom's own handle on mpv, for its thread
static D2D1_RECT_F g_keep;               // the picture's box in the photo; after the change, where it stays
static D2D1_RECT_F g_win_after;          // the player's window once changed (both written before g_applied)
static D2D1_RECT_F g_win_foreseen;       // ... as foreseen before the change
static RECT g_area_before;               // the page area before fullscreen: where the picture goes back to

static D2D1_RECT_F rf(RECT r) { return { (float)r.left, (float)r.top, (float)r.right, (float)r.bottom }; }
static RECT media_area() {
    // where the page or the player is, in screen coordinates (all of the window below the bar)
    RECT rc; GetClientRect(g_win, &rc);
    rc.top = (LONG)ui_band_height();
    MapWindowPoints(g_win, NULL, (POINT *)&rc, 2);
    return rc;
}
static ComPtr<IDXGIAdapter1> screen_card(RECT mon, ComPtr<IDXGIOutput> *output) {
    // the card the screen hangs from (Windows' list of displays: DXGI shows it as the process's card on a laptop
    // with two), and its output for that screen: the photo is composed there, and the picture moves at its refresh
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
    POINT c = { (mon.left + mon.right) / 2, (mon.top + mon.bottom) / 2 };
    HMONITOR hm = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(hm, &mi);
    std::wstring name;
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++, dd.cb = sizeof(dd))
        if (!wcscmp(dd.DeviceName, mi.szDevice)) { name = dd.DeviceString; break; }
    ComPtr<IDXGIFactory1> f;
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
        ComPtr<IDXGIOutput> o;
        for (UINT k = 0; a->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; k++, o.Reset()) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == hm && output) *output = o;
        }
        DXGI_ADAPTER_DESC1 ad;
        if (!name.empty() && SUCCEEDED(a->GetDesc1(&ad)) && name == ad.Description) return a;
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
static void next_frame(IDXGIOutput *out) {
    // the screen's next refresh (or DWM's next frame, with no output)
    if (!out || FAILED(out->WaitForVBlank())) DwmFlush();
}
static void move(void *client, IDXGIOutput *out, D2D1_RECT_F win, D2D1_RECT_F a, D2D1_RECT_F b, double ms) {
    // the picture from box a to box b inside its window, live, a step at each refresh of the screen (easing out)
    double t0 = now_ms();
    for (;;) {
        double t = ms > 0 ? std::min(1.0, (now_ms() - t0) / ms) : 1, e = 1 - pow(1 - t, 3);
        D2D1_RECT_F r = { (float)(a.left + (b.left - a.left) * e), (float)(a.top + (b.top - a.top) * e),
                          (float)(a.right + (b.right - a.right) * e), (float)(a.bottom + (b.bottom - a.bottom) * e) };
        video_box(client, win, r);
        if (t >= 1 || g_stop) break;
        next_frame(out);
    }
}
struct Plan {
    RECT shot;                 // what the photo covers (screen px)
    D2D1_RECT_F win_after;     // the player's window once changed, as foreseen (its margins are set before the change)
    bool shrink;               // out of fullscreen: the picture first shrinks, live, to where it goes back
    D2D1_RECT_F from, to;      // ... from the whole screen to that box (its window does not change meanwhile)
    D2D1_RECT_F keep;          // the picture's box in the photo
    double grow_ms, reveal_ms; // after the change: the picture to its whole window, and what is around fading
};
static void zoom_run(Plan p) {
    double began = now_ms();
    void *client = g_client;
    ComPtr<IDXGIOutput> out;
    ComPtr<IDXGIAdapter1> card = screen_card(p.shot, &out);
    if (p.shrink) move(client, out.Get(), rf(p.shot), p.from, p.to, kShrinkMs);
    int W = p.shot.right - p.shot.left, H = p.shot.bottom - p.shot.top;
    bool ok = false;
    // the photo of the screen as it is now
    std::vector<BYTE> shot((size_t)std::max(0, W) * std::max(0, H) * 4);
    if (!shot.empty()) {
        HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(mdc, bmp);
        BitBlt(mdc, 0, 0, W, H, sdc, p.shot.left, p.shot.top, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        memcpy(shot.data(), bits, shot.size());
        SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL, sdc);
    }
    ComPtr<ID3D11Device> d3d; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDCompositionDevice> dev; ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, ring, center;
    ComPtr<IDCompositionEffectGroup> fx_ring;
    std::vector<ComPtr<IDCompositionVisual>> pieces;
    std::vector<ComPtr<IDCompositionSurface>> surfaces;
    // the photo in pieces: the picture's box, and the four bands around it (the bar, the taskbar, the page...)
    int kl = std::max(0, std::min(W, (int)p.keep.left - (int)p.shot.left)), kt = std::max(0, std::min(H, (int)p.keep.top - (int)p.shot.top));
    int kr = std::max(kl, std::min(W, (int)p.keep.right - (int)p.shot.left)), kb = std::max(kt, std::min(H, (int)p.keep.bottom - (int)p.shot.top));
    RECT parts[5] = { { kl, kt, kr, kb }, { 0, 0, W, kt }, { 0, kb, W, H }, { 0, kt, kl, kb }, { kr, kt, W, kb } };
    do {
        if (shot.empty() || !g_cover || g_stop) break;
        if (FAILED(D3D11CreateDevice(card.Get(), card ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     NULL, 0, D3D11_SDK_VERSION, &d3d, NULL, &ctx))) break;
        if (FAILED(d3d.As(&dxgi)) || FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dev)))) break;
        if (FAILED(dev->CreateTargetForHwnd(g_cover, TRUE, &target))) break;
        dev->CreateVisual(&root); dev->CreateVisual(&ring);
        dev->CreateEffectGroup(&fx_ring);
        ring->SetEffect(fx_ring.Get());
        bool fine = true;
        for (int i = 0; i < 5 && fine; i++) {
            RECT r = parts[i];
            if (r.right <= r.left || r.bottom <= r.top) continue;
            ComPtr<IDCompositionSurface> s;
            ComPtr<IDCompositionVisual> vis;
            fine = fill_surface(dev.Get(), ctx.Get(), r.right - r.left, r.bottom - r.top, shot.data() + ((size_t)r.top * W + r.left) * 4, W * 4, s) &&
                   SUCCEEDED(dev->CreateVisual(&vis));
            if (!fine) break;
            vis->SetContent(s.Get());
            vis->SetOffsetX((float)r.left); vis->SetOffsetY((float)r.top);
            if (i == 0) center = vis; else ring->AddVisual(vis.Get(), TRUE, NULL);
            pieces.push_back(vis); surfaces.push_back(s);
        }
        if (!fine) break;
        root->AddVisual(ring.Get(), TRUE, NULL);
        if (center) root->AddVisual(center.Get(), TRUE, ring.Get());
        target->SetRoot(root.Get());
        dev->Commit();
        dev->WaitForCommitCompletion();
        ok = true;
    } while (false);
    // the window changes now, under the photo (or at once, if there is no photo)
    double t_photo = now_ms(), t_applied = 0, t_settled = 0;
    PostMessageW(g_win, WM_APP_ZOOM, 1, ok ? 1 : 0);
    double t = now_ms();
    while (!g_applied && !g_stop && now_ms() - t < 500) Sleep(2);
    t_applied = now_ms();
    if (g_applied && !g_stop) {
        D2D1_RECT_F win = g_win_after, keep = g_keep;
        if (ok) {
            // once the player draws at its new size, the photo's picture goes: the live one is there, in its place
            t = now_ms();
            while (!g_settled && !g_stop && now_ms() - t < kSettleMaxMs) Sleep(2);
            t_settled = now_ms();
            next_frame(out.Get()); next_frame(out.Get());
            if (center) root->RemoveVisual(center.Get());
            fx_ring->SetOpacity(ease(dev.Get(), 1, 0, p.reveal_ms).Get());
            dev->Commit();
        }
        double t0 = now_ms();
        move(client, out.Get(), win, keep, win, p.grow_ms);
        double left = p.reveal_ms + 16 - (now_ms() - t0);
        if (ok && left > 0 && !g_stop) Sleep((DWORD)left);
        video_box(client, win, win);
    }
    if (g_dev) log_line(L"zoom: " + std::to_wstring((int)(now_ms() - began)) + L" ms (foto a los " + std::to_wstring((int)(t_photo - began)) +
                        L", cambio " + std::to_wstring((int)(t_applied - began)) + L", asentado " + std::to_wstring((int)(t_settled - began)) + L")" +
                        (ok ? L"" : L" (sin foto: cambio directo)"));
    target.Reset(); pieces.clear(); surfaces.clear(); center.Reset(); ring.Reset(); root.Reset(); dev.Reset();
    video_box_done(client);
    PostMessageW(g_win, WM_APP_ZOOM, 2, 0);
}
static LRESULT CALLBACK cover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // the photo lets the mouse through to what is under it (Pluma's own windows): its cursor, its clicks
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}
static bool zoom_go(Plan p, std::function<void()> change, bool for_fullscreen) {
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = cover_proc; wc.hInstance = g_inst; wc.lpszClassName = L"PlumaZoom";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        registered = RegisterClassExW(&wc) != 0;
    }
    void *client = video_box_client();
    if (!client) return false;
    g_client = client;
    g_change = change;
    g_for_fullscreen = for_fullscreen;
    g_keep = p.keep;
    g_win_foreseen = p.win_after;
    g_applied = g_settled = g_stop = false;
    g_busy = true;
    g_cover = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PlumaZoom", L"", WS_POPUP,
                              p.shot.left, p.shot.top, p.shot.right - p.shot.left, p.shot.bottom - p.shot.top, NULL, NULL, g_inst, NULL);
    // and not Windows' own animations meanwhile: of the window (they ran on after the change), nor of the photo,
    // which faded in while the window changed under it (the change was seen through it, once in the recording)
    BOOL off = TRUE;
    DwmSetWindowAttribute(g_win, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    if (g_cover) DwmSetWindowAttribute(g_cover, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    std::thread(zoom_run, p).detach();
    return true;
}
void zoom_remember_area() { g_area_before = media_area(); }
void zoom_start(bool on) {
    // fullscreen in: the picture grows from the page area to the screen; out: it shrinks back there, then the window
    g_want = on;
    RECT mon = fullscreen_rect();
    Plan p = {};
    p.shot = mon;
    if (on) {
        RECT area = media_area();
        p.keep = rf(area);
        p.win_after = rf(mon);
        p.grow_ms = kGrowMs; p.reveal_ms = kGrowMs * 0.75;
    } else {
        RECT back = g_area_before, cut;
        if (!IntersectRect(&cut, &back, &mon) || !EqualRect(&cut, &back)) back = mon;   // (unknown, or on another screen)
        p.shrink = !EqualRect(&back, &mon);
        p.from = rf(mon); p.to = rf(back); p.keep = rf(back);
        p.win_after = rf(back);
        p.grow_ms = kRevealMs; p.reveal_ms = kRevealMs;
    }
    if (!zoom_go(p, [] { fullscreen_now(g_want); }, true)) fullscreen_now(on);
}
bool zoom_into_player(RECT page_player, std::function<void()> change) {
    // a YouTube video going from its page into Pluma's player: the player shows it where the page did, and it grows
    // to fill the page area while the page around it fades (in sight, so the change is understood: it came late and
    // all at once, "se pone así", 25/10)
    if (g_busy || (GetForegroundWindow() != g_win && !g_test_visible) || IsIconic(g_win)) return false;
    RECT area = media_area(), box;
    if (!IntersectRect(&box, &page_player, &area)) return false;
    Plan p = {};
    p.shot = area;
    p.keep = rf(box);
    p.win_after = rf(area);
    p.grow_ms = kGrowMs + 40; p.reveal_ms = kGrowMs;
    void *c = video_box_client();
    if (!c) return false;
    video_box(c, rf(area), rf(box));   // (the player, still under the page, draws there already)
    bool going = zoom_go(p, change, false);
    if (!going) video_box(c, rf(area), rf(area));
    video_box_done(c);
    return going;
}
bool zoom_busy() { return g_busy; }
void zoom_want(bool on) { g_want = on; }
void zoom_stop() {
    // the window goes (closed to the tray): the photo with it, and its thread ends at once
    if (!g_busy) return;
    g_stop = true;
    if (g_cover) ShowWindow(g_cover, SW_HIDE);
}
void zoom_message(WPARAM wp, LPARAM lp) {
    // on the window's thread: the change itself, under the photo; and the end
    if (wp == 1) {
        if (lp && g_cover && !g_stop) {
            SetWindowPos(g_cover, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            DwmFlush();
            DwmFlush();
        }
        // the margins for the window as it will be, before it changes: mpv's first picture at the new size has them
        // already (set after, one picture came out with the old ones: the video small in the middle, 25/10)
        video_box(g_client, g_win_foreseen, g_keep);
        if (g_change && !g_stop) g_change();
        // the picture where it was in the photo, inside the player's window as it is now
        D2D1_RECT_F w = rf(video_window_rect()), k = g_keep;
        k = { std::max(k.left, w.left), std::max(k.top, w.top), std::min(k.right, w.right), std::min(k.bottom, w.bottom) };
        if (k.right <= k.left || k.bottom <= k.top) k = w;
        video_box(g_client, w, k);
        g_win_after = w;
        g_keep = k;
        g_settled = video_settled();
        g_applied = true;
    } else {
        if (g_cover) { DestroyWindow(g_cover); g_cover = NULL; }
        g_client = nullptr;
        g_busy = false;
        BOOL off = FALSE;
        DwmSetWindowAttribute(g_win, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        g_change = nullptr;
        if (g_for_fullscreen && g_fullscreen != g_want && !g_stop) fullscreen_now(g_want);   // asked again meanwhile
        video_zoom_done();
    }
}
void video_resized() { if (g_applied) g_settled = video_settled(); }
