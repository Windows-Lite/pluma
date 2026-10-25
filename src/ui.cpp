// Pluma: the bar (tabs, buttons, address), the menu, the suggestions and the new tab page, drawn with Direct2D.
// Mapea's design system: light paper and dark ink (as Windows by default), Windows's typeface (any other as an option), radius 10 for controls and 14 for
// panels, flat shadows on controls and lifted ones only on the panels that float. One motion language: ease-out,
// no overshoot; 120 ms for hover and focus, 180 ms for tabs, 140 ms for panels; nothing moves when Windows says
// "no animations". The one signature: the active tab is a sheet that slides from tab to tab.
#include "pluma.h"
#include <dwrite_3.h>
#include <unordered_map>
#include <atomic>
#include <thread>

// ---------------------------------------------------------------- metrics (DIP; multiplied by the screen's scale)
static const float kTabRowH = 38, kToolRowH = 44, kNoticeH = 44, kCapW = 46, kTabMax = 236, kTabMin = 40, kTabGap = 2,
                   kTabY = 4, kTabH = 30, kBtn = 32, kAddrH = 34, kPlusW = 30;
static const double kHoverMs = 120, kTabMs = 180, kPanelMs = 140;
static float g_s = 1;
float ui_scale() { return g_s; }
static float S(float v) { return v * g_s; }
static std::wstring strip_scheme(const std::wstring &u);
static std::wstring pretty_path(const std::wstring &s);

// glyphs of Segoe Fluent Icons (Segoe MDL2 Assets on Windows 10)
enum : wchar_t { G_BACK = 0xE72B, G_FWD = 0xE72A, G_RELOAD = 0xE72C, G_STOP = 0xE711, G_ADD = 0xE710, G_MORE = 0xE712,
                 G_X = 0xE8BB, G_MIN = 0xE921, G_MAX = 0xE922, G_RESTORE = 0xE923, G_LOCK = 0xE72E, G_INFO = 0xE946,
                 G_SEARCH = 0xE721, G_GLOBE = 0xE774, G_CLOCK = 0xE81C, G_PUZZLE = 0xEA86, G_VOL = 0xE767, G_MUTE = 0xE74F,
                 G_CHEV_R = 0xE76C, G_CHEV_L = 0xE76B, G_CHECK = 0xE73E, G_GEAR = 0xE713, G_DELETE = 0xE74D,
                 G_FOLDER = 0xE8B7, G_CODE = 0xE943, G_DOWNLOAD = 0xE896, G_PRINT = 0xE749, G_ZOOMIN = 0xE8A3,
                 G_ZOOMOUT = 0xE71F, G_POWER = 0xE7E8, G_COLOR = 0xE790, G_ERASE = 0xE75C, G_FILE = 0xE8A5, G_NEWTAB = 0xEC6C,
                 G_UNDO = 0xE7A7, G_STORE = 0xE719, G_DEV = 0xEC7A, G_ZOOM = 0xE71E, G_STAR = 0xE734, G_STAR_FILL = 0xE735,
                 G_INPRIVATE = 0xE727, G_FONT = 0xE8D2, G_LANG = 0xF2B7, G_UPDATE = 0xE895 };

// ---------------------------------------------------------------- colours (Mapea's tokens)
static D2D1_COLOR_F hex(unsigned v, float a = 1) { return { ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a }; }
static const Palette kLight = { hex(0xf4f6f9), hex(0xffffff), hex(0x1d2733), hex(0x5c6976), hex(0xe2e7ed), hex(0xb9c4cf),
                                hex(0x2f6fed), hex(0x8fb3f6), hex(0xe5484d), hex(0x1d2733, .06f), hex(0x1d2733, .11f) };
static const Palette kDark = { hex(0x1b1f24), hex(0x242a31), hex(0xe7eaee), hex(0x95a0ab), hex(0x313840), hex(0x56616d),
                               hex(0x6596fa), hex(0x3d5f9f), hex(0xf06a6e), hex(0xffffff, .07f), hex(0xffffff, .12f) };
static Anim g_theme;   // 0 light, 1 dark: the bar crossfades when it changes
static Palette g_pal;
static D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float k) {
    return { a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k };
}
static D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
bool dark_theme() {
    if (g_set.theme == 1) return true;
    if (g_set.theme == 0) return false;
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v == 0;
}
const Palette &pal() {
    float k = g_theme.at(now_ms());
    const D2D1_COLOR_F *a = &kLight.bg, *b = &kDark.bg;
    D2D1_COLOR_F *o = &g_pal.bg;
    for (int i = 0; i < (int)(sizeof(Palette) / sizeof(D2D1_COLOR_F)); i++) o[i] = mix(a[i], b[i], k);
    return g_pal;
}

// ---------------------------------------------------------------- Direct2D, DirectWrite, WIC
static ComPtr<ID2D1Factory> g_d2d;
static ComPtr<IDWriteFactory> g_dw;
static ComPtr<IWICImagingFactory> g_wic;
static ComPtr<IDWriteFontCollection> g_ui_fonts;   // the fuentes folder's, when the chosen family is one of them (else Windows's)
static ComPtr<IDWriteFontCollection> g_own_fonts;  // Roboto and Roboto Mono, from the fuentes folder
static std::wstring g_ui_family = L"Segoe UI", g_icon_family = L"Segoe Fluent Icons";
static ComPtr<ID2D1HwndRenderTarget> g_rt;
static D2D1_RENDER_TARGET_TYPE render_type() {
    // PLUMA_DIBUJO=cpu draws the bar without the graphics card (to compare what each costs)
    wchar_t v[8] = L"";
    GetEnvironmentVariableW(L"PLUMA_DIBUJO", v, 8);
    return !wcscmp(v, L"cpu") ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT;
}
static ComPtr<ID2D1SolidColorBrush> g_br;
struct Formats { ComPtr<IDWriteTextFormat> tab, addr, icon, icon_sm, icon_lg, menu, hint, word, ntp, ntp_sub, notice; };
static Formats g_f;
static HFONT g_edit_font;
static std::unordered_map<std::wstring, float> g_widths;   // text widths, measured once
static HBRUSH g_edit_brush;
static COLORREF g_edit_bg;

static ComPtr<IDWriteTextFormat> make_format(bool icons, float px, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> f;
    const wchar_t *family = icons ? g_icon_family.c_str() : g_ui_family.c_str();
    IDWriteFontCollection *coll = icons ? NULL : g_ui_fonts.Get();
    if (FAILED(g_dw->CreateTextFormat(family, coll, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"es-es", &f))) return f;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!icons) {
        DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> dots;
        g_dw->CreateEllipsisTrimmingSign(f.Get(), &dots);
        f->SetTrimming(&tr, dots.Get());
    }
    return f;
}
static void make_formats() {
    g_widths.clear();
    g_f.tab = make_format(false, S(12), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.addr = make_format(false, S(13), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.icon = make_format(true, S(16), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.icon_sm = make_format(true, S(10), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.icon_lg = make_format(true, S(20), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.menu = make_format(false, S(12.5f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.hint = make_format(false, S(11.5f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_f.word = make_format(false, S(30), DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.ntp = make_format(false, S(13), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.ntp_sub = make_format(false, S(12), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_f.notice = make_format(false, S(12.5f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (g_edit_font) DeleteObject(g_edit_font);
    g_edit_font = CreateFontW(-(int)lroundf(S(13)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_ui_family.c_str());
}
static bool has_family(IDWriteFontCollection *c, const std::wstring &name) {
    UINT32 idx; BOOL exists = FALSE;
    return c && SUCCEEDED(c->FindFamilyName(name.c_str(), &idx, &exists)) && exists;
}
static void load_fonts() {
    ComPtr<IDWriteFontCollection> sys;
    g_dw->GetSystemFontCollection(&sys);
    ComPtr<IDWriteFactory5> f5;
    if (!g_own_fonts && SUCCEEDED(g_dw.As(&f5))) {
        ComPtr<IDWriteFontSetBuilder1> b;
        ComPtr<IDWriteFontSet> set;
        ComPtr<IDWriteFontCollection1> coll;
        if (SUCCEEDED(f5->CreateFontSetBuilder(&b))) {
            for (const wchar_t *name : { L"Roboto.ttf", L"RobotoMono.ttf" }) {
                std::wstring path = g_root + L"\\fuentes\\" + name;
                ComPtr<IDWriteFontFile> file;
                if (!PathFileExistsW(path.c_str()) || FAILED(f5->CreateFontFileReference(path.c_str(), NULL, &file))) continue;
                b->AddFontFile(file.Get());
                AddFontResourceExW(path.c_str(), FR_PRIVATE, 0);   // the address field is a Windows edit control (GDI)
            }
            if (SUCCEEDED(b->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll))) g_own_fonts = coll;
        }
    }
    // the chosen family, from the fuentes folder or Windows; else Windows's own (Segoe UI Variable on Windows 11)
    g_ui_fonts.Reset();
    if (!g_set.font.empty() && has_family(g_own_fonts.Get(), g_set.font)) { g_ui_family = g_set.font; g_ui_fonts = g_own_fonts; }
    else if (!g_set.font.empty() && has_family(sys.Get(), g_set.font)) g_ui_family = g_set.font;
    else g_ui_family = has_family(sys.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    if (!has_family(sys.Get(), L"Segoe Fluent Icons")) g_icon_family = L"Segoe MDL2 Assets";
}

// one bitmap from PNG bytes (favicons) or a file (extension icons), for a given render target
static ComPtr<ID2D1Bitmap> decode(ID2D1RenderTarget *rt, const std::string *bytes, const std::wstring *file) {
    ComPtr<ID2D1Bitmap> out;
    ComPtr<IWICBitmapDecoder> dec;
    if (bytes) {
        ComPtr<IStream> s;
        s.Attach(SHCreateMemStream((const BYTE *)bytes->data(), (UINT)bytes->size()));
        if (!s || FAILED(g_wic->CreateDecoderFromStream(s.Get(), NULL, WICDecodeMetadataCacheOnLoad, &dec))) return out;
    } else if (FAILED(g_wic->CreateDecoderFromFilename(file->c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) return out;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(dec->GetFrame(0, &fr)) || FAILED(g_wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeMedianCut)))
        return out;
    rt->CreateBitmapFromWicBitmap(conv.Get(), NULL, &out);
    return out;
}

// ---------------------------------------------------------------- drawing helpers
static D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
static bool inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void fill(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    if (c.a <= 0.001f) return;
    b->SetColor(c);
    if (rad > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b); else rt->FillRectangle(r, b);
}
static void stroke(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w) {
    b->SetColor(c);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(R(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2), rad, rad), b, w);
}
static void text(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, IDWriteTextFormat *f, const std::wstring &s, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (s.empty() || !f || r.right <= r.left) return;
    b->SetColor(c);
    rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static void glyph(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, IDWriteTextFormat *f, wchar_t g, D2D1_RECT_F r, D2D1_COLOR_F c) {
    b->SetColor(c);
    rt->DrawText(&g, 1, f, r, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
}
static float text_w(IDWriteTextFormat *f, const std::wstring &s) {
    wchar_t key[24]; swprintf_s(key, L"%p|", (void *)f);
    std::wstring k = key + s;
    auto it = g_widths.find(k);
    if (it != g_widths.end()) return it->second;
    if (g_widths.size() > 4000) g_widths.clear();
    ComPtr<IDWriteTextLayout> l;
    if (s.empty() || FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 100000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m; l->GetMetrics(&m);
    g_widths[k] = m.widthIncludingTrailingWhitespace;
    return m.widthIncludingTrailingWhitespace;
}
// a soft, lifted shadow (Mapea's --glass-shadow: 0 8px 28px at 10 %) as rings of falling opacity
static void soft_shadow(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, D2D1_RECT_F r, float rad, float strength) {
    const int n = 10;
    for (int i = n; i >= 1; i--) {
        float e = S(1.6f) * i, k = (float)(n - i + 1) / n;
        D2D1_RECT_F q = R(r.left - e, r.top - e + S(5), r.right + e, r.bottom + e + S(5));
        fill(rt, b, q, rad + e, hex(dark_theme() ? 0x000000 : 0x1d2733, 0.012f * k * strength * (dark_theme() ? 2.4f : 1)));
    }
}

// ---------------------------------------------------------------- state of the bar
struct Ghost { float w, x = 0; int slot; std::wstring title; ComPtr<ID2D1Bitmap> icon; Anim k; };   // a tab closing
static std::vector<Ghost> g_ghosts;
static std::unordered_map<int, ComPtr<ID2D1Bitmap>> g_icons;      // tab id -> favicon on the bar's target
static std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> g_host_icons, g_ext_icons;
static Anim g_sheet_k, g_notice_h, g_addr_focus, g_menu_btn_open;
static int g_sheet_from = 0, g_sheet_to = 0;     // tab ids the sheet slides between
static float g_frozen_w;                          // after closing with the mouse, the tabs keep their width until it leaves
enum Hit { H_NONE, H_TAB, H_TAB_CLOSE, H_TAB_AUDIO, H_PLUS, H_BACK, H_FWD, H_RELOAD, H_ADDR, H_ZOOM, H_MENU, H_EXT,
           H_MIN, H_MAX, H_CLOSE, H_NOTICE_BTN, H_NOTICE_X, H_NTP_ROW, H_STAR, H_NTP_X };
struct HitInfo { Hit what = H_NONE; int id = 0; };
static HitInfo g_hover, g_press;
static std::unordered_map<int, Anim> g_btn_hover;  // per control
static int g_caption_hot, g_caption_down;
static bool g_tracking, g_dragging, g_active_window = true;
static int g_drag_id; static float g_drag_dx, g_drag_x, g_press_x;
struct Notice { std::wstring text, button; std::function<void()> action; double until = 0; bool on = false; int owner = 0; };
static Notice g_notice;
static std::wstring g_addr_text;                  // what the address shows when not being typed in
static std::vector<HistoryEntry> g_ntp_rows;
static bool g_ntp_pins, g_ntp_filled;            // the rows are the person's own pages (else suggestions); computed
static double g_ntp_mem_at = -1e9; static size_t g_ntp_mem;
static WNDPROC g_edit_proc;
static bool g_in_change;
static std::wstring g_typed;                      // what the person typed (without the inline completion)
static void close_suggestions();
static void update_suggestions();
static void sug_move(int d);
static bool sug_commit(bool new_tab);
static void open_menu_page(int page, RECT anchor, int tab_id = 0);
static void menu_close();

static int hover_key(Hit h, int id) { return (int)h * 100000 + id; }
static float hov(Hit h, int id) {
    auto it = g_btn_hover.find(hover_key(h, id));
    return it == g_btn_hover.end() ? 0 : it->second.at(now_ms());
}
static bool page_on_screen() {
    Tab *t = active_tab();
    return t && !t->url.empty() && (t->view || t->creating || !t->pending.empty());
}
static bool ntp_on_screen() { Tab *t = active_tab(); return !g_fullscreen && (!t || t->url.empty()); }
float ui_band_height() {
    if (g_fullscreen) return 0;
    // the note's room is taken at once (the page is laid out once, not every frame); the card slides into it
    return S(kTabRowH + kToolRowH) + S(kNoticeH) * g_notice_h.to;
}

// ---------------------------------------------------------------- layout of the tab row
struct Geo { float W, strip_l, strip_r, cap_l, tab_w, plus_x, addr_l, addr_r, menu_x, ext_l; };
static Geo g_geo;
static size_t ext_buttons() { size_t n = 0; for (auto &e : g_exts) if (e.enabled && !e.popup.empty()) n++; return std::min<size_t>(n, 4); }
static void compute_geo() {
    RECT rc; GetClientRect(g_bar, &rc);
    Geo g;
    g.W = (float)rc.right;
    g.cap_l = g.W - S(kCapW * 3);
    g.strip_l = S(8);
    g.strip_r = g.cap_l - S(8 + kPlusW + 6);
    float avail = g.strip_r - g.strip_l;
    size_t n = std::max<size_t>(1, g_tabs.size());
    float w = std::min(S(kTabMax), std::max(S(kTabMin), (avail - S(kTabGap) * (n - 1)) / n));
    if (g_frozen_w > 0 && g_frozen_w < w) w = g_frozen_w;
    g.tab_w = w;
    g.menu_x = g.W - S(8 + kBtn);
    g.ext_l = g.menu_x - S(4) - ext_buttons() * S(kBtn + 2);
    g.addr_l = S(8 + (kBtn + 4) * 3 + 2);
    g.addr_r = g.ext_l - S(6);
    g_geo = g;
    for (auto &t : g_tabs) {
        if (!t->placed) { t->w.snap(0); t->placed = true; }
        t->w.go(w, kTabMs);
    }
}
// where each tab is drawn now: widths animate, positions follow (ghosts keep their slot while they shrink).
// settled: where they end up, for clicks: closing tabs fast must not wait for the animation (the next tab's cross
// is under the mouse at once, as in Chrome)
struct Slot { Tab *t; float x, w; };
static float g_strip_end;
static std::vector<Slot> tab_slots(double now, bool settled = false) {
    std::vector<Slot> out;
    float x = g_geo.strip_l;
    size_t gi = 0;
    std::vector<Ghost *> gs;
    for (auto &g : g_ghosts) gs.push_back(&g);
    std::sort(gs.begin(), gs.end(), [](Ghost *a, Ghost *b) { return a->slot < b->slot; });
    for (size_t i = 0; i <= g_tabs.size(); i++) {
        while (!settled && gi < gs.size() && gs[gi]->slot <= (int)i) { gs[gi]->x = x; x += (gs[gi]->w + S(kTabGap)) * gs[gi]->k.at(now); gi++; }
        if (i == g_tabs.size()) break;
        Tab *t = g_tabs[i].get();
        float w = settled ? t->w.to : t->w.at(now);
        float dx = !settled && t->x.running(now) ? t->x.at(now) : 0;   // a reorder slides from where the tab was
        out.push_back({ t, x + dx, w });
        x += w + S(kTabGap);
    }
    if (!settled) g_strip_end = std::max(g_geo.strip_l, x - S(kTabGap));
    for (auto &s : out)
        if (g_dragging && s.t->id == g_drag_id) s.x = std::max(g_geo.strip_l, std::min(g_drag_x - g_drag_dx, g_geo.strip_r - s.w));
    return out;
}
static float strip_end(double now) { tab_slots(now); return g_strip_end; }
static float settled_strip_end(double now) {
    auto sl = tab_slots(now, true);
    return sl.empty() ? g_geo.strip_l : sl.back().x + sl.back().w;
}

// ---------------------------------------------------------------- rectangles of the controls
static D2D1_RECT_F tab_rect(const Slot &s) { return R(s.x, S(kTabY), s.x + s.w, S(kTabY + kTabH)); }
static D2D1_RECT_F tab_close_rect(const Slot &s) {
    float c = S(kTabY + kTabH / 2);
    return R(s.x + s.w - S(26), c - S(10), s.x + s.w - S(6), c + S(10));
}
static bool tab_compact(const Slot &s) { return s.w < S(96); }
static D2D1_RECT_F tab_audio_rect(const Slot &s) {
    D2D1_RECT_F c = tab_close_rect(s);
    return R(c.left - S(22), c.top, c.left - S(2), c.bottom);
}
static D2D1_RECT_F plus_rect(double now, bool settled = false) {
    float x = (settled ? settled_strip_end(now) : strip_end(now)) + S(4);
    return R(x, S(kTabY), x + S(kPlusW), S(kTabY + kTabH));
}
static D2D1_RECT_F cap_rect(int i) { return R(g_geo.cap_l + S(kCapW) * i, 0, g_geo.cap_l + S(kCapW) * (i + 1), S(kTabRowH)); }
static float tool_y() { return S(kTabRowH); }
static D2D1_RECT_F btn_rect(int i) {   // 0 back, 1 forward, 2 reload
    float y = tool_y() + S((kToolRowH - kBtn) / 2);
    float x = S(8) + S(kBtn + 4) * i;
    return R(x, y, x + S(kBtn), y + S(kBtn));
}
static D2D1_RECT_F menu_rect() { float y = tool_y() + S((kToolRowH - kBtn) / 2); return R(g_geo.menu_x, y, g_geo.menu_x + S(kBtn), y + S(kBtn)); }
static D2D1_RECT_F ext_rect(size_t i) {
    float y = tool_y() + S((kToolRowH - kBtn) / 2);
    float x = g_geo.ext_l + S(2) + i * S(kBtn + 2);
    return R(x, y, x + S(kBtn), y + S(kBtn));
}
static D2D1_RECT_F addr_rect() {
    float y = tool_y() + S((kToolRowH - kAddrH) / 2);
    return R(g_geo.addr_l, y, g_geo.addr_r, y + S(kAddrH));
}
// the star at the end of the address: the page on the new tab page, or off it (Ctrl+D)
static bool star_shown() {
    Tab *t = active_tab();
    return t && !ui_address_focused() && (starts_with(t->url, L"https://") || starts_with(t->url, L"http://"));
}
static D2D1_RECT_F star_rect() {
    D2D1_RECT_F a = addr_rect();
    return R(a.right - S(34), a.top + S(4), a.right - S(6), a.bottom - S(4));
}
static D2D1_RECT_F zoom_rect() {
    D2D1_RECT_F a = addr_rect();
    float r = star_shown() ? star_rect().left - S(4) : a.right - S(6);
    return R(r - S(56), a.top + S(5), r, a.bottom - S(5));
}
static bool zoom_shown() { double z = tab_zoom_factor(); return fabs(z - 1.0) > 0.01; }
static D2D1_RECT_F notice_rect() {
    float y = S(kTabRowH + kToolRowH);
    return R(0, y, g_geo.W, y + S(kNoticeH) * g_notice_h.to);
}
static D2D1_RECT_F notice_btn_rect() {
    D2D1_RECT_F n = notice_rect();
    float w = text_w(g_f.menu.Get(), g_notice.button) + S(28);
    float y = n.top + S((kNoticeH - 28) / 2);
    return R(g_geo.W - S(48) - w, y, g_geo.W - S(48), y + S(28));
}
static D2D1_RECT_F notice_x_rect() {
    D2D1_RECT_F n = notice_rect();
    float y = n.top + S((kNoticeH - 28) / 2);
    return R(g_geo.W - S(40), y, g_geo.W - S(12), y + S(28));
}
// the new tab page: one column, the most visited sites and what Pluma is using
static float ntp_col_l() { return std::max(S(24), (g_geo.W - S(560)) / 2); }
static const float kNtpIncognitoH = 90;   // the incognito page's words, above the rows
static float ntp_top() {
    RECT rc; GetClientRect(g_bar, &rc);
    bool incognito = active_tab() && active_tab()->incognito;
    return ui_band_height() + std::max(S(36), (rc.bottom - ui_band_height()) * 0.16f) + (incognito ? S(kNtpIncognitoH) : 0);
}
static D2D1_RECT_F ntp_row_rect(size_t i) {
    float l = ntp_col_l(), t = ntp_top() + S(118) + i * S(40);
    return R(l - S(12), t, l + S(560) + S(12), t + S(38));
}
static D2D1_RECT_F ntp_x_rect(size_t i) {   // the cross that takes a row off (on hover)
    D2D1_RECT_F r = ntp_row_rect(i);
    return R(r.right - S(38), r.top + S(5), r.right - S(10), r.bottom - S(5));
}
static void ntp_fill() {
    // the person's own pages, if they have any (only those); else, if they want them, the sites they visit most
    g_ntp_pins = !g_pins.empty();
    bool incognito = active_tab() && active_tab()->incognito;   // (incognito: their own pages, never what they visit most)
    if (g_ntp_pins) g_ntp_rows.assign(g_pins.begin(), g_pins.begin() + std::min<size_t>(g_pins.size(), 12));
    else if (g_set.suggest_top && !incognito) g_ntp_rows = history_top(8);
    else g_ntp_rows.clear();
    // (none yet: again at the next layout, the history may not be loaded yet when the first new tab shows)
    g_ntp_filled = !g_ntp_rows.empty() || !g_set.suggest_top;
}

static HitInfo hit_test(float x, float y) {
    HitInfo h;
    double now = now_ms();
    if (g_fullscreen) return h;
    if (y < S(kTabRowH)) {
        for (int i = 0; i < 3; i++) if (inside(cap_rect(i), x, y)) { h.what = (Hit)(H_MIN + i); return h; }
        for (auto &s : tab_slots(now, true)) {
            D2D1_RECT_F r = tab_rect(s);
            if (!inside(r, x, y)) continue;
            h.id = s.t->id;
            bool active = s.t == active_tab();
            if ((!tab_compact(s) || active) && inside(tab_close_rect(s), x, y)) h.what = H_TAB_CLOSE;
            else if (s.t->audible && !tab_compact(s) && inside(tab_audio_rect(s), x, y)) h.what = H_TAB_AUDIO;
            else h.what = H_TAB;
            return h;
        }
        if (inside(plus_rect(now, true), x, y)) h.what = H_PLUS;
        return h;
    }
    if (y < S(kTabRowH + kToolRowH)) {
        for (int i = 0; i < 3; i++) if (inside(btn_rect(i), x, y)) { h.what = (Hit)(H_BACK + i); return h; }
        if (inside(menu_rect(), x, y)) { h.what = H_MENU; return h; }
        size_t k = 0;
        for (size_t i = 0; i < g_exts.size() && k < ext_buttons(); i++) {
            if (!g_exts[i].enabled || g_exts[i].popup.empty()) continue;
            if (inside(ext_rect(k), x, y)) { h.what = H_EXT; h.id = (int)i; return h; }
            k++;
        }
        if (star_shown() && inside(star_rect(), x, y)) { h.what = H_STAR; return h; }
        if (zoom_shown() && inside(zoom_rect(), x, y)) { h.what = H_ZOOM; return h; }
        if (inside(addr_rect(), x, y)) { h.what = H_ADDR; return h; }
        return h;
    }
    if (g_notice.on && y < S(kTabRowH + kToolRowH + kNoticeH)) {
        if (inside(notice_btn_rect(), x, y)) h.what = H_NOTICE_BTN;
        else if (inside(notice_x_rect(), x, y)) h.what = H_NOTICE_X;
        return h;
    }
    if (ntp_on_screen())
        for (size_t i = 0; i < g_ntp_rows.size(); i++)
            if (inside(ntp_row_rect(i), x, y)) { h.what = inside(ntp_x_rect(i), x, y) ? H_NTP_X : H_NTP_ROW; h.id = (int)i; return h; }
    return h;
}
int ui_hit_caption(POINT p) {
    if (p.y >= (LONG)S(kTabRowH)) return HTCLIENT;
    HitInfo h = hit_test((float)p.x, (float)p.y);
    if (h.what == H_MIN) return HTMINBUTTON;
    if (h.what == H_MAX) return HTMAXBUTTON;
    if (h.what == H_CLOSE) return HTCLOSE;
    if (h.what == H_NONE) return HTCAPTION;
    return HTCLIENT;
}

// ---------------------------------------------------------------- painting the bar
static ComPtr<ID2D1Bitmap> tab_icon(Tab *t) {
    auto it = g_icons.find(t->id);
    if (it != g_icons.end()) return it->second;
    ComPtr<ID2D1Bitmap> b;
    if (!t->icon_png.empty()) b = decode(g_rt.Get(), &t->icon_png, NULL);
    g_icons[t->id] = b;
    return b;
}
static ComPtr<ID2D1Bitmap> host_icon(ID2D1RenderTarget *rt, std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> &cache, const std::wstring &url) {
    std::wstring host = url_host(url);
    auto it = cache.find(host);
    if (it != cache.end()) return it->second;
    std::string png;
    ComPtr<ID2D1Bitmap> b;
    if (!host.empty() && read_file(g_state + L"\\iconos\\" + hash_hex(host) + L".png", png)) b = decode(rt, &png, NULL);
    cache[host] = b;
    return b;
}
static void draw_icon_bitmap(ID2D1RenderTarget *rt, ID2D1Bitmap *b, float cx, float cy, float size, float a) {
    D2D1_RECT_F r = R(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2);
    rt->DrawBitmap(b, r, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}
static void draw_spinner(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, float cx, float cy, float r, double now, D2D1_COLOR_F c) {
    // an arc of three quarters around the origin, made once per size, turned once a second
    static ComPtr<ID2D1PathGeometry> g;
    static ComPtr<ID2D1StrokeStyle> ss;
    static float made_r = 0;
    if (!g || made_r != r) {
        g.Reset();
        ComPtr<ID2D1GeometrySink> sk;
        if (FAILED(g_d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&sk))) { g.Reset(); return; }
        sk->BeginFigure(D2D1::Point2F(r, 0), D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(r * cosf(4.2f), r * sinf(4.2f)), D2D1::SizeF(r, r), 0,
                                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
        sk->Close();
        made_r = r;
    }
    if (!ss) g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), NULL, 0, &ss);
    float deg = (float)fmod(now / 1000.0, 1.0) * 360.0f;
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(deg) * D2D1::Matrix3x2F::Translation(cx, cy) * old);
    b->SetColor(c);
    rt->DrawGeometry(g.Get(), b, S(1.6f), ss.Get());
    rt->SetTransform(old);
}
static D2D1_RECT_F lerp_rect(D2D1_RECT_F a, D2D1_RECT_F b, float k) {
    return R(a.left + (b.left - a.left) * k, a.top + (b.top - a.top) * k, a.right + (b.right - a.right) * k, a.bottom + (b.bottom - a.bottom) * k);
}
static bool paint_tabs(ID2D1RenderTarget *rt, double now) {
    const Palette &p = pal();
    bool busy = false;
    auto slots = tab_slots(now);
    Tab *act = active_tab();
    // the sheet under the active tab, sliding from the previous one
    D2D1_RECT_F to{}, from{};
    bool have_to = false, have_from = false;
    for (auto &s : slots) {
        if (s.t == act) { to = tab_rect(s); have_to = true; }
        if (s.t->id == g_sheet_from) { from = tab_rect(s); have_from = true; }
    }
    if (have_to) {
        float k = g_sheet_k.at(now);
        D2D1_RECT_F r = have_from && k < 1 ? lerp_rect(from, to, k) : to;
        busy |= g_sheet_k.running(now);
        fill(rt, g_br.Get(), R(r.left, r.top + S(1), r.right, r.bottom + S(1)), S(9), alpha(hex(dark_theme() ? 0x000000 : 0x1d2733), 0.07f * (dark_theme() ? 3 : 1)));
        fill(rt, g_br.Get(), r, S(9), p.surface);
        stroke(rt, g_br.Get(), r, S(9), alpha(p.text, dark_theme() ? 0.08f : 0.06f), S(1));
    }
    for (auto &g : g_ghosts) busy |= g.k.running(now);
    for (auto &s : slots) {
        Tab *t = s.t;
        D2D1_RECT_F r = tab_rect(s);
        if (r.right - r.left < S(4)) continue;
        bool active = t == act;
        busy |= t->w.running(now) || t->x.running(now) || t->hover.running(now) || t->close_hover.running(now);
        float hv = t->hover.at(now);
        if (!active && hv > 0) fill(rt, g_br.Get(), r, S(9), alpha(p.hover, hv));
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        bool compact = tab_compact(s);
        float cy = (r.top + r.bottom) / 2;
        float ix = compact && !active ? (r.left + r.right) / 2 : r.left + S(18);
        if (t->loading && t->view) { draw_spinner(rt, g_br.Get(), ix, cy, S(6.5f), now, p.accent); busy = true; }
        else if (t->incognito) {
            // incognito: Edge's InPrivate sign instead of the site's icon, in the accent
            glyph(rt, g_br.Get(), g_f.icon.Get(), G_INPRIVATE, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), p.accent);
        } else {
            auto b = tab_icon(t);
            if (b) draw_icon_bitmap(rt, b.Get(), ix, cy, S(16), 1);
            else glyph(rt, g_br.Get(), g_f.icon.Get(), t->url.empty() ? G_NEWTAB : G_GLOBE, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), p.sub);
        }
        if (!compact) {
            float right = r.right - S(30);
            if (t->audible || t->muted) {
                D2D1_RECT_F ar = tab_audio_rect(s);
                float ah = hov(H_TAB_AUDIO, t->id);
                if (ah > 0) fill(rt, g_br.Get(), ar, S(10), alpha(p.hover, ah * 1.4f));
                glyph(rt, g_br.Get(), g_f.icon_sm.Get(), t->muted ? G_MUTE : G_VOL, ar, active ? p.text : p.sub);
                right = ar.left - S(2);
            }
            std::wstring title = !t->title.empty() ? t->title : t->url.empty() ? T(L"Pestaña nueva") : strip_scheme(t->url);
            text(rt, g_br.Get(), g_f.tab.Get(), title, R(r.left + S(32), r.top, right, r.bottom), active ? p.text : p.sub);
        }
        if (!compact || active) {
            float ch = t->close_hover.at(now);
            float vis = active ? 1 : std::max(hv, ch);
            if (vis > 0) {
                D2D1_RECT_F c = tab_close_rect(s);
                if (ch > 0) fill(rt, g_br.Get(), c, S(10), alpha(p.press, ch));
                glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_X, c, alpha(active ? p.text : p.sub, vis));
            }
        }
        rt->PopAxisAlignedClip();
    }
    // closing tabs shrink and fade where they were
    for (auto &g : g_ghosts) {
        float k = g.k.at(now);
        if (k <= 0) continue;
        D2D1_RECT_F r = R(g.x, S(kTabY), g.x + g.w * k, S(kTabY + kTabH));
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        if (g.icon) draw_icon_bitmap(rt, g.icon.Get(), r.left + S(18), (r.top + r.bottom) / 2, S(16), k * 0.6f);
        text(rt, g_br.Get(), g_f.tab.Get(), g.title, R(r.left + S(32), r.top, r.left + g.w - S(30), r.bottom), alpha(p.sub, k * 0.6f));
        rt->PopAxisAlignedClip();
    }
    // new tab
    D2D1_RECT_F pr = plus_rect(now);
    float ph = hov(H_PLUS, 0);
    if (ph > 0) fill(rt, g_br.Get(), pr, S(9), alpha(p.hover, ph * 1.4f));
    glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_ADD, pr, p.sub);
    return busy;
}
static bool paint_caption(ID2D1RenderTarget *rt, double now) {
    const Palette &p = pal();
    bool busy = false;
    wchar_t gl[3] = { G_MIN, (wchar_t)(IsZoomed(g_win) ? G_RESTORE : G_MAX), G_X };
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = cap_rect(i);
        auto &a = g_btn_hover[hover_key((Hit)(H_MIN + i), 0)];
        busy |= a.running(now);
        float h = a.at(now);
        bool down = g_caption_down == (i == 0 ? HTMINBUTTON : i == 1 ? HTMAXBUTTON : HTCLOSE);
        D2D1_COLOR_F ink = g_active_window ? p.text : p.sub;
        if (i == 2) {
            fill(rt, g_br.Get(), r, 0, alpha(down ? mix(p.error, hex(0x000000), 0.15f) : p.error, h));
            ink = mix(ink, hex(0xffffff), h);
        } else fill(rt, g_br.Get(), r, 0, alpha(down ? p.press : p.hover, h * 1.2f));
        glyph(rt, g_br.Get(), g_f.icon_sm.Get(), gl[i], r, ink);
    }
    return busy;
}
static void paint_button(ID2D1RenderTarget *rt, D2D1_RECT_F r, Hit h, int id, wchar_t g, bool enabled, double now, bool *busy) {
    const Palette &p = pal();
    auto &a = g_btn_hover[hover_key(h, id)];
    *busy |= a.running(now);
    float hv = enabled ? a.at(now) : 0;
    bool down = g_press.what == h && g_press.id == id && g_hover.what == h;
    if (hv > 0 || down) fill(rt, g_br.Get(), r, S(8), down ? p.press : alpha(p.hover, hv * 1.3f));
    glyph(rt, g_br.Get(), g_f.icon.Get(), g, r, enabled ? p.text : alpha(p.sub, 0.45f));
}
static std::wstring strip_scheme(const std::wstring &u) {
    std::wstring s = starts_with(u, L"https://") ? u.substr(8) : u;
    size_t slash = s.find(L'/');
    if (slash != std::wstring::npos && slash + 1 == s.size()) s.pop_back();   // "example.com/" reads "example.com"
    return pretty_path(s);
}
static std::wstring pretty_path(const std::wstring &s) {
    // what people read: "/wiki/España", not "/wiki/Espa%C3%B1a" (only for showing it; the address itself, the one
    // that is edited and copied, stays as it is; and so does anything that does not decode to valid text)
    if (s.find(L'%') == std::wstring::npos) return s;
    std::string bytes;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            bytes += (char)wcstol(s.substr(i + 1, 2).c_str(), NULL, 16);
            i += 2;
        } else if (s[i] < 0x80) bytes += (char)s[i];
        else return s;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), NULL, 0);
    if (n <= 0) return s;
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), out.data(), n);
    for (wchar_t c : out) if (c < 0x20 || c == 0x7F) return s;
    return out;
}
static bool paint_toolbar(ID2D1RenderTarget *rt, double now) {
    const Palette &p = pal();
    bool busy = false;
    Tab *t = active_tab();
    paint_button(rt, btn_rect(0), H_BACK, 0, G_BACK, t && t->can_back, now, &busy);
    paint_button(rt, btn_rect(1), H_FWD, 0, G_FWD, t && t->can_fwd, now, &busy);
    paint_button(rt, btn_rect(2), H_RELOAD, 0, t && t->loading ? G_STOP : G_RELOAD, t && !t->url.empty(), now, &busy);
    paint_button(rt, menu_rect(), H_MENU, 0, G_MORE, true, now, &busy);
    size_t k = 0;
    for (size_t i = 0; i < g_exts.size() && k < ext_buttons(); i++) {
        Extension &e = g_exts[i];
        if (!e.enabled || e.popup.empty()) continue;
        D2D1_RECT_F r = ext_rect(k++);
        auto &a = g_btn_hover[hover_key(H_EXT, (int)i)];
        busy |= a.running(now);
        if (a.at(now) > 0) fill(rt, g_br.Get(), r, S(8), alpha(p.hover, a.at(now) * 1.3f));
        ComPtr<ID2D1Bitmap> b;
        auto it = g_ext_icons.find(e.id);
        if (it != g_ext_icons.end()) b = it->second;
        else { if (!e.icon_path.empty()) b = decode(rt, NULL, &e.icon_path); g_ext_icons[e.id] = b; }
        if (b) draw_icon_bitmap(rt, b.Get(), (r.left + r.right) / 2, (r.top + r.bottom) / 2, S(18), 1);
        else glyph(rt, g_br.Get(), g_f.icon.Get(), G_PUZZLE, r, p.text);
    }
    // the address: a sheet of paper like the active tab; when typed in, a ring of the accent
    D2D1_RECT_F a = addr_rect();
    float fk = g_addr_focus.at(now);
    busy |= g_addr_focus.running(now);
    float ah = hov(H_ADDR, 0);
    fill(rt, g_br.Get(), R(a.left, a.top + S(1), a.right, a.bottom + S(1)), S(10), alpha(hex(dark_theme() ? 0x000000 : 0x1d2733), 0.07f * (dark_theme() ? 3 : 1)));
    fill(rt, g_br.Get(), a, S(10), p.surface);
    stroke(rt, g_br.Get(), a, S(10), mix(mix(p.line, p.line_strong, ah * 0.6f), p.accent, fk), S(1));
    if (fk > 0) stroke(rt, g_br.Get(), R(a.left - S(3), a.top - S(3), a.right + S(3), a.bottom + S(3)), S(13), alpha(p.accent_soft, 0.45f * fk), S(3));
    // loading: a line along the foot of the address, inside it
    if (t && (t->loading || t->load_done > 0)) {
        double el = now - t->load_started;
        float pk = 1, fade = 1;
        if (t->loading) {
            float target = (float)(0.15 + 0.7 * (1 - exp(-el / 2500.0)));
            if (target > t->progress) t->progress = target;
            pk = std::min(1.0f, t->progress);
        } else {
            fade = (float)std::max(0.0, 1 - (now - t->load_done) / 260.0);
            if (fade <= 0) t->load_done = 0;
        }
        if (fade > 0) {
            // the line follows the curve of the address's corners: drawn as a thin rounded bar inside it
            float x1 = a.left + (a.right - a.left) * pk;
            float inset = S(10) * 0.29f;   // where a radius-10 corner is 2 px tall
            D2D1_RECT_F line = R(a.left + inset, a.bottom - S(2) - S(0.5f), std::max(a.left + inset, std::min(x1, a.right - inset)), a.bottom - S(0.5f));
            fill(rt, g_br.Get(), line, S(1), alpha(p.accent, fade));
        }
        busy = true;
    }
    bool focused = ui_address_focused();
    float ix = a.left + S(20), cy = (a.top + a.bottom) / 2;
    std::wstring u = t ? t->url : L"";
    wchar_t ig = G_SEARCH;
    D2D1_COLOR_F ic = p.sub;
    if (!focused && !u.empty()) {
        if (starts_with(u, L"https://")) ig = G_LOCK;
        else if (starts_with(u, L"http://")) { ig = G_INFO; ic = p.error; }
        else if (starts_with(u, L"file:")) ig = G_FILE;
        else if (starts_with(u, L"chrome-extension:")) ig = G_PUZZLE;
        else ig = G_INFO;
    }
    glyph(rt, g_br.Get(), g_f.icon_sm.Get(), ig, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), ic);
    float right = a.right - S(12);
    if (star_shown()) {
        // the star: filled in the accent when the page is one of the person's own on the new tab page
        D2D1_RECT_F s = star_rect();
        auto &sa = g_btn_hover[hover_key(H_STAR, 0)];
        busy |= sa.running(now);
        float sh = sa.at(now);
        if (sh > 0) fill(rt, g_br.Get(), s, S(7), alpha(p.hover, sh * 1.3f));
        bool on = pins_has(t->url);
        glyph(rt, g_br.Get(), g_f.icon_sm.Get(), on ? G_STAR_FILL : G_STAR, s, on ? p.accent : mix(p.sub, p.text, sh));
        right = s.left - S(4);
    }
    if (zoom_shown()) {
        D2D1_RECT_F z = zoom_rect();
        float zh = hov(H_ZOOM, 0);
        fill(rt, g_br.Get(), z, S(7), mix(p.bg, p.press, zh));
        wchar_t zs[16]; swprintf_s(zs, L"%d %%", (int)lround(tab_zoom_factor() * 100));
        auto f = g_f.hint;
        text(rt, g_br.Get(), f.Get(), zs, R(z.left, z.top, z.right - S(8), z.bottom), p.sub);
        right = z.left - S(6);
    }
    if (t && t->incognito && !focused) {
        // a pill that says so, at the end of the address, as Edge's "InPrivate"
        float tw = text_w(g_f.hint.Get(), T(L"Incógnito"));
        D2D1_RECT_F pr = R(right - tw - S(36), a.top + S(5), right, a.bottom - S(5));
        fill(rt, g_br.Get(), pr, S(7), alpha(p.accent_soft, 0.55f));
        glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_INPRIVATE, R(pr.left + S(4), pr.top, pr.left + S(26), pr.bottom), p.accent);
        text(rt, g_br.Get(), g_f.hint.Get(), T(L"Incógnito"), R(pr.left + S(28), pr.top, pr.right - S(4), pr.bottom), p.accent);
        right = pr.left - S(6);
    }
    if (!focused) {
        D2D1_RECT_F tr = R(a.left + S(36), a.top, right, a.bottom);
        if (u.empty()) text(rt, g_br.Get(), g_f.addr.Get(), T(L"Busca o escribe una dirección"), tr, p.sub);
        else {
            // the site in ink, the rest of the address in grey
            std::wstring shown = strip_scheme(u);
            size_t cut = std::wstring::npos;
            if (starts_with(u, L"https://") || starts_with(u, L"http://")) {
                size_t start = starts_with(u, L"https://") ? 0 : 7;
                cut = shown.find_first_of(L"/?#", start);
            }
            if (cut == std::wstring::npos) text(rt, g_br.Get(), g_f.addr.Get(), shown, tr, p.text);
            else {
                std::wstring site = shown.substr(0, cut), rest = pretty_path(shown.substr(cut));
                if (rest == L"/") rest.clear();
                float sw = text_w(g_f.addr.Get(), site);
                text(rt, g_br.Get(), g_f.addr.Get(), site, R(tr.left, tr.top, std::min(tr.right, tr.left + sw + 1), tr.bottom), p.text);
                if (tr.left + sw < tr.right) text(rt, g_br.Get(), g_f.addr.Get(), rest, R(tr.left + sw, tr.top, tr.right, tr.bottom), p.sub);
            }
        }
    }
    return busy;
}
static bool paint_notice(ID2D1RenderTarget *rt, double now) {
    float k = g_notice_h.at(now);
    bool busy = g_notice_h.running(now);
    if (k <= 0) return busy;
    const Palette &p = pal();
    D2D1_RECT_F n = notice_rect();
    rt->PushAxisAlignedClip(n, D2D1_ANTIALIAS_MODE_ALIASED);
    float off = S(kNoticeH) * (1 - k);   // slides down from under the toolbar
    D2D1_RECT_F inner = R(S(8), n.top - off + S(2), g_geo.W - S(8), n.top - off + S(kNoticeH) - S(6));
    fill(rt, g_br.Get(), inner, S(10), p.surface);
    stroke(rt, g_br.Get(), inner, S(10), p.line, S(1));
    glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_INFO, R(inner.left + S(8), inner.top, inner.left + S(32), inner.bottom), p.accent);
    text(rt, g_br.Get(), g_f.notice.Get(), g_notice.text, R(inner.left + S(36), inner.top, notice_btn_rect().left - S(12), inner.bottom), p.text);
    D2D1_RECT_F b = notice_btn_rect(); b.top -= off; b.bottom -= off;
    float bh = hov(H_NOTICE_BTN, 0);
    fill(rt, g_br.Get(), b, S(8), mix(p.accent, hex(0x000000), 0.12f * bh));
    auto f = g_f.menu;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text(rt, g_br.Get(), f.Get(), g_notice.button, b, hex(0xffffff));
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    D2D1_RECT_F x = notice_x_rect(); x.top -= off; x.bottom -= off;
    float xh = hov(H_NOTICE_X, 0);
    if (xh > 0) fill(rt, g_br.Get(), x, S(8), alpha(p.hover, xh * 1.3f));
    glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_X, x, p.sub);
    rt->PopAxisAlignedClip();
    return busy;
}
static bool paint_ntp(ID2D1RenderTarget *rt, double now) {
    const Palette &p = pal();
    bool busy = false;
    float l = ntp_col_l(), top = ntp_top();
    Tab *at = active_tab();
    if (at && at->incognito) {
        // what incognito does, and what it does not (as Chrome's page says it); ntp_top() leaves room for it
        float y = top - S(kNtpIncognitoH);
        glyph(rt, g_br.Get(), g_f.icon_lg.Get(), G_INPRIVATE, R(l - S(4), y + S(2), l + S(36), y + S(42)), p.accent);
        text(rt, g_br.Get(), g_f.word.Get(), T(L"incógnito"), R(l + S(46), y, l + S(560), y + S(44)), p.text);
        const wchar_t *lines[] = { T(L"Pluma no guarda el historial ni las cookies de estas pestañas:"),
                                   T(L"todo se borra al cerrar la última o al cerrar Pluma."),
                                   T(L"Las descargas y tus páginas sí se quedan, y las webs y tu red"),
                                   T(L"siguen viendo lo que visitas.") };
        for (int k = 0; k < 4; k++) {
            float ly = y + S(52) + k * S(22) + (k >= 2 ? S(8) : 0);
            text(rt, g_br.Get(), g_f.ntp.Get(), lines[k], R(l, ly, l + S(560), ly + S(22)), p.sub);
        }
    } else {
        text(rt, g_br.Get(), g_f.word.Get(), L"pluma", R(l, top, l + S(560), top + S(44)), p.text);
        text(rt, g_br.Get(), g_f.ntp.Get(), T(L"Escribe una dirección arriba, o lo que quieras buscar."), R(l, top + S(46), l + S(560), top + S(70)), p.sub);
    }
    // the person's own pages (only those, once there is one), or the sites they visit most
    if (!g_ntp_rows.empty() || g_ntp_pins || !g_set.suggest_top)
        text(rt, g_br.Get(), g_f.ntp.Get(), g_ntp_pins || !g_set.suggest_top ? T(L"Tus páginas") : T(L"Lo que más visitas"), R(l, top + S(90), l + S(560), top + S(112)), p.sub);
    for (size_t i = 0; i < g_ntp_rows.size(); i++) {
        D2D1_RECT_F r = ntp_row_rect(i);
        auto &a = g_btn_hover[hover_key(H_NTP_ROW, (int)i)];
        auto &ax = g_btn_hover[hover_key(H_NTP_X, (int)i)];
        busy |= a.running(now) || ax.running(now);
        float hk = std::max(a.at(now), ax.at(now));   // the row is lit with the mouse over its cross too
        if (hk > 0) fill(rt, g_br.Get(), r, S(10), alpha(p.surface, hk));
        float cy = (r.top + r.bottom) / 2;
        auto b = host_icon(rt, g_host_icons, g_ntp_rows[i].url);
        if (b) draw_icon_bitmap(rt, b.Get(), l + S(8), cy, S(16), 1);
        else glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_GLOBE, R(l - S(2), cy - S(10), l + S(18), cy + S(10)), p.sub);
        std::wstring host = url_host(g_ntp_rows[i].url);
        if (starts_with(host, L"www.")) host = host.substr(4);
        float hw = std::min(S(220), text_w(g_f.ntp_sub.Get(), host) + S(4));
        std::wstring title = g_ntp_rows[i].title.empty() ? host : g_ntp_rows[i].title;
        // with the mouse on the row, its site gives way to the cross that takes it off
        float xw = S(44) * hk;
        text(rt, g_br.Get(), g_f.ntp.Get(), title, R(l + S(30), r.top, r.right - S(12) - hw - S(16) - xw, r.bottom), p.text);
        text(rt, g_br.Get(), g_f.ntp_sub.Get(), host, R(r.right - S(12) - hw - xw, r.top, r.right - S(12) - xw, r.bottom), alpha(p.sub, 1 - hk * 0.4f));
        if (hk > 0) {
            D2D1_RECT_F x = ntp_x_rect(i);
            if (ax.at(now) > 0) fill(rt, g_br.Get(), x, S(7), alpha(p.hover, ax.at(now) * 1.3f));
            glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_X, x, alpha(mix(p.sub, p.text, ax.at(now)), hk));
        }
    }
    // how to put their own pages there
    if (!g_ntp_pins) {
        float y = ntp_top() + S(118) + g_ntp_rows.size() * S(40) + S(10);
        text(rt, g_br.Get(), g_f.ntp.Get(), g_ntp_rows.empty() ? T(L"Añade tus páginas con la estrella de la barra (Ctrl+D).")
                                                                : T(L"¿Solo las tuyas? Añádelas con la estrella de la barra (Ctrl+D)."),
             R(l, y, l + S(560), y + S(24)), p.sub);
    }
    if (now - g_ntp_mem_at > 2500) { g_ntp_mem = engine_memory_mb(); g_ntp_mem_at = now; }
    RECT rc; GetClientRect(g_bar, &rc);
    std::wstring mem = T(L"Pluma usa ahora ") + std::to_wstring(g_ntp_mem) + T(L" MB de memoria");
    text(rt, g_br.Get(), g_f.ntp.Get(), mem, R(l, rc.bottom - S(48), l + S(560), rc.bottom - S(24)), p.sub);
    return busy;
}
static void release_bitmaps() {
    g_icons.clear(); g_host_icons.clear(); g_ext_icons.clear();
    for (auto &g : g_ghosts) g.icon.Reset();
}
// Animation frames: a thread waits for each refresh of the screen (DwmFlush) and asks the window's thread for
// one frame, only while something moves. The window never repaints in a loop, so nothing else waits (Windows
// gives timers their turn only when no painting is pending) and the motion keeps the screen's own rhythm.
static HANDLE g_anim_event;
static std::atomic<bool> g_anim_on{ false }, g_frame_pending{ false };
static bool g_bar_busy;
static void want_frames() {
    if (!g_anim_event) return;
    if (!g_anim_on.exchange(true)) SetEvent(g_anim_event);
}
static void anim_thread() {
    while (true) {
        WaitForSingleObject(g_anim_event, INFINITE);
        while (g_anim_on) {
            if (FAILED(DwmFlush())) Sleep(16);
            if (!g_frame_pending.exchange(true)) PostMessageW(g_bar, WM_APP + 22, 0, 0);
        }
    }
}
static double g_paint_ms, g_paint_max; static int g_paint_n;
static void paint() {
    LARGE_INTEGER q0, qf; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    struct Timer { LARGE_INTEGER q0, qf; ~Timer() {
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
        double ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
        static bool first = true;
        if (first) { first = false; return; }
        g_paint_ms += ms; g_paint_max = std::max(g_paint_max, ms);
        if (++g_paint_n == 600 && g_dev) {
            wchar_t b[96]; swprintf_s(b, L"barra: %.2f ms de media por fotograma, %.2f el peor (600 fotogramas)", g_paint_ms / g_paint_n, g_paint_max);
            log_line(b);
        }
        if (g_paint_n >= 600) { g_paint_n = 0; g_paint_ms = 0; g_paint_max = 0; }
    } } timer{ q0, qf };
    if (!g_rt) {
        RECT rc; GetClientRect(g_bar, &rc);
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(render_type(),
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_bar, D2D1::SizeU(std::max(1L, rc.right), std::max(1L, rc.bottom)),
                                                     D2D1_PRESENT_OPTIONS_IMMEDIATELY), &g_rt))) return;
        g_rt->CreateSolidColorBrush(hex(0), &g_br);
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    double now = now_ms();
    const Palette &p = pal();
    g_rt->BeginDraw();
    g_rt->Clear(p.bg);
    bool busy = g_theme.running(now) || g_notice_h.running(now);
    if (!g_fullscreen) {
        busy |= paint_tabs(g_rt.Get(), now);
        busy |= paint_caption(g_rt.Get(), now);
        busy |= paint_toolbar(g_rt.Get(), now);
        busy |= paint_notice(g_rt.Get(), now);
        if (ntp_on_screen()) busy |= paint_ntp(g_rt.Get(), now);
        // where the page starts, a hairline
        if (page_on_screen()) fill(g_rt.Get(), g_br.Get(), R(0, ui_band_height() - S(1), g_geo.W, ui_band_height()), 0, p.line);
    }
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { g_rt.Reset(); g_br.Reset(); release_bitmaps(); busy = true; }
    // remove finished ghosts
    g_ghosts.erase(std::remove_if(g_ghosts.begin(), g_ghosts.end(), [now](const Ghost &g) { return !g.k.running(now) && g.k.at(now) <= 0; }), g_ghosts.end());
    if (g_notice.on && g_notice.until > 0 && (double)GetTickCount64() > g_notice.until) { g_notice.on = false; g_notice_h.go(0, kTabMs); ui_layout(); busy = true; }
    g_bar_busy = busy || g_notice_h.running(now);
    if (g_bar_busy) want_frames();
}
void ui_invalidate() { if (g_bar) InvalidateRect(g_bar, NULL, FALSE); }
static void start_animation_thread() {
    g_anim_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    std::thread(anim_thread).detach();
}
void ui_post(std::function<void()> f) { PostMessageW(g_bar, WM_APP + 21, 0, (LPARAM)new std::function<void()>(std::move(f))); }

// ---------------------------------------------------------------- the address field
bool ui_address_focused() { return g_edit && GetFocus() == g_edit; }
static void place_edit() {
    D2D1_RECT_F a = addr_rect();
    int h = (int)lroundf(S(18));
    float right = zoom_shown() ? zoom_rect().left - S(6) : a.right - S(12);
    SetWindowPos(g_edit, NULL, (int)(a.left + S(36)), (int)((a.top + a.bottom) / 2 - h / 2.0f), (int)(right - a.left - S(36)), h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}
void ui_set_address_text(const std::wstring &s) {
    g_addr_text = s;
    if (!ui_address_focused()) { g_in_change = true; SetWindowTextW(g_edit, s.c_str()); g_in_change = false; }
    ui_invalidate();
}
void ui_focus_address(bool select_all) {
    if (!g_edit || g_fullscreen) return;
    Tab *t = active_tab();
    g_in_change = true;
    SetWindowTextW(g_edit, t ? t->url.c_str() : L"");
    g_in_change = false;
    g_typed.clear();
    place_edit();
    ShowWindow(g_edit, SW_SHOWNA);
    SetFocus(g_edit);
    SendMessageW(g_edit, EM_SETSEL, select_all ? 0 : GetWindowTextLengthW(g_edit), -1);
    g_addr_focus.go(1, kHoverMs);
    ui_invalidate();
}
static void blur_address(bool to_page) {
    close_suggestions();
    if (to_page) {
        Tab *t = active_tab();
        if (t && t->ctrl) t->ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        else SetFocus(g_win);
    }
}
static void commit_address(bool new_tab) {
    if (sug_commit(new_tab)) return;
    wchar_t buf[8192];
    GetWindowTextW(g_edit, buf, 8192);
    std::wstring url = fixup_input(buf);
    if (url.empty()) return;
    close_suggestions();
    Tab *t = active_tab();
    if (new_tab || !t) t = tab_new(L"", true);
    tab_navigate(t, url);
    ui_set_address_text(url);
}
static void delete_word_left() {
    DWORD s = 0, e = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (s != e) { SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)L""); return; }
    int n = GetWindowTextLengthW(g_edit);
    std::wstring t(n + 1, 0);
    GetWindowTextW(g_edit, t.data(), n + 1);
    t.resize(n);
    size_t i = s;
    while (i > 0 && iswspace(t[i - 1])) i--;
    while (i > 0 && !iswspace(t[i - 1]) && t[i - 1] != L'/' && t[i - 1] != L'.') i--;
    if (i == s && i > 0) i--;
    SendMessageW(g_edit, EM_SETSEL, i, s);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
}
static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (wp == VK_RETURN) { commit_address(alt || (ctrl && shift)); return 0; }
        if (wp == VK_ESCAPE) {
            Tab *t = active_tab();
            g_in_change = true; SetWindowTextW(h, t ? t->url.c_str() : L""); g_in_change = false;
            SendMessageW(h, EM_SETSEL, 0, -1);
            if (t && !t->url.empty()) blur_address(true); else close_suggestions();
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN) { sug_move(wp == VK_UP ? -1 : 1); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (ctrl && wp == VK_BACK) { delete_word_left(); return 0; }
        if (wp == VK_TAB) return 0;
        // the browser's own keys still work while typing (not the edit's: copy, paste, undo, arrows)
        if ((ctrl && wcschr(L"TWNJQ1234567890\x09\x21\x22", (wchar_t)wp)) || wp == VK_F5 || wp == VK_F6 || wp == VK_F11 || wp == VK_F12 ||
            (alt && (wp == VK_LEFT || wp == VK_RIGHT)))
            if (handle_shortcut((UINT)wp, ctrl, shift, alt)) return 0;
        break;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { commit_address(true); return 0; }
        if (handle_shortcut((UINT)wp, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, true)) return 0;
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 0x7F || wp == 1) return 0;   // no beep; 0x7F = Ctrl+Backspace
        if (wp >= 32) {
            LRESULT r = CallWindowProcW(g_edit_proc, h, msg, wp, lp);
            // typed at the end: complete inline with the best address from the history
            DWORD s = 0, e = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
            int n = GetWindowTextLengthW(h);
            std::wstring txt(n + 1, 0);
            GetWindowTextW(h, txt.data(), n + 1);
            txt.resize(n);
            g_typed = txt;
            if (s == e && (int)e == n) {
                std::wstring comp;
                history_suggest(txt, &comp);
                if (comp.size() > txt.size()) {
                    g_in_change = true;
                    SetWindowTextW(h, comp.c_str());
                    SendMessageW(h, EM_SETSEL, n, (LPARAM)comp.size());
                    g_in_change = false;
                }
                // sure of where this is going (a known site): it starts loading now, in a new tab
                Tab *at = active_tab();
                if (at && !at->view && !at->creating) tabs_preload(comp.size() > txt.size() ? fixup_input(comp) : L"");
            }
            update_suggestions();
            return r;
        }
        break;
    case WM_KILLFOCUS:
        PostMessageW(g_bar, WM_APP + 20, 0, 0);
        break;
    case WM_SETFOCUS:
        g_addr_focus.go(1, kHoverMs);
        ui_invalidate();
        break;
    case WM_MOUSEWHEEL: return 0;
    }
    return CallWindowProcW(g_edit_proc, h, msg, wp, lp);
}

// ---------------------------------------------------------------- floating panels (menu, suggestions, notes)
// Each is a layered window drawn with Direct2D into memory: rounded corners, a soft shadow and a fade, cheap
// enough for any computer (only while it fades).
struct Panel {
    HWND h = NULL;
    int w = 0, hh = 0;
    HDC dc = NULL; HBITMAP bmp = NULL; HGDIOBJ old = NULL;
    ComPtr<ID2D1DCRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> icons;
    Anim fade, slide;
    bool closing = false;
    POINT pos{};
    std::function<void(Panel &, double)> draw;
};
static Panel g_menu, g_sug, g_toast;
static const float kPanelPad = 18;   // room around the panel for its shadow (DIP)
static void panel_free(Panel &p) {
    if (p.h) { DestroyWindow(p.h); p.h = NULL; }
    p.rt.Reset(); p.br.Reset(); p.icons.clear();
    if (p.dc) { SelectObject(p.dc, p.old); DeleteDC(p.dc); p.dc = NULL; }
    if (p.bmp) { DeleteObject(p.bmp); p.bmp = NULL; }
    p.w = p.hh = 0;
}
static void panel_render(Panel &p) {
    if (!p.h || p.w <= 0 || p.hh <= 0) return;
    if (!p.dc) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = p.w; bi.bmiHeader.biHeight = -p.hh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HDC screen = GetDC(NULL);
        p.dc = CreateCompatibleDC(screen);
        ReleaseDC(NULL, screen);
        p.bmp = CreateDIBSection(p.dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        p.old = SelectObject(p.dc, p.bmp);
    }
    if (!p.rt) {
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(render_type(),
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(g_d2d->CreateDCRenderTarget(&rp, &p.rt))) return;
        p.rt->CreateSolidColorBrush(hex(0), &p.br);
        p.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    RECT r = { 0, 0, p.w, p.hh };
    p.rt->BindDC(p.dc, &r);
    double now = now_ms();
    p.rt->BeginDraw();
    p.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (p.draw) p.draw(p, now);
    if (p.rt->EndDraw() == D2DERR_RECREATE_TARGET) { p.rt.Reset(); p.br.Reset(); p.icons.clear(); }
    float k = p.fade.at(now);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)lroundf(255 * std::max(0.0f, std::min(1.0f, k))), AC_SRC_ALPHA };
    SIZE sz = { p.w, p.hh };
    POINT zero = { 0, 0 };
    UpdateLayeredWindow(p.h, NULL, &p.pos, &sz, p.dc, &zero, 0, &bf, ULW_ALPHA);
    if (p.fade.running(now) || p.slide.running(now)) want_frames();
    else if (p.closing) panel_free(p);
}
static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void panel_open(Panel &p, int x, int y, int w, int hh, bool activate) {
    if (p.closing) panel_free(p);
    p.closing = false;
    if (p.w != w || p.hh != hh) {
        p.rt.Reset();
        if (p.dc) { SelectObject(p.dc, p.old); DeleteDC(p.dc); p.dc = NULL; }
        if (p.bmp) { DeleteObject(p.bmp); p.bmp = NULL; }
    }
    p.w = w; p.hh = hh; p.pos = { x, y };
    bool fresh = !p.h;
    if (!p.h) {
        DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | (activate ? 0 : WS_EX_NOACTIVATE);
        if (&p == &g_toast) ex |= WS_EX_TRANSPARENT;
        p.h = CreateWindowExW(ex, L"PlumaPanel", L"", WS_POPUP, x, y, w, hh, g_win, NULL, g_inst, NULL);
    }
    if (fresh) { p.fade.snap(0); p.fade.go(1, kPanelMs); p.slide.snap(1); p.slide.go(0, kPanelMs); }
    panel_render(p);
    if (fresh) ShowWindow(p.h, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(p.h);
}
static void panel_close(Panel &p) {
    if (!p.h || p.closing) return;
    p.closing = true;
    p.fade.go(0, kPanelMs * 0.8);
    panel_render(p);
}
static void panel_card(Panel &p, D2D1_RECT_F r) {
    const Palette &c = pal();
    soft_shadow(p.rt.Get(), p.br.Get(), r, S(14), 1);
    fill(p.rt.Get(), p.br.Get(), r, S(14), c.surface);
    stroke(p.rt.Get(), p.br.Get(), r, S(14), alpha(c.text, dark_theme() ? 0.1f : 0.07f), S(1));
}

// ---------------------------------------------------------------- suggestions under the address
static std::vector<Suggestion> g_sugs;   // [0] is what Enter does with what was typed
static int g_sug_sel = 0, g_sug_hover = -1;
static const float kSugRow = 38;
static void sug_draw(Panel &p, double now) {
    const Palette &c = pal();
    float pad = S(kPanelPad);
    D2D1_RECT_F card = R(pad, pad - S(4) * p.slide.at(now) * 1.5f, p.w - pad, p.hh - pad);
    panel_card(p, card);
    for (size_t i = 0; i < g_sugs.size(); i++) {
        const Suggestion &s = g_sugs[i];
        D2D1_RECT_F r = R(card.left + S(6), card.top + S(6) + i * S(kSugRow), card.right - S(6), card.top + S(6) + (i + 1) * S(kSugRow));
        if ((int)i == g_sug_sel) fill(p.rt.Get(), p.br.Get(), r, S(9), alpha(c.accent, dark_theme() ? 0.2f : 0.09f));
        else if ((int)i == g_sug_hover) fill(p.rt.Get(), p.br.Get(), r, S(9), c.hover);
        float cy = (r.top + r.bottom) / 2, ix = r.left + S(18);
        ComPtr<ID2D1Bitmap> b;
        if (!s.search) b = host_icon(p.rt.Get(), p.icons, s.url);
        if (b) draw_icon_bitmap(p.rt.Get(), b.Get(), ix, cy, S(16), 1);
        else glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), s.search ? G_SEARCH : i == 0 ? G_GLOBE : G_CLOCK, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), c.sub);
        std::wstring main = s.title, side;
        if (s.search) side = T(L"buscar con ") + engine_name(g_set.engine);
        else if (i == 0) { main = strip_scheme(s.url); side = T(L"ir a la dirección"); }
        else { side = strip_scheme(s.url); if (main.empty()) { main = side; side.clear(); } }
        float tl = r.left + S(38), tr = r.right - S(12);
        float mw = std::min((tr - tl) * 0.62f, text_w(g_f.menu.Get(), main) + S(2));
        text(p.rt.Get(), p.br.Get(), g_f.menu.Get(), main, R(tl, r.top, tl + mw, r.bottom), c.text);
        if (!side.empty()) text(p.rt.Get(), p.br.Get(), g_f.menu.Get(), side, R(tl + mw + S(14), r.top, tr, r.bottom), c.sub);
    }
}
static void place_suggestions() {
    if (g_sugs.empty()) { close_suggestions(); return; }
    D2D1_RECT_F a = addr_rect();
    POINT o = { 0, 0 };
    ClientToScreen(g_bar, &o);
    float pad = S(kPanelPad);
    int w = (int)(a.right - a.left + pad * 2);
    int h = (int)(S(12) + g_sugs.size() * S(kSugRow) + pad * 2);
    g_sug.draw = sug_draw;
    panel_open(g_sug, (int)(o.x + a.left - pad), (int)(o.y + a.bottom + S(4) - pad), w, h, false);
}
static void update_suggestions() {
    if (!ui_address_focused()) return;
    wchar_t buf[4096];
    GetWindowTextW(g_edit, buf, 4096);
    std::wstring all = buf, typed = g_typed.empty() ? all : g_typed;
    g_sugs.clear();
    g_sug_sel = 0;
    if (trim(typed).empty()) { close_suggestions(); return; }
    std::wstring target = fixup_input(all);
    bool is_search = target.rfind(search_url(L""), 0) == 0;
    Suggestion first;
    first.search = is_search;
    first.url = target;
    first.title = is_search ? trim(typed) : all;
    g_sugs.push_back(first);
    auto same = [](std::wstring a, std::wstring b) {
        while (!a.empty() && a.back() == L'/') a.pop_back();
        while (!b.empty() && b.back() == L'/') b.pop_back();
        return lower(a) == lower(b);
    };
    for (auto &s : history_suggest(typed, NULL)) {
        if (g_sugs.size() >= 7) break;
        if (same(s.url, target)) continue;
        g_sugs.push_back(s);
    }
    // what was typed is a search? then a plain search for it comes first, and the history after
    place_suggestions();
}
static void close_suggestions() { g_sugs.clear(); g_sug_hover = -1; panel_close(g_sug); }
static void sug_move(int d) {
    if (g_sugs.empty()) { update_suggestions(); return; }
    g_sug_sel = (g_sug_sel + d + (int)g_sugs.size()) % (int)g_sugs.size();
    const Suggestion &s = g_sugs[g_sug_sel];
    g_in_change = true;
    SetWindowTextW(g_edit, g_sug_sel == 0 ? (g_typed.empty() ? s.title : g_typed).c_str() : s.url.c_str());
    SendMessageW(g_edit, EM_SETSEL, GetWindowTextLengthW(g_edit), GetWindowTextLengthW(g_edit));
    g_in_change = false;
    panel_render(g_sug);
}
static bool sug_commit(bool new_tab) {
    if (g_sugs.empty() || g_sug_sel <= 0 || g_sug_sel >= (int)g_sugs.size()) return false;
    std::wstring url = g_sugs[g_sug_sel].url;
    close_suggestions();
    Tab *t = active_tab();
    if (new_tab || !t) t = tab_new(L"", true);
    tab_navigate(t, url);
    ui_set_address_text(url);
    return true;
}

// ---------------------------------------------------------------- the menu
// each language by its own name; "" is Windows's
static std::wstring lang_name(const wchar_t *code) {
    static const struct { const wchar_t *code, *name; } names[] = { { L"es", L"Español" }, { L"en", L"English" }, { L"pt", L"Português" },
                                                                    { L"fr", L"Français" }, { L"de", L"Deutsch" }, { L"it", L"Italiano" } };
    for (auto &n : names) if (!wcscmp(code, n.code)) return n.name;
    return T(L"Como Windows");
}
enum { PAGE_MAIN, PAGE_EXT, PAGE_SCRIPTS, PAGE_ENGINE, PAGE_THEME, PAGE_FONT, PAGE_LANG, PAGE_UPDATE, PAGE_CLEAR, PAGE_TAB, PAGE_ONE_EXT };
struct MItem {
    int kind = 0;   // 0 action, 1 separator, 2 opens a page, 3 check, 4 radio, 5 zoom row, 6 note, 7 back
    std::wstring text, hint;
    wchar_t icon = 0;
    bool on = false, enabled = true, danger = false;
    std::function<void()> act;
    int page = 0;
    wchar_t b1 = 0, b2 = 0;                 // small buttons at the right of the row
    std::function<void()> act1, act2;
};
static std::vector<MItem> g_items;
static int g_menu_page = PAGE_MAIN, g_menu_tab = 0, g_menu_sel = -1, g_menu_hot_btn = 0;
static RECT g_menu_anchor;
static const float kMenuW = 344, kItemH = 34, kSepH = 9;
static float item_h(const MItem &m) { return m.kind == 1 ? S(kSepH) : m.kind == 6 ? S(30) : S(kItemH); }
static std::vector<MItem> build_page(int page);
static void menu_rebuild() { g_items = build_page(g_menu_page); }
static D2D1_RECT_F item_rect(size_t i) {
    float pad = S(kPanelPad), y = pad + S(6);
    for (size_t k = 0; k < i; k++) y += item_h(g_items[k]);
    return R(pad + S(6), y, g_menu.w - pad - S(6), y + item_h(g_items[i]));
}
static D2D1_RECT_F item_btn(size_t i, int which) {   // 1 or 2, from the right
    D2D1_RECT_F r = item_rect(i);
    float x = r.right - S(4) - which * S(30);
    return R(x, r.top + S(3), x + S(28), r.bottom - S(3));
}
static void menu_draw(Panel &p, double now) {
    const Palette &c = pal();
    float pad = S(kPanelPad);
    D2D1_RECT_F card = R(pad, pad - S(6) * p.slide.at(now), p.w - pad, p.hh - pad);
    panel_card(p, card);
    float dx = S(14) * p.slide.at(now);   // a page comes in from the side it was opened towards
    p.rt->PushAxisAlignedClip(card, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < g_items.size(); i++) {
        const MItem &m = g_items[i];
        D2D1_RECT_F r = item_rect(i);
        r.left += dx; r.right += dx;
        if (m.kind == 1) { fill(p.rt.Get(), p.br.Get(), R(r.left + S(8), (r.top + r.bottom) / 2, r.right - S(8), (r.top + r.bottom) / 2 + S(1)), 0, c.line); continue; }
        if (m.kind == 6) { text(p.rt.Get(), p.br.Get(), g_f.menu.Get(), m.text, R(r.left + S(12), r.top, r.right - S(12), r.bottom), c.sub); continue; }
        bool sel = (int)i == g_menu_sel && m.enabled && m.kind != 5;
        if (sel) fill(p.rt.Get(), p.br.Get(), r, S(9), c.hover);
        D2D1_COLOR_F ink = !m.enabled ? alpha(c.sub, 0.55f) : m.danger ? c.error : c.text;
        float ix = r.left + S(18), cy = (r.top + r.bottom) / 2;
        wchar_t ig = m.icon;
        if (m.kind == 3) ig = m.on ? G_CHECK : 0;
        if (m.kind == 4) ig = m.on ? G_CHECK : 0;
        if (m.kind == 7) ig = G_CHEV_L;
        if (ig) glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), ig, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), m.kind >= 3 && m.kind <= 4 ? c.accent : (m.danger ? c.error : c.sub));
        float tr = r.right - S(12);
        if (m.kind == 2) { glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), G_CHEV_R, R(r.right - S(28), r.top, r.right - S(8), r.bottom), c.sub); tr -= S(18); }
        if (m.b1) {
            for (int k = 1; k <= 2; k++) {
                wchar_t g = k == 1 ? m.b1 : m.b2;
                if (!g) continue;
                D2D1_RECT_F b = item_btn(i, k); b.left += dx; b.right += dx;
                if (g_menu_sel == (int)i && g_menu_hot_btn == k) fill(p.rt.Get(), p.br.Get(), b, S(7), c.press);
                glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), g, b, c.sub);
                tr = b.left - S(4);
            }
        }
        if (m.kind == 5) {
            // zoom: a minus, the level, a plus
            text(p.rt.Get(), p.br.Get(), g_f.menu.Get(), m.text, R(r.left + S(38), r.top, r.right, r.bottom), ink);
            D2D1_RECT_F minus = item_btn(i, 4), plus = item_btn(i, 1);
            minus.left += dx; minus.right += dx; plus.left += dx; plus.right += dx;
            for (int k : { 4, 1 }) {
                D2D1_RECT_F b = k == 4 ? minus : plus;
                if (g_menu_sel == (int)i && g_menu_hot_btn == k) fill(p.rt.Get(), p.br.Get(), b, S(7), c.press);
                else fill(p.rt.Get(), p.br.Get(), b, S(7), c.hover);
                glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), k == 4 ? G_ZOOMOUT : G_ZOOMIN, b, c.text);
            }
            wchar_t zs[16]; swprintf_s(zs, L"%d %%", (int)lround(tab_zoom_factor() * 100));
            auto f = g_f.menu;
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            text(p.rt.Get(), p.br.Get(), f.Get(), zs, R(minus.right, r.top, plus.left, r.bottom), c.text);
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            continue;
        }
        float hw = m.hint.empty() ? 0 : std::min(S(150), text_w(g_f.hint.Get(), m.hint) + S(4));
        text(p.rt.Get(), p.br.Get(), g_f.menu.Get(), m.text, R(r.left + S(38), r.top, tr - hw - S(8), r.bottom), m.kind == 7 ? c.sub : ink);
        if (hw > 0) text(p.rt.Get(), p.br.Get(), g_f.hint.Get(), m.hint, R(tr - hw, r.top, tr, r.bottom), c.sub);
    }
    p.rt->PopAxisAlignedClip();
}
static void menu_place(bool from_side) {
    menu_rebuild();
    float h = S(12);
    for (auto &m : g_items) h += item_h(m);
    float pad = S(kPanelPad);
    int w = (int)(S(kMenuW) + pad * 2), hh = (int)(h + pad * 2);
    // under its button, right edges lined up; kept on the screen
    int x = g_menu_anchor.right - w + (int)pad, y = g_menu_anchor.bottom + (int)S(4) - (int)pad;
    if (g_menu_page == PAGE_TAB) x = g_menu_anchor.left - (int)pad;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&g_menu_anchor, MONITOR_DEFAULTTONEAREST), &mi);
    x = std::max((int)mi.rcWork.left - (int)pad, std::min(x, (int)mi.rcWork.right - w + (int)pad));
    if (y + hh > mi.rcWork.bottom + pad) y = std::max((int)mi.rcWork.top, (int)(mi.rcWork.bottom + pad) - hh);
    g_menu.draw = menu_draw;
    bool fresh = !g_menu.h || g_menu.closing;
    panel_open(g_menu, x, y, w, hh, true);
    if (!fresh && from_side) { g_menu.slide.snap(1); g_menu.slide.go(0, kPanelMs); panel_render(g_menu); }
}
static void open_menu_page(int page, RECT anchor, int tab_id) {
    g_menu_page = page; g_menu_tab = tab_id; g_menu_sel = -1; g_menu_anchor = anchor;
    close_suggestions();
    menu_place(false);
    g_menu_btn_open.go(1, kHoverMs);
}
static void menu_close() {
    if (!g_menu.h) return;
    panel_close(g_menu);
    g_menu_btn_open.go(0, kHoverMs);
    ui_invalidate();
}
static void menu_go(int page) { g_menu_page = page; g_menu_sel = -1; menu_place(true); }
static void after_menu(std::function<void()> f) {
    // the menu goes first, then the action (some open dialogs or move the focus)
    menu_close();
    SetForegroundWindow(g_win);
    f();
}
void ui_open_menu() {
    D2D1_RECT_F m = menu_rect();
    POINT a = { (LONG)m.left, (LONG)m.top }, b = { (LONG)m.right, (LONG)m.bottom };
    ClientToScreen(g_bar, &a); ClientToScreen(g_bar, &b);
    open_menu_page(PAGE_MAIN, { a.x, a.y, b.x, b.y });
}
static MItem item(const std::wstring &text, wchar_t icon, const std::wstring &hint, std::function<void()> act) {
    MItem m; m.text = text; m.icon = icon; m.hint = hint; m.act = act; return m;
}
static MItem sep() { MItem m; m.kind = 1; return m; }
static MItem note(const std::wstring &t) { MItem m; m.kind = 6; m.text = t; m.enabled = false; return m; }
static MItem back(const std::wstring &t) { MItem m; m.kind = 7; m.text = t; m.act = [] { menu_go(PAGE_MAIN); }; return m; }
static MItem sub(const std::wstring &t, wchar_t icon, const std::wstring &value, int page) {
    MItem m; m.kind = 2; m.text = t; m.icon = icon; m.hint = value; m.page = page; return m;
}
static std::vector<MItem> build_page(int page) {
    std::vector<MItem> v;
    if (page == PAGE_MAIN) {
        v.push_back(item(T(L"Pestaña nueva"), G_NEWTAB, T(L"Ctrl+T"), [] { after_menu([] { tab_new(L"", true); }); }));
        v.push_back(item(T(L"Pestaña de incógnito"), G_INPRIVATE, T(L"Ctrl+Mayús+N"), [] { after_menu([] { tab_new(L"", true, -1, true); }); }));
        MItem re = item(T(L"Reabrir pestaña cerrada"), G_UNDO, T(L"Ctrl+Mayús+T"), [] { after_menu([] { tab_reopen_closed(); }); });
        v.push_back(re);
        v.push_back(sep());
        MItem z; z.kind = 5; z.text = L"Zoom"; z.icon = G_ZOOM; v.push_back(z);
        v.push_back(item(T(L"Buscar en la página"), G_SEARCH, T(L"Ctrl+F"), [] { after_menu([] { tab_find(); }); }));
        v.push_back(item(T(L"Imprimir"), G_PRINT, T(L"Ctrl+P"), [] { after_menu([] { tab_print(); }); }));
        v.push_back(item(T(L"Descargas"), G_DOWNLOAD, T(L"Ctrl+J"), [] { after_menu([] { tab_downloads(); }); }));
        v.push_back(sep());
        size_t on = 0; for (auto &e : g_exts) if (!e.builtin && e.enabled) on++;
        v.push_back(sub(T(L"Extensiones"), G_PUZZLE, on == 1 ? T(L"1 activa") : on ? std::to_wstring(on) + T(L" activas") : L"", PAGE_EXT));
        size_t son = 0; for (auto &s : g_scripts) if (s.enabled) son++;
        v.push_back(sub(T(L"Scripts de usuario"), G_CODE, son == 1 ? T(L"1 activo") : son ? std::to_wstring(son) + T(L" activos") : L"", PAGE_SCRIPTS));
        v.push_back(sub(T(L"Buscador"), G_GLOBE, engine_name(g_set.engine), PAGE_ENGINE));
        v.push_back(sub(T(L"Tema"), G_COLOR, g_set.theme == 1 ? T(L"Oscuro") : g_set.theme == 2 ? T(L"Como Windows") : T(L"Claro"), PAGE_THEME));
        v.push_back(sub(T(L"Fuente"), G_FONT, g_set.font.empty() ? T(L"La de Windows") : g_set.font, PAGE_FONT));
        v.push_back(sub(T(L"Idioma"), G_LANG, lang_name(g_set.lang.empty() ? L"" : lang_code()), PAGE_LANG));
        v.push_back(sub(T(L"Actualizaciones"), G_UPDATE, update_version(), PAGE_UPDATE));
        if (video_available()) {
            // YouTube in Pluma's own player (a fraction of the page's memory)
            MItem yt; yt.kind = 3; yt.text = T(L"YouTube en el reproductor de Pluma"); yt.on = g_set.native_video;
            yt.act = [] { g_set.native_video = !g_set.native_video; settings_save(); video_settings_changed(); };
            v.push_back(yt);
        }
        // the new tab page: the sites visited most, while the person has no pages of their own there
        MItem st; st.kind = 3; st.text = T(L"Sugerir lo que más visito"); st.on = g_set.suggest_top;
        st.act = [] { g_set.suggest_top = !g_set.suggest_top; settings_save(); g_ntp_filled = false; };
        v.push_back(st);
        v.push_back(sub(T(L"Borrar datos"), G_ERASE, L"", PAGE_CLEAR));
        v.push_back(item(T(L"Herramientas de desarrollo"), G_DEV, L"F12", [] { after_menu([] { tab_devtools(); }); }));
        v.push_back(sep());
        v.push_back(note(T(L"Pluma usa ahora ") + std::to_wstring(engine_memory_mb()) + T(L" MB de memoria")));
        v.push_back(item(T(L"Salir de Pluma"), G_POWER, T(L"Ctrl+Mayús+Q"), [] { after_menu([] { app_quit(); }); }));
    } else if (page == PAGE_EXT) {
        v.push_back(back(T(L"Extensiones")));
        bool any = false;
        for (size_t i = 0; i < g_exts.size(); i++) {
            const Extension &e = g_exts[i];
            if (e.builtin) continue;
            any = true;
            MItem m; m.kind = 3; m.text = e.name; m.on = e.enabled;
            m.act = [i] { ext_toggle(i); };
            if (!e.options.empty()) { m.b2 = G_GEAR; m.act2 = [i] { after_menu([i] { ext_open_options(i); }); }; }
            m.b1 = G_DELETE; m.act1 = [i] { ext_remove(i); };
            v.push_back(m);
        }
        if (!any) v.push_back(note(T(L"Todavía no hay ninguna.")));
        if (ext_bundled_removed())
            v.push_back(item(T(L"Volver a poner uBlock Origin Lite"), G_UNDO, L"", [] { after_menu([] { ext_restore_bundled(); }); }));
        v.push_back(sep());
        v.push_back(item(T(L"Buscar en la Chrome Web Store"), G_STORE, L"", [] { after_menu([] { tab_new(L"https://chromewebstore.google.com/", true); }); }));
        v.push_back(item(T(L"Abrir la carpeta de extensiones"), G_FOLDER, L"", [] { after_menu([] { ShellExecuteW(NULL, L"open", extensions_folder().c_str(), NULL, NULL, SW_SHOWNORMAL); }); }));
        v.push_back(note(T(L"En la tienda, abre una extensión y pulsa «Añadir a Pluma».")));
    } else if (page == PAGE_SCRIPTS) {
        v.push_back(back(T(L"Scripts de usuario")));
        for (size_t i = 0; i < g_scripts.size(); i++) {
            MItem m; m.kind = 3; m.text = g_scripts[i].name; m.on = g_scripts[i].enabled;
            m.act = [i] { script_toggle(i); tabs_set_scripts(); };
            v.push_back(m);
        }
        if (g_scripts.empty()) v.push_back(note(T(L"Todavía no hay ninguno.")));
        v.push_back(sep());
        v.push_back(item(T(L"Abrir la carpeta de scripts"), G_FOLDER, L"", [] { after_menu([] { ShellExecuteW(NULL, L"open", scripts_folder().c_str(), NULL, NULL, SW_SHOWNORMAL); }); }));
        v.push_back(note(T(L"Abre un archivo .user.js en Pluma para instalarlo.")));
    } else if (page == PAGE_ENGINE) {
        v.push_back(back(T(L"Buscador")));
        for (const wchar_t *id : { L"google", L"duckduckgo", L"bing", L"startpage", L"brave" }) {
            MItem m; m.kind = 4; m.text = engine_name(id); m.on = g_set.engine == id;
            std::wstring sid = id;
            m.act = [sid] { g_set.engine = sid; settings_save(); menu_rebuild(); panel_render(g_menu); };
            v.push_back(m);
        }
    } else if (page == PAGE_THEME) {
        v.push_back(back(T(L"Tema")));
        const wchar_t *names[] = { T(L"Claro"), T(L"Oscuro"), T(L"Como Windows") };
        for (int i : { 2, 0, 1 }) {
            MItem m; m.kind = 4; m.text = names[i]; m.on = g_set.theme == i;
            m.act = [i] { g_set.theme = i; settings_save(); ui_theme_changed(); tabs_apply_theme(); menu_rebuild(); panel_render(g_menu); };
            v.push_back(m);
        }
    } else if (page == PAGE_FONT) {
        v.push_back(back(T(L"Fuente")));
        // Windows's own, the two Pluma brings, and ones every Windows has (any other installed one: fuente= in ajustes.ini)
        const wchar_t *fams[] = { L"", L"Roboto", L"Roboto Mono", L"Arial", L"Verdana", L"Georgia", L"Consolas" };
        for (const wchar_t *f : fams) {
            MItem m; m.kind = 4; m.text = *f ? f : T(L"La de Windows"); m.on = g_set.font == f;
            std::wstring fam = f;
            m.act = [fam] { g_set.font = fam; settings_save(); ui_font_changed(); menu_rebuild(); panel_render(g_menu); };
            v.push_back(m);
        }
        if (std::find(std::begin(fams), std::end(fams), g_set.font) == std::end(fams)) {
            MItem m; m.kind = 4; m.text = g_set.font; m.on = true; v.push_back(m);
        }
    } else if (page == PAGE_LANG) {
        v.push_back(back(T(L"Idioma")));
        for (const wchar_t *c : { L"", L"es", L"en", L"pt", L"fr", L"de", L"it" }) {
            MItem m; m.kind = 4; m.text = lang_name(c); m.on = g_set.lang == c;
            std::wstring code = c;
            m.act = [code] { g_set.lang = code; settings_save(); ui_invalidate(); menu_rebuild(); panel_render(g_menu); };
            v.push_back(m);
        }
    } else if (page == PAGE_UPDATE) {
        v.push_back(back(T(L"Actualizaciones")));
        const wchar_t *modes[] = { T(L"Nunca"), T(L"Avisarme"), T(L"Automáticas") };
        for (int i : { 2, 1, 0 }) {
            MItem m; m.kind = 4; m.text = modes[i]; m.on = g_set.update == i;
            m.act = [i] { g_set.update = i; settings_save(); menu_rebuild(); panel_render(g_menu); };
            v.push_back(m);
        }
        v.push_back(sep());
        v.push_back(item(T(L"Buscar ahora"), G_RELOAD, L"", [] { after_menu([] { update_check_now(); }); }));
        v.push_back(note(T(L"Versión ") + std::wstring(update_version())));
    } else if (page == PAGE_CLEAR) {
        v.push_back(back(T(L"Borrar datos")));
        v.push_back(item(T(L"El historial de Pluma"), G_CLOCK, L"", [] { history_clear(); menu_close(); ui_toast(T(L"Historial borrado")); }));
        MItem all = item(T(L"Todo menos las contraseñas"), G_ERASE, L"", [] {
            ComPtr<ICoreWebView2_13> v13; ComPtr<ICoreWebView2Profile> pr; ComPtr<ICoreWebView2Profile2> p2;
            Tab *t = nullptr;
            for (auto &x : g_tabs) if (x->view) { t = x.get(); break; }
            history_clear();
            if (t && SUCCEEDED(t->view.As(&v13)) && SUCCEEDED(v13->get_Profile(&pr)) && SUCCEEDED(pr.As(&p2)))
                p2->ClearBrowsingData((COREWEBVIEW2_BROWSING_DATA_KINDS)(COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_SITE | COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE |
                                      COREWEBVIEW2_BROWSING_DATA_KINDS_DOWNLOAD_HISTORY | COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY),
                                      Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([](HRESULT) -> HRESULT { ui_toast(T(L"Datos borrados: cookies, caché e historial")); return S_OK; }).Get());
            else ui_toast(T(L"Historial borrado"));
            menu_close();
        });
        all.danger = true;
        v.push_back(all);
        v.push_back(note(T(L"Las cookies cierran tus sesiones en las webs.")));
    } else if (page == PAGE_TAB) {
        int id = g_menu_tab;
        Tab *t = tab_by_id(id);
        if (!t) return v;
        v.push_back(item(T(L"Recargar"), G_RELOAD, L"", [id] { after_menu([id] { int i = tab_index(id); if (i >= 0) { tab_activate(i); tab_reload(false); } }); }));
        v.push_back(item(T(L"Duplicar"), G_NEWTAB, L"", [id] { after_menu([id] { Tab *t = tab_by_id(id); if (t && !t->url.empty()) tab_new(t->url, true, tab_index(id) + 1, t->incognito); }); }));
        if (t->view) v.push_back(item(t->muted ? T(L"Volver a dar sonido") : T(L"Silenciar"), t->muted ? G_VOL : G_MUTE, L"", [id] { after_menu([id] { tab_mute(tab_by_id(id)); }); }));
        v.push_back(sep());
        v.push_back(item(T(L"Cerrar las de la derecha"), G_X, L"", [id] { after_menu([id] { int i = tab_index(id); while (i >= 0 && (int)g_tabs.size() > i + 1) tab_close((int)g_tabs.size() - 1); }); }));
        v.push_back(item(T(L"Cerrar las demás"), G_X, L"", [id] { after_menu([id] { for (int k = (int)g_tabs.size() - 1; k >= 0; k--) if (g_tabs[k]->id != id) tab_close(k); }); }));
        v.push_back(item(T(L"Cerrar"), G_X, T(L"Ctrl+W"), [id] { after_menu([id] { tab_close(tab_index(id)); }); }));
    } else if (page == PAGE_ONE_EXT) {
        // right click on an extension's button, as in Chrome (g_menu_tab: its place in g_exts)
        size_t i = (size_t)g_menu_tab;
        if (i >= g_exts.size()) return v;
        v.push_back(note(g_exts[i].name));
        if (!g_exts[i].options.empty()) v.push_back(item(T(L"Opciones"), G_GEAR, L"", [i] { after_menu([i] { ext_open_options(i); }); }));
        v.push_back(item(T(L"Desactivar"), G_POWER, L"", [i] { after_menu([i] { ext_toggle(i); ui_layout(); }); }));
        MItem rm = item(T(L"Quitar de Pluma"), G_DELETE, L"", [i] { after_menu([i] { ext_remove(i); }); });
        rm.danger = true;
        v.push_back(rm);
        v.push_back(note(T(L"Vuelve a activarla en ⋯ > Extensiones.")));
    }
    return v;
}
static int menu_hit(int x, int y, int *btn) {
    *btn = 0;
    for (size_t i = 0; i < g_items.size(); i++) {
        D2D1_RECT_F r = item_rect(i);
        if (!inside(r, (float)x, (float)y)) continue;
        const MItem &m = g_items[i];
        if (m.kind == 5) { if (inside(item_btn(i, 4), (float)x, (float)y)) *btn = 4; else if (inside(item_btn(i, 1), (float)x, (float)y)) *btn = 1; }
        else if (m.b1 && inside(item_btn(i, 1), (float)x, (float)y)) *btn = 1;
        else if (m.b2 && inside(item_btn(i, 2), (float)x, (float)y)) *btn = 2;
        return (int)i;
    }
    return -1;
}
static void menu_activate(int i, int btn) {
    if (i < 0 || i >= (int)g_items.size()) return;
    MItem m = g_items[i];
    if (!m.enabled || m.kind == 1 || m.kind == 6) return;
    if (m.kind == 5) {
        if (btn == 4) tab_zoom(-1); else if (btn == 1) tab_zoom(1); else tab_zoom(0);
        panel_render(g_menu);
        ui_invalidate();
        return;
    }
    if (btn == 1 && m.act1) { m.act1(); if (g_menu.h && !g_menu.closing) { menu_place(false); } return; }
    if (btn == 2 && m.act2) { m.act2(); return; }
    if (m.kind == 2) { menu_go(m.page); return; }
    if (m.act) m.act();
    if (m.kind == 3 && g_menu.h && !g_menu.closing) { menu_rebuild(); panel_render(g_menu); ui_layout(); }
}
static void menu_key(UINT vk) {
    int n = (int)g_items.size();
    auto selectable = [](const MItem &m) { return m.enabled && m.kind != 1 && m.kind != 6; };
    if (vk == VK_ESCAPE) { if (g_menu_page != PAGE_MAIN && g_menu_page != PAGE_TAB) menu_go(PAGE_MAIN); else after_menu([] {}); return; }
    if (vk == VK_LEFT && g_menu_page != PAGE_MAIN && g_menu_page != PAGE_TAB) { menu_go(PAGE_MAIN); return; }
    if (vk == VK_UP || vk == VK_DOWN) {
        int d = vk == VK_UP ? -1 : 1, i = g_menu_sel;
        for (int k = 0; k < n; k++) { i = (i + d + n) % n; if (selectable(g_items[i])) break; }
        g_menu_sel = i; g_menu_hot_btn = 0;
        panel_render(g_menu);
        return;
    }
    if ((vk == VK_RETURN || vk == VK_SPACE || vk == VK_RIGHT) && g_menu_sel >= 0) {
        if (vk == VK_RIGHT && g_items[g_menu_sel].kind != 2) return;
        menu_activate(g_menu_sel, 0);
    }
}

// ---------------------------------------------------------------- notes and toasts
void ui_notice(const std::wstring &text, const std::wstring &button, std::function<void()> action, int seconds, int owner_tab) {
    if (g_notice.on && g_notice.text == text && g_notice.owner == owner_tab) return;
    g_notice.text = text; g_notice.button = button; g_notice.action = action; g_notice.on = true; g_notice.owner = owner_tab;
    g_notice.until = seconds > 0 ? (double)(GetTickCount64() + seconds * 1000ULL) : 0;
    g_notice_h.go(1, kTabMs);
    ui_layout();
}
static void notice_close() { g_notice.on = false; g_notice_h.go(0, kTabMs); ui_layout(); }
void ui_notice_drop(int tab_id) { if (g_notice.on && g_notice.owner && g_notice.owner == tab_id) notice_close(); }
static std::wstring g_toast_text;
static void toast_draw(Panel &p, double now) {
    const Palette &c = pal();
    float pad = S(kPanelPad);
    D2D1_RECT_F card = R(pad, pad + S(6) * p.slide.at(now), p.w - pad, p.hh - pad);
    panel_card(p, card);
    text(p.rt.Get(), p.br.Get(), g_f.menu.Get(), g_toast_text, R(card.left + S(16), card.top, card.right - S(16), card.bottom), c.text);
}
void ui_toast(const std::wstring &t) {
    g_toast_text = t;
    float pad = S(kPanelPad);
    int w = (int)(std::min(S(520), text_w(g_f.menu.Get(), t) + S(34)) + pad * 2), h = (int)(S(40) + pad * 2);
    RECT rc; GetClientRect(g_win, &rc);
    POINT o = { (rc.right - w) / 2, rc.bottom - h - (int)S(18) };
    ClientToScreen(g_win, &o);
    g_toast.draw = toast_draw;
    panel_open(g_toast, o.x, o.y, w, h, false);
    SetTimer(g_toast.h, 2, 2600, NULL);
}
void ui_close_popups() { close_suggestions(); if (g_menu.h && !g_menu.closing) menu_close(); }

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Panel *p = h == g_menu.h ? &g_menu : h == g_sug.h ? &g_sug : h == g_toast.h ? &g_toast : nullptr;
    switch (msg) {
    case WM_TIMER:
        if (!p) break;
        if (wp == 2) { KillTimer(h, 2); panel_close(*p); return 0; }
        panel_render(*p);
        return 0;
    case WM_MOUSEACTIVATE: return p == &g_menu ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_ACTIVATE:
        if (p == &g_menu && LOWORD(wp) == WA_INACTIVE && !g_menu.closing) menu_close();
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        if (p == &g_menu) {
            int btn = 0, i = menu_hit(x, y, &btn);
            if (i != g_menu_sel || btn != g_menu_hot_btn) { g_menu_sel = i; g_menu_hot_btn = btn; panel_render(g_menu); }
        } else if (p == &g_sug) {
            float pad = S(kPanelPad);
            int i = (int)((y - pad - S(6)) / S(kSugRow));
            if (y < pad + S(6) || i >= (int)g_sugs.size()) i = -1;
            if (i != g_sug_hover) { g_sug_hover = i; panel_render(g_sug); }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (p == &g_menu) { g_menu_hot_btn = 0; panel_render(g_menu); }
        if (p == &g_sug) { g_sug_hover = -1; panel_render(g_sug); }
        return 0;
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (p == &g_menu) { int btn = 0, i = menu_hit(x, y, &btn); if (i >= 0) menu_activate(i, btn); else after_menu([] {}); }
        else if (p == &g_sug && g_sug_hover >= 0) {
            g_sug_sel = g_sug_hover;
            if (g_sug_sel == 0) commit_address(false); else sug_commit(false);
        }
        return 0;
    }
    case WM_KEYDOWN: if (p == &g_menu) { menu_key((UINT)wp); return 0; } break;
    case WM_NCHITTEST: return HTCLIENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- the bar's window: mouse and keys
static void set_hover(HitInfo h) {
    if (h.what == g_hover.what && h.id == g_hover.id) return;
    // fade the old one out, the new one in
    auto fade = [](HitInfo x, float v) {
        if (x.what == H_TAB) { Tab *t = tab_by_id(x.id); if (t) t->hover.go(v, kHoverMs); }
        else if (x.what == H_TAB_CLOSE) { Tab *t = tab_by_id(x.id); if (t) { t->close_hover.go(v, kHoverMs); t->hover.go(v, kHoverMs); } }
        else if (x.what == H_TAB_AUDIO) { Tab *t = tab_by_id(x.id); if (t) t->hover.go(v, kHoverMs); g_btn_hover[hover_key(H_TAB_AUDIO, x.id)].go(v, kHoverMs); }
        else if (x.what != H_NONE) g_btn_hover[hover_key(x.what, x.id)].go(v, kHoverMs);
    };
    fade(g_hover, 0);
    fade(h, 1);
    g_hover = h;
    if (h.what == H_NTP_ROW) SetTimer(g_bar, 33, 120, NULL);
    if (h.what == H_TAB) SetTimer(g_bar, 34, 150, NULL);
    HCURSOR cur = LoadCursorW(NULL, h.what == H_ADDR ? IDC_IBEAM : h.what == H_NTP_ROW || h.what == H_NTP_X ? IDC_HAND : IDC_ARROW);
    SetCursor(cur);
    ui_invalidate();
}
void ui_caption_hover(int code) {
    HitInfo h;
    if (code == HTMINBUTTON) h.what = H_MIN; else if (code == HTMAXBUTTON) h.what = H_MAX; else if (code == HTCLOSE) h.what = H_CLOSE;
    if (h.what != H_NONE || g_hover.what == H_MIN || g_hover.what == H_MAX || g_hover.what == H_CLOSE) set_hover(h);
}
void ui_caption_press(int code, bool down) { g_caption_down = down ? code : 0; ui_invalidate(); }
void ui_window_activated(bool active) {
    g_active_window = active;
    if (!active) close_suggestions();
    ui_invalidate();
}
static void drag_update(float x) {
    g_drag_x = x;
    Tab *t = tab_by_id(g_drag_id);
    int i = tab_index(g_drag_id);
    if (!t || i < 0) return;
    // the slot under the dragged tab's centre
    float c = x - g_drag_dx + g_geo.tab_w / 2;
    int target = (int)((c - g_geo.strip_l) / (g_geo.tab_w + S(kTabGap)));
    target = std::max(0, std::min(target, (int)g_tabs.size() - 1));
    if (target != i) {
        double now = now_ms();
        auto before = tab_slots(now);
        Tab *act = active_tab();
        auto moved = std::move(g_tabs[i]);
        g_tabs.erase(g_tabs.begin() + i);
        g_tabs.insert(g_tabs.begin() + target, std::move(moved));
        for (size_t k = 0; k < g_tabs.size(); k++) if (g_tabs[k].get() == act) g_active = (int)k;
        auto after = tab_slots(now);
        // the others slide into their new places
        for (auto &b : before)
            for (auto &a : after)
                if (a.t == b.t && a.t->id != g_drag_id) { float off = b.x - (a.x - (a.t->x.running(now) ? a.t->x.at(now) : 0)); a.t->x.snap(off); a.t->x.go(0, kTabMs); }
        session_save_soon();
    }
    ui_invalidate();
}
static void ntp_refresh() { g_ntp_filled = false; g_hover = HitInfo(); ui_layout(); }
static void ntp_take_off(size_t i) {
    // a row off the new tab page: one of the person's own pages, or a suggested site (it does not come back), with a
    // way back
    HistoryEntry e = g_ntp_rows[i];
    if (g_ntp_pins) {
        size_t at = 0;
        for (size_t k = 0; k < g_pins.size(); k++) if (g_pins[k].url == e.url) at = k;
        pins_remove(e.url);
        ui_notice(T(L"Quitada de tus páginas."), T(L"Deshacer"), [e, at] { pins_add(e.url, e.title, at); ntp_refresh(); }, 8);
    } else {
        history_hide_site(e.url, true);
        ui_notice(T(L"No volverá a salir aquí."), T(L"Deshacer"), [e] { history_hide_site(e.url, false); ntp_refresh(); }, 8);
    }
    ntp_refresh();
}
void ui_toggle_pin() {
    // the page on screen onto the new tab page, or off it (the address's star, Ctrl+D)
    Tab *t = active_tab();
    if (!t || !(starts_with(t->url, L"https://") || starts_with(t->url, L"http://"))) return;
    std::wstring title = t->title.empty() ? url_host(t->url) : t->title;
    pins_toggle(t->url, title);
    g_ntp_filled = false;
    ui_invalidate();
}
static void bar_click(HitInfo h, float x, float y) {
    switch (h.what) {
    case H_PLUS: tab_new(L"", true); break;
    case H_BACK: tab_go_back(); break;
    case H_FWD: tab_go_forward(); break;
    case H_RELOAD: { Tab *t = active_tab(); if (t && t->loading) tab_stop(); else tab_reload(GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_CONTROL) < 0); break; }
    case H_MENU: if (g_menu.h && !g_menu.closing) menu_close(); else ui_open_menu(); break;
    case H_ZOOM: tab_zoom(0); ui_invalidate(); break;
    case H_EXT: {
        D2D1_RECT_F r{};
        size_t k = 0;
        for (size_t i = 0; i < g_exts.size(); i++) { if (!g_exts[i].enabled || g_exts[i].popup.empty()) continue; if ((int)i == h.id) { r = ext_rect(k); break; } k++; }
        POINT a = { (LONG)r.left, (LONG)r.top }, b = { (LONG)r.right, (LONG)r.bottom };
        ClientToScreen(g_bar, &a); ClientToScreen(g_bar, &b);
        ext_open_popup((size_t)h.id, { a.x, a.y, b.x, b.y });
        break;
    }
    case H_TAB_CLOSE: { float w = g_geo.tab_w; tab_close(tab_index(h.id), true); g_frozen_w = w; compute_geo(); set_hover(hit_test(x, y)); break; }
    case H_TAB_AUDIO: tab_mute(tab_by_id(h.id)); break;
    case H_NOTICE_BTN: { auto a = g_notice.action; notice_close(); if (a) a(); break; }
    case H_NOTICE_X: notice_close(); break;
    case H_NTP_ROW: if (h.id < (int)g_ntp_rows.size()) { Tab *t = active_tab(); std::wstring u = g_ntp_rows[h.id].url; if (t) tab_navigate(t, u); else tab_new(u, true); } break;
    case H_NTP_X: if (h.id < (int)g_ntp_rows.size()) ntp_take_off((size_t)h.id); break;
    case H_STAR: ui_toggle_pin(); break;
    default: break;
    }
}
static LRESULT CALLBACK bar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        if (!IsZoomed(g_win) && !g_fullscreen) {
            UINT dpi = GetDpiForWindow(g_win);
            int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            if (p.y < fy) return HTTRANSPARENT;   // the window's top edge resizes it
        }
        return ui_hit_caption(p) == HTCLIENT ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); paint(); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g_rt) { if (FAILED(g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp))))) { g_rt.Reset(); g_br.Reset(); release_bitmaps(); } }
        compute_geo();
        ui_invalidate();
        return 0;
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        // (not in tests: they move the mouse with messages, and Windows, seeing the real one elsewhere, says it left)
        if (!g_tracking && !g_test) { TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme); g_tracking = true; }
        ui_caption_hover(0);
        if (g_press.what == H_TAB && (wp & MK_LBUTTON)) {
            if (!g_dragging && fabs(x - g_press_x) > S(5) && g_tabs.size() > 1) g_dragging = true;
            if (g_dragging) { drag_update(x); return 0; }
        }
        set_hover(hit_test(x, y));
        if (g_frozen_w > 0 && y > S(kTabRowH + 10)) { g_frozen_w = 0; compute_geo(); ui_invalidate(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        set_hover({});
        if (g_frozen_w > 0) { g_frozen_w = 0; compute_geo(); ui_invalidate(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(NULL, g_hover.what == H_ADDR ? IDC_IBEAM : g_hover.what == H_NTP_ROW ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        HitInfo hi = hit_test(x, y);
        if (hi.what != H_MENU) ui_close_popups();
        g_press = hi;
        g_press_x = x;
        if (hi.what == H_TAB) {
            // a tab is chosen on press, as in every browser; dragging moves it
            int i = tab_index(hi.id);
            if (i != g_active && i >= 0) {
                g_sheet_from = active_tab() ? active_tab()->id : 0;
                g_sheet_to = hi.id;
                g_sheet_k.snap(0); g_sheet_k.go(1, kTabMs);
                tab_activate(i);
            }
            for (auto &s : tab_slots(now_ms())) if (s.t->id == hi.id) g_drag_dx = x - s.x;
            g_drag_id = hi.id;
            SetCapture(h);
        } else if (hi.what == H_ADDR) {
            if (!ui_address_focused()) ui_focus_address(true);
        } else if (hi.what != H_NONE) SetCapture(h);
        ui_invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        HitInfo hi = hit_test(x, y);
        HitInfo pressed = g_press;
        g_press = {};
        bool was_drag = g_dragging;
        g_dragging = false;
        if (GetCapture() == h) ReleaseCapture();
        if (was_drag) { Tab *t = tab_by_id(g_drag_id); if (t) { auto sl = tab_slots(now_ms()); (void)sl; } ui_invalidate(); return 0; }
        if (hi.what == pressed.what && hi.id == pressed.id && hi.what != H_TAB) bar_click(hi, x, y);
        ui_invalidate();
        return 0;
    }
    case WM_MBUTTONUP: {
        HitInfo hi = hit_test((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        if (hi.what == H_TAB || hi.what == H_TAB_CLOSE || hi.what == H_TAB_AUDIO) {
            float w = g_geo.tab_w; tab_close(tab_index(hi.id), true); g_frozen_w = w; compute_geo();
            set_hover(hit_test((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp)));
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        HitInfo hi = hit_test((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        if (hi.what == H_TAB || hi.what == H_TAB_CLOSE || hi.what == H_EXT) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &pt);
            open_menu_page(hi.what == H_EXT ? PAGE_ONE_EXT : PAGE_TAB, { pt.x, pt.y, pt.x, pt.y }, hi.id);
        }
        return 0;
    }
    case WM_CAPTURECHANGED: if (g_dragging) { g_dragging = false; ui_invalidate(); } return 0;
    case WM_COMMAND:
        if ((HWND)lp == g_edit && HIWORD(wp) == EN_CHANGE && !g_in_change) {
            int n = GetWindowTextLengthW(g_edit);
            std::wstring t(n + 1, 0);
            GetWindowTextW(g_edit, t.data(), n + 1);
            t.resize(n);
            g_typed = t;
            update_suggestions();
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        const Palette &p = pal();
        COLORREF bg = RGB((int)(p.surface.r * 255), (int)(p.surface.g * 255), (int)(p.surface.b * 255));
        if (!g_edit_brush || bg != g_edit_bg) { if (g_edit_brush) DeleteObject(g_edit_brush); g_edit_brush = CreateSolidBrush(bg); g_edit_bg = bg; }
        SetTextColor((HDC)wp, RGB((int)(p.text.r * 255), (int)(p.text.g * 255), (int)(p.text.b * 255)));
        SetBkColor((HDC)wp, bg);
        return (LRESULT)g_edit_brush;
    }
    case WM_APP + 21: { auto f = (std::function<void()> *)lp; (*f)(); delete f; return 0; }
    case WM_APP + 22: {   // one animation frame
        g_frame_pending = false;
        double now = now_ms();
        bool busy = false;
        if (g_bar_busy) { paint(); busy |= g_bar_busy; }
        for (Panel *p : { &g_menu, &g_sug, &g_toast })
            if (p->h && (p->fade.running(now) || p->slide.running(now) || p->closing)) { panel_render(*p); busy |= p->h != NULL; }
        if (!busy) g_anim_on = false;
        return 0;
    }
    case WM_TIMER:
        if (wp == 34) {   // the mouse rests on a tab that was let go: it starts loading before the click
            KillTimer(h, 34);
            if (g_hover.what == H_TAB) tab_warm(tab_by_id(g_hover.id));
        }
        if (wp == 33) {   // the mouse rests on a site of the new tab page: it starts loading
            KillTimer(h, 33);
            Tab *t = active_tab();
            if (g_hover.what == H_NTP_ROW && g_hover.id < (int)g_ntp_rows.size() && t && !t->view && !t->creating) tabs_preload(g_ntp_rows[g_hover.id].url);
        }
        return 0;
    case WM_APP + 20:   // the address field lost the focus
        if (!ui_address_focused()) {
            ShowWindow(g_edit, SW_HIDE);
            close_suggestions();
            g_addr_focus.go(0, kHoverMs);
            Tab *t = active_tab();
            g_in_change = true; SetWindowTextW(g_edit, t ? t->url.c_str() : L""); g_in_change = false;
            ui_invalidate();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- keys
bool handle_shortcut(UINT vk, bool ctrl, bool shift, bool alt) {
    if (ctrl && !alt) {
        if (vk == 'T' && shift) { tab_reopen_closed(); return true; }
        if (vk == 'N' && shift) { tab_new(L"", true, -1, true); return true; }   // incognito, as in Chrome
        if (vk == 'T' || vk == 'N') { tab_new(L"", true); return true; }
        if (vk == 'W' || vk == VK_F4) { tab_close(g_active); return true; }
        if (vk == VK_TAB || vk == VK_PRIOR || vk == VK_NEXT) {
            if (g_tabs.size() < 2) return true;
            bool prev = vk == VK_PRIOR || (vk == VK_TAB && shift);
            int n = (int)g_tabs.size(), i = (g_active + (prev ? n - 1 : 1)) % n;
            g_sheet_from = active_tab() ? active_tab()->id : 0;
            g_sheet_k.snap(0); g_sheet_k.go(1, kTabMs);
            tab_activate(i);
            return true;
        }
        if (vk >= '1' && vk <= '9') {
            int i = vk == '9' ? (int)g_tabs.size() - 1 : (int)(vk - '1');
            if (i < (int)g_tabs.size() && i != g_active) { g_sheet_from = active_tab() ? active_tab()->id : 0; g_sheet_k.snap(0); g_sheet_k.go(1, kTabMs); tab_activate(i); }
            return true;
        }
        if (vk == 'L' || vk == 'E' || vk == 'K') { ui_focus_address(true); return true; }
        if (vk == 'J') { tab_downloads(); return true; }
        if (vk == 'D' && !shift) { ui_toggle_pin(); return true; }
        if (vk == 'Q' && shift) { app_quit(); return true; }
        if (vk == 'R' && !shift) { tab_reload(false); return true; }
        if ((vk == 'R' && shift) || vk == VK_F5) { tab_reload(true); return true; }
        if (vk == 'F' && !shift) { tab_find(); return true; }
        if (vk == 'P' && !shift) { tab_print(); return true; }
        if (vk == VK_OEM_PLUS || vk == VK_ADD) { tab_zoom(1); ui_invalidate(); return true; }
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { tab_zoom(-1); ui_invalidate(); return true; }
        if (vk == '0' || vk == VK_NUMPAD0) { tab_zoom(0); ui_invalidate(); return true; }
        return false;
    }
    if (alt && !ctrl) {
        if (vk == VK_LEFT) { tab_go_back(); return true; }
        if (vk == VK_RIGHT) { tab_go_forward(); return true; }
        if (vk == 'D') { ui_focus_address(true); return true; }
        if (vk == 'F' || vk == 'E') { ui_open_menu(); return true; }
        return false;
    }
    if (!ctrl && !alt) {
        if (vk == VK_F5) { tab_reload(false); return true; }
        if (vk == VK_F6) { ui_focus_address(true); return true; }
        if (vk == VK_F11) { set_fullscreen(!g_fullscreen); return true; }
        if (vk == VK_F12) { tab_devtools(); return true; }
        if (vk == VK_ESCAPE && g_fullscreen) {
            Tab *t = active_tab();
            BOOL page_fs = FALSE;
            if (t && t->view) t->view->get_ContainsFullScreenElement(&page_fs);
            if (!page_fs) { set_fullscreen(false); return true; }
        }
    }
    return false;
}

// ---------------------------------------------------------------- setup, layout, theme
void ui_init() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)g_dw.GetAddressOf());
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    g_s = GetDpiForWindow(g_win) / 96.0f;
    load_fonts();
    make_formats();
    g_theme.snap(dark_theme() ? 1.0f : 0.0f);
    BOOL dark = dark_theme();
    DwmSetWindowAttribute(g_win, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = bar_proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"PlumaBarra";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wc);
    WNDCLASSEXW pc = { sizeof(pc) };
    pc.lpfnWndProc = panel_proc;
    pc.hInstance = g_inst;
    pc.lpszClassName = L"PlumaPanel";
    pc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&pc);
    RECT rc; GetClientRect(g_win, &rc);
    g_bar = CreateWindowExW(0, L"PlumaBarra", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, rc.right, rc.bottom, g_win, NULL, g_inst, NULL);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 100, 20, g_bar, NULL, g_inst, NULL);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_edit_font, FALSE);
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 8000, 0);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    compute_geo();
    start_animation_thread();
}
void ui_layout() {
    if (!g_bar) return;
    RECT rc; GetClientRect(g_win, &rc);
    int band = (int)lroundf(ui_band_height());
    bool ntp = ntp_on_screen();
    if (ntp && !g_ntp_filled) ntp_fill();
    if (!ntp) { g_ntp_rows.clear(); g_ntp_filled = false; }
    int h = g_fullscreen ? 0 : (page_on_screen() && !ntp ? band : rc.bottom);
    SetWindowPos(g_bar, HWND_TOP, 0, 0, rc.right, h, SWP_NOACTIVATE | (h == 0 ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
    compute_geo();
    if (ui_address_focused()) place_edit();
    if (g_fullscreen) { ShowWindow(g_edit, SW_HIDE); ui_close_popups(); }
    tabs_layout_views();
    ui_invalidate();
}
void ui_theme_changed() {
    g_theme.go(dark_theme() ? 1.0f : 0.0f, 300);
    BOOL dark = dark_theme();
    DwmSetWindowAttribute(g_win, 20, &dark, sizeof(dark));
    InvalidateRect(g_win, NULL, FALSE);
    if (g_edit) InvalidateRect(g_edit, NULL, TRUE);
    ui_invalidate();
}
void ui_font_changed() {
    load_fonts();
    make_formats();
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
    compute_geo();
    ui_invalidate();
}
void ui_dpi_changed() {
    g_s = GetDpiForWindow(g_win) / 96.0f;
    make_formats();
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
    release_bitmaps();
    ui_layout();
}
void ui_tab_closing(Tab *t, bool from_mouse) {
    double now = now_ms();
    for (auto &s : tab_slots(now)) {
        if (s.t != t) continue;
        Ghost g;
        g.w = s.w; g.slot = tab_index(t->id); g.title = t->title;
        auto it = g_icons.find(t->id);
        if (it != g_icons.end()) g.icon = it->second;
        g.k.snap(1); g.k.go(0, kTabMs);
        if (g_motion) g_ghosts.push_back(g);
    }
    // the tabs after it shift left by one slot as the ghost shrinks
    for (auto &g : g_ghosts) if (g.slot > tab_index(t->id) && tab_index(t->id) >= 0) g.slot--;
    if (g_sheet_from == t->id) g_sheet_from = 0;
    if (!from_mouse) g_frozen_w = 0;
    ui_invalidate();
}
void ui_tab_icon_changed(Tab *t) { g_icons.erase(t->id); ui_invalidate(); }
void ui_release_icon(Tab *t) { g_icons.erase(t->id); }
