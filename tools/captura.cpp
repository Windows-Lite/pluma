// captura.exe <window class> <out.png> [click x y | rclick x y | mclick x y | move x y | key vk | char text | wait ms
//                                       | cls class | burst n ms | drag x1 y1 x2 y2 steps]...
// <window class> may be "class|words": the window of that class whose title has those words (one of several windows)
// Takes a picture of a window (also one off the screen) with everything DWM composes in it, web pages included,
// and can first send it mouse and key messages, for testing Pluma without touching the screen or the focus.
// burst n ms: n pictures, one every ms (out.png -> out-00.png, out-01.png...), to catch what flickers.
// Build: cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE captura.cpp
#include <windows.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

static HWND child_at(HWND top, int x, int y) {
    // the deepest child under a client point of the top window (the bar, or the address field)
    POINT p = { x, y };
    HWND h = top;
    while (true) {
        POINT q = p;
        MapWindowPoints(top, h, &q, 1);
        HWND c = ChildWindowFromPointEx(h, q, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!c || c == h) break;
        wchar_t cls[64]; GetClassNameW(c, cls, 64);
        if (wcsncmp(cls, L"Pluma", 5) && wcscmp(cls, L"Edit")) break;   // not into the web page's own windows
        h = c;
    }
    return h;
}
static void mouse(HWND top, int x, int y, UINT down, UINT up) {
    HWND h = child_at(top, x, y);
    POINT q = { x, y };
    MapWindowPoints(top, h, &q, 1);
    LPARAM lp = MAKELPARAM(q.x, q.y);
    SendMessageW(h, WM_MOUSEMOVE, 0, lp);
    if (down) { SendMessageW(h, down, down == WM_LBUTTONDOWN ? MK_LBUTTON : 0, lp); Sleep(30); SendMessageW(h, up, 0, lp); }
}
static void drag(HWND top, int x1, int y1, int x2, int y2, int steps) {
    // the left button down at (x1, y1), moved in steps to (x2, y2) (anywhere, also off the window: a tab taken out of
    // its strip) and up there; every message to the window under the start, as when it holds the mouse
    // (the points stay where they are on the screen: the window may move with the drag, as one with a single tab)
    HWND h = child_at(top, x1, y1);
    POINT a = { x1, y1 }, b = { x2, y2 };
    ClientToScreen(top, &a); ClientToScreen(top, &b);
    auto at = [&](int x, int y) { POINT q = { x, y }; ScreenToClient(h, &q); return MAKELPARAM(q.x, q.y); };
    SendMessageW(h, WM_MOUSEMOVE, 0, at(a.x, a.y));
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, at(a.x, a.y));
    if (steps < 1) steps = 1;
    for (int k = 1; k <= steps; k++) {
        Sleep(16);
        if (!IsWindow(h)) return;
        SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, at(a.x + (b.x - a.x) * k / steps, a.y + (b.y - a.y) * k / steps));
    }
    Sleep(30);
    if (IsWindow(h)) SendMessageW(h, WM_LBUTTONUP, 0, at(b.x, b.y));
}
static HWND find(const std::wstring &spec) {
    size_t bar = spec.find(L'|');
    if (bar == std::wstring::npos) return FindWindowW(spec.c_str(), NULL);
    struct Ctx { std::wstring cls, words; HWND out; } c = { spec.substr(0, bar), spec.substr(bar + 1), NULL };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto *c = (Ctx *)lp;
        wchar_t cls[256], title[512];
        if (!GetClassNameW(h, cls, 256) || c->cls != cls) return TRUE;
        GetWindowTextW(h, title, 512);
        if (wcsstr(title, c->words.c_str())) { c->out = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.out;
}
static IWICImagingFactory *g_wic;
static bool shoot(HWND w, const wchar_t *path) {
    RECT r; GetWindowRect(w, &r);
    int cw = r.right - r.left, ch = r.bottom - r.top;
    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(w, mdc, 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mdc, old);
    for (int i = 0; i < cw * ch; i++) ((BYTE *)bits)[i * 4 + 3] = 255;
    // the window's own popups on top (the video player's controls are layered windows of their own), blended as
    // the screen shows them
    struct Ctx { HWND owner; RECT r; BYTE *bits; int cw, ch; std::vector<HWND> list; } ctx = { w, r, (BYTE *)bits, cw, ch, {} };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx *c = (Ctx *)lp;
        if (GetWindow(h, GW_OWNER) == c->owner && IsWindowVisible(h)) c->list.push_back(h);
        return TRUE;
    }, (LPARAM)&ctx);
    for (auto it = ctx.list.rbegin(); it != ctx.list.rend(); ++it) {   // bottom of the z-order first
        HWND p = *it;
        RECT pr; GetWindowRect(p, &pr);
        int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
        if (pw <= 0 || ph <= 0) continue;
        BITMAPINFO pb = bi;
        pb.bmiHeader.biWidth = pw; pb.bmiHeader.biHeight = -ph;
        void *pbits = NULL;
        HDC pdc = CreateCompatibleDC(mdc);
        HBITMAP pbmp = CreateDIBSection(pdc, &pb, DIB_RGB_COLORS, &pbits, NULL, 0);
        HGDIOBJ po = SelectObject(pdc, pbmp);
        PrintWindow(p, pdc, 2);
        SelectObject(pdc, po);
        bool layered = (GetWindowLongW(p, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        BYTE ca = 255; DWORD fl = 0;
        if (layered) GetLayeredWindowAttributes(p, NULL, &ca, &fl);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                int dx = pr.left - r.left + x, dy = pr.top - r.top + y;
                if (dx < 0 || dy < 0 || dx >= cw || dy >= ch) continue;
                BYTE *s = (BYTE *)pbits + (y * pw + x) * 4, *d = (BYTE *)bits + (dy * cw + dx) * 4;
                int a = layered ? s[3] : 255;   // premultiplied
                for (int k = 0; k < 3; k++) d[k] = (BYTE)(std::min)(255, s[k] + d[k] * (255 - a) / 255);
            }
        DeleteObject(pbmp); DeleteDC(pdc);
    }
    IWICBitmap *wb = NULL;
    g_wic->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppBGRA, cw * 4, cw * ch * 4, (BYTE *)bits, &wb);
    IWICStream *st = NULL; g_wic->CreateStream(&st); st->InitializeFromFilename(path, GENERIC_WRITE);
    IWICBitmapEncoder *enc = NULL; g_wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc); enc->Initialize(st, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode *fr = NULL; enc->CreateNewFrame(&fr, NULL); fr->Initialize(NULL);
    fr->SetSize(cw, ch);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA; fr->SetPixelFormat(&fmt);
    fr->WriteSource(wb, NULL); fr->Commit(); enc->Commit();
    fr->Release(); enc->Release(); st->Release(); wb->Release();
    DeleteObject(bmp); DeleteDC(mdc);
    return true;
}
int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { fprintf(stderr, "captura <class> <out.png> [actions]\n"); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitialize(NULL);
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    HWND w = find(argv[1]);
    if (!w) { fprintf(stderr, "no window\n"); return 1; }
    for (int i = 3; i < argc; i++) {
        std::wstring a = argv[i];
        if ((a == L"click" || a == L"rclick" || a == L"mclick" || a == L"move") && i + 2 < argc) {
            int x = _wtoi(argv[i + 1]), y = _wtoi(argv[i + 2]);
            i += 2;
            if (a == L"click") mouse(w, x, y, WM_LBUTTONDOWN, WM_LBUTTONUP);
            else if (a == L"rclick") mouse(w, x, y, WM_RBUTTONDOWN, WM_RBUTTONUP);
            else if (a == L"mclick") mouse(w, x, y, WM_MBUTTONDOWN, WM_MBUTTONUP);
            else mouse(w, x, y, 0, 0);
        } else if (a == L"key" && i + 1 < argc) {
            HWND f = GetWindow(w, GW_CHILD);
            GUITHREADINFO gi = { sizeof(gi) };
            if (GetGUIThreadInfo(GetWindowThreadProcessId(w, NULL), &gi) && gi.hwndFocus) f = gi.hwndFocus;
            UINT vk = (UINT)wcstoul(argv[++i], NULL, 0);
            SendMessageW(f, WM_KEYDOWN, vk, 1);
            SendMessageW(f, WM_KEYUP, vk, 0xC0000001);
        } else if (a == L"char" && i + 1 < argc) {
            GUITHREADINFO gi = { sizeof(gi) };
            HWND f = NULL;
            if (GetGUIThreadInfo(GetWindowThreadProcessId(w, NULL), &gi)) f = gi.hwndFocus;
            if (!f) { fprintf(stderr, "nothing has the focus\n"); continue; }
            for (const wchar_t *c = argv[++i]; *c; c++) { SendMessageW(f, WM_CHAR, *c, 1); Sleep(15); }
        } else if (a == L"drag" && i + 5 < argc) {
            drag(w, _wtoi(argv[i + 1]), _wtoi(argv[i + 2]), _wtoi(argv[i + 3]), _wtoi(argv[i + 4]), _wtoi(argv[i + 5]));
            i += 5;
        } else if (a == L"wait" && i + 1 < argc) Sleep(_wtoi(argv[++i]));
        else if (a == L"cls" && i + 1 < argc) { HWND o = find(argv[++i]); if (o) w = o; }
        else if (a == L"burst" && i + 2 < argc) {
            int n = _wtoi(argv[i + 1]), every = _wtoi(argv[i + 2]);
            i += 2;
            std::wstring base = argv[2];
            size_t dot = base.rfind(L'.');
            for (int k = 0; k < n; k++) {
                wchar_t num[16]; swprintf_s(num, L"-%02d", k);
                shoot(w, (base.substr(0, dot) + num + base.substr(dot)).c_str());
                Sleep(every);
            }
            return 0;
        }
    }
    shoot(w, argv[2]);
    RECT r; GetWindowRect(w, &r);
    printf("%dx%d\n", (int)(r.right - r.left), (int)(r.bottom - r.top));
    return 0;
}
