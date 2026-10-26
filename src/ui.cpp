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
                   kTabY = 4, kTabH = 30, kBtn = 32, kAddrH = 34, kPlusW = 30, kBmRowH = 32, kBmBtnH = 24, kAvatarW = 32;
static const double kHoverMs = 120, kTabMs = 180, kPanelMs = 140;
static float &ui_g_s();
#define g_s (ui_g_s())   // the window's screen scale (state of each window: UiWin, at the end)
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
                 G_INPRIVATE = 0xE727, G_FONT = 0xE8D2, G_LANG = 0xF2B7, G_UPDATE = 0xE895, G_READER = 0xE736, G_CLOUD = 0xE753,
                 G_COPY = 0xE8C8, G_WINDOW = 0xE78B, G_NEW_WINDOW_OUT = 0xE8A7, G_SWITCH = 0xE8AB, G_WARN = 0xE7BA };

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
bool dark_theme() { return g_incognito || user_dark_theme(); }   // (incognito: dark, bar and pages)
bool user_dark_theme() {
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
static ComPtr<ID2D1HwndRenderTarget> &ui_g_rt();
#define g_rt (ui_g_rt())
static D2D1_RENDER_TARGET_TYPE render_type() {
    // PLUMA_DIBUJO=cpu draws the bar without the graphics card (to compare what each costs)
    wchar_t v[8] = L"";
    GetEnvironmentVariableW(L"PLUMA_DIBUJO", v, 8);
    return !wcscmp(v, L"cpu") ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT;
}
static ComPtr<ID2D1SolidColorBrush> &ui_g_br();
#define g_br (ui_g_br())
struct Formats { ComPtr<IDWriteTextFormat> tab, addr, icon, icon_sm, icon_lg, chip, chip_b, menu, hint, word, ntp, ntp_sub, notice, avatar, avatar_lg, title; };
static Formats &ui_g_f();
#define g_f (ui_g_f())
static HFONT &ui_g_edit_font();
#define g_edit_font (ui_g_edit_font())
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
    // Edge's sizes (with its same Segoe UI Variable): tab titles 12, address bar and menus 14, shortcuts 12
    g_f.tab = make_format(false, S(12), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.addr = make_format(false, S(14), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.icon = make_format(true, S(16), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.icon_sm = make_format(true, S(10), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.icon_lg = make_format(true, S(20), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    // the "Not secure" chip before the address, as in Chrome and Edge
    g_f.chip = make_format(false, S(13), DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.chip_b = make_format(false, S(13), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.menu = make_format(false, S(14), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.hint = make_format(false, S(12), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_f.word = make_format(false, S(30), DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.ntp = make_format(false, S(13), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.ntp_sub = make_format(false, S(12), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_f.notice = make_format(false, S(14), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_f.avatar = make_format(false, S(11.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.avatar_lg = make_format(false, S(22), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.title = make_format(false, S(16), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (g_edit_font) DeleteObject(g_edit_font);
    g_edit_font = CreateFontW(-(int)lroundf(S(14)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
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
static std::vector<Ghost> &ui_g_ghosts();
#define g_ghosts (ui_g_ghosts())
static std::unordered_map<int, ComPtr<ID2D1Bitmap>> &ui_g_icons();
#define g_icons (ui_g_icons())
static std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> &ui_g_host_icons();
#define g_host_icons (ui_g_host_icons())
static std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> &ui_g_ext_icons();
#define g_ext_icons (ui_g_ext_icons())
static Anim &ui_g_sheet_k();
#define g_sheet_k (ui_g_sheet_k())
static Anim &ui_g_notice_h();
#define g_notice_h (ui_g_notice_h())
static Anim &ui_g_addr_focus();
#define g_addr_focus (ui_g_addr_focus())
static Anim &ui_g_menu_btn_open();
#define g_menu_btn_open (ui_g_menu_btn_open())
static int &ui_g_sheet_from();
#define g_sheet_from (ui_g_sheet_from())
static int &ui_g_sheet_to();
#define g_sheet_to (ui_g_sheet_to())
static float &ui_g_frozen_w();
#define g_frozen_w (ui_g_frozen_w())
enum Hit { H_NONE, H_TAB, H_TAB_CLOSE, H_TAB_AUDIO, H_PLUS, H_BACK, H_FWD, H_RELOAD, H_ADDR, H_ZOOM, H_MENU, H_EXT,
           H_MIN, H_MAX, H_CLOSE, H_NOTICE_BTN, H_NOTICE_X, H_NTP_ROW, H_STAR, H_NTP_X, H_BM, H_READER, H_PROFILE, H_TRANSLATE };
struct HitInfo { Hit what = H_NONE; int id = 0; };
static HitInfo &ui_g_hover();
#define g_hover (ui_g_hover())
static HitInfo &ui_g_press();
#define g_press (ui_g_press())
static std::unordered_map<int, Anim> &ui_g_btn_hover();
#define g_btn_hover (ui_g_btn_hover())
static int &ui_g_caption_hot();
#define g_caption_hot (ui_g_caption_hot())
static int &ui_g_caption_down();
#define g_caption_down (ui_g_caption_down())
static bool &ui_g_tracking();
#define g_tracking (ui_g_tracking())
static bool &ui_g_dragging();
#define g_dragging (ui_g_dragging())
static bool &ui_g_active_window();
#define g_active_window (ui_g_active_window())
static int &ui_g_drag_id();
#define g_drag_id (ui_g_drag_id())
static float &ui_g_drag_dx();
#define g_drag_dx (ui_g_drag_dx())
static float &ui_g_drag_x();
#define g_drag_x (ui_g_drag_x())
static float &ui_g_press_x();
#define g_press_x (ui_g_press_x())
static float &ui_g_press_y();
#define g_press_y (ui_g_press_y())
struct Notice { std::wstring text, button; std::function<void()> action; double until = 0; bool on = false; int owner = 0; };
static Notice &ui_g_notice();
#define g_notice (ui_g_notice())
static std::wstring &ui_g_addr_text();
#define g_addr_text (ui_g_addr_text())
static std::vector<HistoryEntry> &ui_g_ntp_rows();
#define g_ntp_rows (ui_g_ntp_rows())
static bool &ui_g_ntp_pins();
#define g_ntp_pins (ui_g_ntp_pins())
static bool &ui_g_ntp_filled();
#define g_ntp_filled (ui_g_ntp_filled())
static double g_ntp_mem_at = -1e9; static size_t g_ntp_mem;
static WNDPROC g_edit_proc;
static bool g_in_change;
static std::wstring &ui_g_typed();
#define g_typed (ui_g_typed())
static void close_suggestions();
static void update_suggestions();
static void sug_move(int d);
static bool sug_commit(bool new_tab);
static void open_menu_page(int page, RECT anchor, int tab_id = 0, bool quiet = false);
static bool g_menu_page_is_profiles();   // the profile button's menu is open (its button stays lit)
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
static bool bm_bar_shown() { return g_set.bookmarks_bar && !g_fullscreen; }
static float bm_bar_h() { return bm_bar_shown() ? S(kBmRowH) : 0; }
float ui_band_height() {
    if (g_fullscreen) return 0;
    // the note's room is taken at once (the page is laid out once, not every frame); the card slides into it
    return S(kTabRowH + kToolRowH) + bm_bar_h() + S(kNoticeH) * g_notice_h.to;
}

// ---------------------------------------------------------------- layout of the tab row
struct Geo { float W, strip_l, strip_r, cap_l, tab_w, plus_x, addr_l, addr_r, menu_x, ext_l, avatar_l, avatar_w; };
static Geo &ui_g_geo();
#define g_geo (ui_g_geo())
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
    // the profile's avatar (as Edge's) right after the address; in incognito, its badge instead
    g.avatar_w = g_incognito ? text_w(g_f.hint.Get(), T(L"Incógnito")) + S(38) : S(kAvatarW);
    g.avatar_l = g.ext_l - S(2) - g.avatar_w;
    g.addr_l = S(8 + (kBtn + 4) * 3 + 2);
    g.addr_r = g.avatar_l - S(6);
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
static float &ui_g_strip_end();
#define g_strip_end (ui_g_strip_end())
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
static D2D1_COLOR_F rgb_of(unsigned c) { return hex(c & 0xffffff); }
static std::wstring initial_of(const std::wstring &name) {
    std::wstring n = trim(name);
    if (n.empty()) return L"?";
    std::wstring one = n.substr(0, IS_HIGH_SURROGATE(n[0]) && n.size() > 1 ? 2 : 1);
    CharUpperBuffW(one.data(), (DWORD)one.size());
    return one;
}
// a profile's avatar: its colour, its initial in white (big: the flyout's header)
static void draw_avatar(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, float cx, float cy, float r, const Profile &p, bool big = false) {
    b->SetColor(rgb_of(p.color));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b);
    text(rt, b, (big ? g_f.avatar_lg : g_f.avatar).Get(), initial_of(profile_shown_name(p)), R(cx - r, cy - r, cx + r, cy + r), hex(0xffffff));
}
static D2D1_RECT_F cap_rect(int i) { return R(g_geo.cap_l + S(kCapW) * i, 0, g_geo.cap_l + S(kCapW) * (i + 1), S(kTabRowH)); }
static float tool_y() { return S(kTabRowH); }
static D2D1_RECT_F btn_rect(int i) {   // 0 back, 1 forward, 2 reload
    float y = tool_y() + S((kToolRowH - kBtn) / 2);
    float x = S(8) + S(kBtn + 4) * i;
    return R(x, y, x + S(kBtn), y + S(kBtn));
}
static D2D1_RECT_F profile_rect() {
    float y = tool_y() + S((kToolRowH - kBtn) / 2);
    return R(g_geo.avatar_l, y, g_geo.avatar_l + g_geo.avatar_w, y + S(kBtn));
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
// the book before it: the page is an article, and the reader view is a click (or F9) away (reader.cpp)
static bool reader_shown() { Tab *t = active_tab(); return star_shown() && (t->readerable || t->reader); }
static D2D1_RECT_F reader_rect() {
    D2D1_RECT_F s = star_rect();
    return R(s.left - S(30), s.top, s.left - S(2), s.bottom);
}
// the translate icon before them: the page is in another language, or translated (translate.cpp)
static bool translate_shown() { Tab *t = active_tab(); return star_shown() && (t->tr_state || (t->tr_offer && g_set.tr_offer)); }
static D2D1_RECT_F translate_rect() {
    D2D1_RECT_F s = reader_shown() ? reader_rect() : star_rect();
    return R(s.left - S(30), s.top, s.left - S(2), s.bottom);
}
static D2D1_RECT_F zoom_rect() {
    D2D1_RECT_F a = addr_rect();
    float r = translate_shown() ? translate_rect().left - S(4) : reader_shown() ? reader_rect().left - S(4) : star_shown() ? star_rect().left - S(4) : a.right - S(6);
    return R(r - S(56), a.top + S(5), r, a.bottom - S(5));
}
static bool zoom_shown() { double z = tab_zoom_factor(); return fabs(z - 1.0) > 0.01; }
// the optional bookmarks bar: a row under the toolbar with the "Barra de marcadores" folder's top-level items
struct BmBtn { int id; bool folder; float x, w; std::wstring title; };
static std::vector<BmBtn> bm_bar_items() {
    std::vector<BmBtn> out;
    if (!bm_bar_shown()) return out;
    BmNode *f = bm_find(bm_bar_folder());
    if (!f) return out;
    float x = S(10);
    for (auto &k : f->kids) {
        std::wstring t = k.title.empty() ? (k.folder ? T(L"Carpeta") : url_host(k.url)) : k.title;
        if (t.size() > 28) t = t.substr(0, 27) + L"…";
        float w = text_w(g_f.menu.Get(), t) + S(28);
        if (x + w > g_geo.W - S(10) && !out.empty()) break;   // no overflow menu yet: what fits
        out.push_back({ k.id, k.folder, x, w, t });
        x += w + S(4);
    }
    return out;
}
static D2D1_RECT_F bm_btn_rect(const BmBtn &b) {
    float y = S(kTabRowH + kToolRowH) + (bm_bar_h() - S(kBmBtnH)) / 2;
    return R(b.x, y, b.x + b.w, y + S(kBmBtnH));
}
static D2D1_RECT_F notice_rect() {
    float y = S(kTabRowH + kToolRowH) + bm_bar_h();
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
        if (inside(profile_rect(), x, y)) { h.what = H_PROFILE; return h; }
        size_t k = 0;
        for (size_t i = 0; i < g_exts.size() && k < ext_buttons(); i++) {
            if (!g_exts[i].enabled || g_exts[i].popup.empty()) continue;
            if (inside(ext_rect(k), x, y)) { h.what = H_EXT; h.id = (int)i; return h; }
            k++;
        }
        if (star_shown() && inside(star_rect(), x, y)) { h.what = H_STAR; return h; }
        if (reader_shown() && inside(reader_rect(), x, y)) { h.what = H_READER; return h; }
        if (translate_shown() && inside(translate_rect(), x, y)) { h.what = H_TRANSLATE; return h; }
        if (zoom_shown() && inside(zoom_rect(), x, y)) { h.what = H_ZOOM; return h; }
        if (inside(addr_rect(), x, y)) { h.what = H_ADDR; return h; }
        return h;
    }
    if (bm_bar_shown() && y < S(kTabRowH + kToolRowH) + bm_bar_h()) {
        for (auto &b : bm_bar_items()) if (inside(bm_btn_rect(b), x, y)) { h.what = H_BM; h.id = b.id; return h; }
        return h;
    }
    if (g_notice.on && y < S(kTabRowH + kToolRowH) + bm_bar_h() + S(kNoticeH)) {
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
static D2D1_COLOR_F strip_color(const Palette &p) {
    // the tabs' row, a step darker than the bar (as Edge's): the tab you are on stands out from the rest
    return mix(p.bg, dark_theme() ? hex(0x000000) : hex(0x1d2733), dark_theme() ? 0.32f : 0.075f);
}
static bool paint_tabs(ID2D1RenderTarget *rt, double now) {
    const Palette &p = pal();
    bool busy = false;
    fill(rt, g_br.Get(), R(0, 0, g_geo.W, S(kTabRowH)), 0, strip_color(p));
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
    for (size_t i = 0; i + 1 < slots.size(); i++) {
        // a thin line between two tabs, unless one of them is the active one or under the mouse
        Tab *a = slots[i].t, *b = slots[i + 1].t;
        if (a == act || b == act || a->hover.at(now) > 0 || b->hover.at(now) > 0) continue;
        D2D1_RECT_F r = tab_rect(slots[i]);
        float x = r.right + S(kTabGap) / 2;
        fill(rt, g_br.Get(), R(x - S(0.5f), r.top + S(8), x + S(0.5f), r.bottom - S(8)), 0, alpha(p.text, dark_theme() ? 0.18f : 0.16f));
    }
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
        else if (settings_page_url(t->url)) glyph(rt, g_br.Get(), g_f.icon.Get(), G_GEAR, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), p.text);
        else if (t->incognito && t->url.empty()) {
            // incognito's new tab: Edge's InPrivate sign, in the accent (pages keep their icon, as in Edge)
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
            float vis = 1;   // (on every tab, as in Edge: closing one you are not on takes one click)
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
    glyph(rt, g_br.Get(), g_f.icon.Get(), G_ADD, pr, p.text);
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
// this computer's own servers are not "not secure" (Chrome treats them as secure contexts)
static bool local_host(const std::wstring &u) {
    std::wstring h = url_host(u);
    if (starts_with(h, L"[")) return starts_with(h, L"[::1]");
    h = h.substr(0, h.find(L':'));
    return h == L"localhost" || (h.size() > 10 && h.compare(h.size() - 10, 10, L".localhost") == 0) || starts_with(h, L"127.");
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
    {
        // the profile's avatar, lit while its flyout is open; in incognito, the badge (as Edge's "InPrivate")
        D2D1_RECT_F ar = profile_rect();
        float ah = std::max(hov(H_PROFILE, 0), g_menu_page_is_profiles() ? 1.f : 0.f);
        busy |= g_btn_hover[hover_key(H_PROFILE, 0)].running(now);
        bool down = g_press.what == H_PROFILE && g_hover.what == H_PROFILE;
        float cy = (ar.top + ar.bottom) / 2;
        if (g_incognito) {
            D2D1_RECT_F pr = R(ar.left, ar.top + S(3), ar.right, ar.bottom - S(3));
            fill(rt, g_br.Get(), pr, S(8), down ? p.press : mix(alpha(p.accent_soft, 0.55f), p.hover, ah * 0.6f));
            glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_INPRIVATE, R(pr.left + S(6), pr.top, pr.left + S(28), pr.bottom), p.accent);
            text(rt, g_br.Get(), g_f.hint.Get(), T(L"Incógnito"), R(pr.left + S(28), pr.top, pr.right - S(8), pr.bottom), p.accent);
        } else {
            if (ah > 0 || down) fill(rt, g_br.Get(), ar, S(8), down ? p.press : alpha(p.hover, ah * 1.3f));
            draw_avatar(rt, g_br.Get(), (ar.left + ar.right) / 2, cy, S(11), profile_current());
        }
    }
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
    std::wstring u = t ? settings_page_display(t->url) : L"";
    wchar_t ig = G_SEARCH;
    D2D1_COLOR_F ic = p.sub;
    // the site info, the size of the toolbar's glyphs; a page that is not safe says so in red, as in Chrome and Edge:
    // plain http (not this computer's own) on a soft red pill, a certificate error on a stronger one
    int unsafe = 0;   // 1 http, 2 certificate error
    if (!focused && !u.empty()) {
        if (starts_with(u, L"https://")) { ig = G_LOCK; if (t->cert_error) unsafe = 2; }
        else if (starts_with(u, L"http://")) { ig = G_INFO; if (!local_host(u)) unsafe = 1; }
        else if (starts_with(u, L"file:")) ig = G_FILE;
        else if (starts_with(u, L"chrome-extension:")) ig = G_PUZZLE;
        else ig = G_INFO;
    }
    float text_l = a.left + S(36);
    if (unsafe) {
        ig = G_WARN; ic = p.error;
        auto cf = unsafe == 2 ? g_f.chip_b : g_f.chip;
        std::wstring label = T(L"No seguro");
        D2D1_RECT_F chip = R(a.left + S(5), a.top + S(5), ix + S(13) + text_w(cf.Get(), label) + S(9), a.bottom - S(5));
        float k = (unsafe == 2 ? 0.20f : 0.10f) * (dark_theme() ? 1.6f : 1);
        fill(rt, g_br.Get(), chip, S(7), alpha(p.error, k));
        if (unsafe == 2) stroke(rt, g_br.Get(), chip, S(7), alpha(p.error, 0.55f), S(1));
        text(rt, g_br.Get(), cf.Get(), label, R(ix + S(13), a.top, chip.right, a.bottom), p.error);
        text_l = chip.right + S(8);
    }
    glyph(rt, g_br.Get(), g_f.icon.Get(), ig, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), ic);
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
    if (reader_shown()) {
        // the book: in the accent while the reader view is on screen
        D2D1_RECT_F r = reader_rect();
        auto &ra = g_btn_hover[hover_key(H_READER, 0)];
        busy |= ra.running(now);
        float rh = ra.at(now);
        if (t->reader) fill(rt, g_br.Get(), r, S(7), p.accent_soft);
        else if (rh > 0) fill(rt, g_br.Get(), r, S(7), alpha(p.hover, rh * 1.3f));
        glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_READER, r, t->reader ? p.accent : mix(p.sub, p.text, rh));
        right = r.left - S(4);
    }
    if (translate_shown()) {
        // translate: in the accent while offered (the page is in another language), on a soft fill once translated
        D2D1_RECT_F r = translate_rect();
        auto &ta = g_btn_hover[hover_key(H_TRANSLATE, 0)];
        busy |= ta.running(now);
        float th = ta.at(now);
        if (t->tr_state) fill(rt, g_br.Get(), r, S(7), p.accent_soft);
        else if (th > 0) fill(rt, g_br.Get(), r, S(7), alpha(p.hover, th * 1.3f));
        glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_LANG, r, t->tr_state || t->tr_offer ? p.accent : mix(p.sub, p.text, th));
        right = r.left - S(4);
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
    if (!focused) {
        D2D1_RECT_F tr = R(text_l, a.top, right, a.bottom);
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
static bool paint_bm_bar(ID2D1RenderTarget *rt, double now) {
    if (!bm_bar_shown()) return false;
    const Palette &p = pal();
    bool busy = false;
    float y0 = S(kTabRowH + kToolRowH);
    fill(rt, g_br.Get(), R(0, y0, g_geo.W, y0 + bm_bar_h()), 0, p.bg);
    fill(rt, g_br.Get(), R(0, y0 + bm_bar_h() - S(1), g_geo.W, y0 + bm_bar_h()), 0, p.line);
    auto items = bm_bar_items();
    if (items.empty()) {
        text(rt, g_br.Get(), g_f.hint.Get(), T(L"Marca páginas con la estrella para verlas aquí."), R(S(12), y0, g_geo.W - S(12), y0 + bm_bar_h()), p.sub);
        return false;
    }
    for (auto &b : items) {
        D2D1_RECT_F r = bm_btn_rect(b);
        auto &a = g_btn_hover[hover_key(H_BM, b.id)];
        busy |= a.running(now);
        if (a.at(now) > 0) fill(rt, g_br.Get(), r, S(7), alpha(p.hover, a.at(now) * 1.3f));
        float cy = (r.top + r.bottom) / 2;
        if (b.folder) glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_FOLDER, R(r.left + S(4), cy - S(9), r.left + S(22), cy + S(9)), p.sub);
        else {
            auto ic = host_icon(rt, g_host_icons, bm_find(b.id) ? bm_find(b.id)->url : L"");
            if (ic) draw_icon_bitmap(rt, ic.Get(), r.left + S(14), cy, S(16), 1);
            else glyph(rt, g_br.Get(), g_f.icon_sm.Get(), G_GLOBE, R(r.left + S(4), cy - S(9), r.left + S(22), cy + S(9)), p.sub);
        }
        text(rt, g_br.Get(), g_f.menu.Get(), b.title, R(r.left + S(24), r.top, r.right - S(4), r.bottom), p.text);
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
static bool &ui_g_bar_busy();
#define g_bar_busy (ui_g_bar_busy())
static void want_frames() {
    if (!g_anim_event) return;
    if (!g_anim_on.exchange(true)) SetEvent(g_anim_event);
}
static void anim_thread() {
    while (true) {
        WaitForSingleObject(g_anim_event, INFINITE);
        while (g_anim_on) {
            if (FAILED(DwmFlush())) Sleep(16);
            if (!g_frame_pending.exchange(true)) PostMessageW(g_app, WM_APP + 22, 0, 0);   // (ui_frame, for every window)
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
        busy |= paint_bm_bar(g_rt.Get(), now);
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
void ui_post(std::function<void()> f) { PostMessageW(g_app, WM_APP + 21, 0, (LPARAM)new std::function<void()>(std::move(f))); }

// ---------------------------------------------------------------- the address field
bool ui_address_focused() { return g_edit && GetFocus() == g_edit; }
static void place_edit() {
    D2D1_RECT_F a = addr_rect();
    int h = (int)lroundf(S(18));
    float right = zoom_shown() ? zoom_rect().left - S(6) : a.right - S(12);
    SetWindowPos(g_edit, NULL, (int)(a.left + S(36)), (int)((a.top + a.bottom) / 2 - h / 2.0f), (int)(right - a.left - S(36)), h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}
void ui_set_address_text(const std::wstring &url) {
    std::wstring s = settings_page_display(url);   // (Pluma's own pages: pluma://...)
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
// Enter, or a suggestion or a site chosen: the bar leaves edit mode at once, as in Chrome and Edge, and the page gets
// the focus. A tab with no view yet (a new tab, the preload that takes its place) gets it when its view comes
// (tab_activate); till then the window holds it. (Not the tab held before tab_navigate: a preload replaced it.)
static void address_done(const std::wstring &url) {
    blur_address(true);
    if (ui_address_focused()) SetFocus(g_win);
    ui_set_address_text(url);
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
    address_done(url);
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
    Window *self = win_of_hwnd(h);
    if (!self) return CallWindowProcW(g_edit_proc, h, msg, wp, lp);
    WinScope ws(self);
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
        if ((ctrl && wcschr(L"TWNJQ1234567890\x09\x21\x22", (wchar_t)wp)) || wp == VK_F5 || wp == VK_F6 || wp == VK_F9 || wp == VK_F11 || wp == VK_F12 ||
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
static Panel g_menu, g_sug, g_toast, g_bubble, g_tip;
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
    WinScope ws(win_of_hwnd(GetWindow(p.h, GW_OWNER)));   // (drawn with its window's scale and fonts)
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
    if (p.closing || (p.h && GetWindow(p.h, GW_OWNER) != g_win)) panel_free(p);   // (open over another window: it moves to this one)
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
        if (&p == &g_toast || &p == &g_tip) ex |= WS_EX_TRANSPARENT;
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
    address_done(url);
    return true;
}

// ---------------------------------------------------------------- the menu
// each language by its own name; "" is Windows's
std::wstring lang_name(const wchar_t *code) {
    static const struct { const wchar_t *code, *name; } names[] = { { L"es", L"Español" }, { L"en", L"English" }, { L"pt", L"Português" },
                                                                    { L"fr", L"Français" }, { L"de", L"Deutsch" }, { L"it", L"Italiano" } };
    for (auto &n : names) if (!wcscmp(code, n.code)) return n.name;
    return T(L"Como Windows");
}
enum { PAGE_MAIN, PAGE_EXT, PAGE_SCRIPTS, PAGE_ENGINE, PAGE_THEME, PAGE_FONT, PAGE_CLEAR, PAGE_TAB, PAGE_ONE_EXT, PAGE_BOOKMARKS, PAGE_STAR, PAGE_BM_OPEN, PAGE_PROFILES, PAGE_TRANSLATE, PAGE_TR_TARGET, PAGE_INCOGNITO, PAGE_DOWNLOADS };
struct MItem {
    int kind = 0;   // 0 action, 1 separator, 2 opens a page, 3 check, 4 radio, 5 zoom row, 6 note, 7 back, 8 header (the profile's flyout),
                    // 9 a download (name, its bar while on its way, its state under it; hint = the state)
    std::wstring text, hint;
    wchar_t icon = 0;
    bool on = false, enabled = true, danger = false;
    std::function<void()> act;
    int page = 0;
    wchar_t b1 = 0, b2 = 0;                 // small buttons at the right of the row
    bool avatar = false; Profile prof;      // a profile's row: its avatar instead of an icon
    std::function<void()> act1, act2;
    float progress = -1; int dl_state = 0;   // a download's row (DownloadRow)
};
static std::vector<MItem> g_items;
static int g_menu_page = PAGE_MAIN, g_menu_tab = 0, g_menu_sel = -1, g_menu_hot_btn = 0;
static bool g_menu_page_is_profiles() { return g_menu.h && !g_menu.closing && (g_menu_page == PAGE_PROFILES || g_menu_page == PAGE_INCOGNITO); }
static RECT g_menu_anchor;
static bool g_menu_quiet;              // opened by Pluma itself (the translate offer): never takes the focus, closes on its own
static bool g_menu_quiet_touched;      // ... the mouse went over it (then it stays until a click elsewhere)
static double g_menu_quiet_since;
static int g_menu_quiet_tab;           // ... and the tab on screen then (another one: it goes)
static bool keep_on_screen() { return !g_test || g_test_visible; }   // (a test instance's panels stay off the screen with it)
static std::wstring fmt1(const wchar_t *tpl, const std::wstring &a) {
    std::wstring s = tpl;
    size_t at = s.find(L"%1");
    if (at != std::wstring::npos) s.replace(at, 2, a);
    return s;
}
static const float kMenuW = 344, kItemH = 34, kSepH = 9;
static float item_h(const MItem &m) { return m.kind == 1 ? S(kSepH) : m.kind == 6 ? S(30) : m.kind == 8 ? S(76) : m.kind == 9 ? S(54) : S(kItemH); }
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
    if (g_items[i].kind == 9) { float cy = (r.top + r.bottom) / 2; return R(x, cy - S(15), x + S(28), cy + S(15)); }   // (a tall row: a square button)
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
        if (m.kind == 8) {
            // as Edge's flyout: the profile big, its avatar and its name (incognito: its sign on the accent)
            float cx = r.left + S(36), cy = (r.top + r.bottom) / 2, rad = S(24);
            if (m.avatar) draw_avatar(p.rt.Get(), p.br.Get(), cx, cy, rad, m.prof, true);
            else {
                p.br->SetColor(c.accent_soft);
                p.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), p.br.Get());
                glyph(p.rt.Get(), p.br.Get(), g_f.icon_lg.Get(), m.icon, R(cx - rad, cy - rad, cx + rad, cy + rad), c.accent);
            }
            float tl = cx + rad + S(16), tr = r.right - S(12);
            if (m.hint.empty()) text(p.rt.Get(), p.br.Get(), g_f.title.Get(), m.text, R(tl, r.top, tr, r.bottom), c.text);
            else {
                text(p.rt.Get(), p.br.Get(), g_f.title.Get(), m.text, R(tl, cy - S(23), tr, cy + S(1)), c.text);
                text(p.rt.Get(), p.br.Get(), g_f.ntp.Get(), m.hint, R(tl, cy + S(1), tr, cy + S(21)), c.sub);
            }
            continue;
        }
        if (m.kind == 9) {
            // a download, as in Edge's flyout: its file, its name, a bar while it comes, its state; the button at the right
            bool hot = (int)i == g_menu_sel && (m.act || (m.b1 && g_menu_hot_btn == 1));
            if (hot) fill(p.rt.Get(), p.br.Get(), r, S(9), c.hover);
            float ix = r.left + S(18), cy = (r.top + r.bottom) / 2, tr = r.right - S(12);
            glyph(p.rt.Get(), p.br.Get(), g_f.icon.Get(), G_FILE, R(ix - S(12), cy - S(12), ix + S(12), cy + S(12)), m.dl_state == 2 ? c.error : c.sub);
            if (m.b1) {
                D2D1_RECT_F b = item_btn(i, 1); b.left += dx; b.right += dx;
                if (g_menu_sel == (int)i && g_menu_hot_btn == 1) fill(p.rt.Get(), p.br.Get(), b, S(7), c.press);
                glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), m.b1, b, c.sub);
                tr = b.left - S(4);
            }
            float tl = r.left + S(38);
            text(p.rt.Get(), p.br.Get(), g_f.menu.Get(), m.text, R(tl, r.top + S(4), tr, cy + S(1)), m.dl_state >= 2 ? c.sub : c.text);
            if (m.dl_state == 0) {
                D2D1_RECT_F bar = R(tl, cy + S(4), tr, cy + S(7));
                fill(p.rt.Get(), p.br.Get(), bar, S(1.5f), c.line);
                float f = m.progress < 0 ? 0.3f : m.progress;   // (size unknown: a short piece)
                if (f > 0) fill(p.rt.Get(), p.br.Get(), R(bar.left, bar.top, bar.left + (bar.right - bar.left) * f, bar.bottom), S(1.5f), m.progress < 0 ? c.accent_soft : c.accent);
                text(p.rt.Get(), p.br.Get(), g_f.tab.Get(), m.hint, R(tl, cy + S(8), tr, r.bottom - S(2)), c.sub);
            } else {
                D2D1_RECT_F st = R(tl, cy + S(1), tr, r.bottom - S(6));
                text(p.rt.Get(), p.br.Get(), g_f.tab.Get(), m.hint, st, m.dl_state == 2 ? c.error : c.sub);
                // done: says what a click does, as Edge's "Open file"
                if (m.dl_state == 1) text(p.rt.Get(), p.br.Get(), g_f.tab.Get(), T(L"Abrir archivo"), R(tl + text_w(g_f.tab.Get(), m.hint) + S(10), st.top, tr, st.bottom), c.accent);
            }
            continue;
        }
        bool sel = (int)i == g_menu_sel && m.enabled && m.kind != 5;
        if (sel) fill(p.rt.Get(), p.br.Get(), r, S(9), c.hover);
        D2D1_COLOR_F ink = !m.enabled ? alpha(c.sub, 0.55f) : m.danger ? c.error : c.text;
        float ix = r.left + S(18), cy = (r.top + r.bottom) / 2;
        wchar_t ig = m.icon;
        if (m.kind == 3) ig = m.on ? G_CHECK : 0;
        if (m.kind == 4) ig = m.on ? G_CHECK : 0;
        if (m.kind == 7) ig = G_CHEV_L;
        if (m.avatar) draw_avatar(p.rt.Get(), p.br.Get(), ix, cy, S(11), m.prof);
        else if (ig) glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), ig, R(ix - S(10), cy - S(10), ix + S(10), cy + S(10)), m.kind >= 3 && m.kind <= 4 ? c.accent : (m.danger ? c.error : c.sub));
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
    if (keep_on_screen()) {
        x = std::max((int)mi.rcWork.left - (int)pad, std::min(x, (int)mi.rcWork.right - w + (int)pad));
        if (y + hh > mi.rcWork.bottom + pad) y = std::max((int)mi.rcWork.top, (int)(mi.rcWork.bottom + pad) - hh);
    }
    g_menu.draw = menu_draw;
    bool fresh = !g_menu.h || g_menu.closing;
    panel_open(g_menu, x, y, w, hh, !g_menu_quiet);
    if (!fresh && from_side) { g_menu.slide.snap(1); g_menu.slide.go(0, kPanelMs); panel_render(g_menu); }
}
static void open_menu_page(int page, RECT anchor, int tab_id, bool quiet) {
    // (a quiet one's window cannot take the focus: the person's own menu is a new window)
    if (g_menu.h && g_menu_quiet && !quiet) panel_free(g_menu);
    g_menu_quiet = quiet; g_menu_quiet_touched = false; g_menu_quiet_since = now_ms();
    { Tab *a = active_tab(); g_menu_quiet_tab = a ? a->id : 0; }
    g_menu_page = page; g_menu_tab = tab_id; g_menu_sel = -1; g_menu_anchor = anchor;
    close_suggestions();
    menu_place(false);
    if (quiet && g_menu.h) SetTimer(g_menu.h, 6, 200, NULL);
    if (!quiet) g_menu_btn_open.go(1, kHoverMs);
}
static void menu_close() {
    if (!g_menu.h) return;
    if (g_menu_quiet) { KillTimer(g_menu.h, 6); g_menu_quiet = false; }
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
void data_clear_all() {
    ComPtr<ICoreWebView2_13> v13; ComPtr<ICoreWebView2Profile> pr; ComPtr<ICoreWebView2Profile2> p2;
    Tab *t = nullptr;
    for (Tab *x : all_tabs()) if (x->view && !x->incognito) { t = x; break; }
    history_clear();
    if (t && SUCCEEDED(t->view.As(&v13)) && SUCCEEDED(v13->get_Profile(&pr)) && SUCCEEDED(pr.As(&p2)))
        p2->ClearBrowsingData((COREWEBVIEW2_BROWSING_DATA_KINDS)(COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_SITE | COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE |
                              COREWEBVIEW2_BROWSING_DATA_KINDS_DOWNLOAD_HISTORY | COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY),
                              Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([](HRESULT) -> HRESULT { ui_toast(T(L"Datos borrados: cookies, caché e historial")); return S_OK; }).Get());
    else ui_toast(T(L"Historial borrado"));
}
void ui_ntp_changed() { g_ntp_filled = false; ui_invalidate(); }
void ui_open_menu_page(const std::wstring &which) {
    // the settings page's "Extensiones" and "Scripts": the menu, open at that page
    D2D1_RECT_F m = menu_rect();
    POINT a = { (LONG)m.left, (LONG)m.top }, b = { (LONG)m.right, (LONG)m.bottom };
    ClientToScreen(g_bar, &a); ClientToScreen(g_bar, &b);
    open_menu_page(which == L"scripts" ? PAGE_SCRIPTS : PAGE_EXT, { a.x, a.y, b.x, b.y });
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
        v.push_back(item(T(L"Nueva ventana"), G_WINDOW, T(L"Ctrl+N"), [] { after_menu([] { win_open_new(); }); }));
        v.push_back(item(T(L"Nueva ventana de incógnito"), G_INPRIVATE, T(L"Ctrl+Mayús+N"), [] { after_menu([] { app_open_incognito(); }); }));
        MItem re = item(T(L"Reabrir pestaña cerrada"), G_UNDO, T(L"Ctrl+Mayús+T"), [] { after_menu([] { tab_reopen_closed(); }); });
        v.push_back(re);
        v.push_back(sep());
        MItem z; z.kind = 5; z.text = L"Zoom"; z.icon = G_ZOOM; v.push_back(z);
        v.push_back(item(T(L"Descargas"), G_DOWNLOAD, T(L"Ctrl+J"), [] { menu_go(PAGE_DOWNLOADS); }));
        Tab *at = active_tab();
        if (at && at->view && !settings_page_url(at->url) && (starts_with(at->url, L"https://") || starts_with(at->url, L"http://"))) {
            if (at->tr_state) v.push_back(item(T(L"Mostrar original"), G_LANG, L"", [] { after_menu([] { translate_original(active_tab()); }); }));
            else v.push_back(item(T(L"Traducir página"), G_LANG, translate_lang_name(translate_target(), true), [] { after_menu([] { translate_page(active_tab()); }); }));
        }
        v.push_back(sep());
        size_t on = 0; for (auto &e : g_exts) if (!e.builtin && e.enabled) on++;
        v.push_back(sub(T(L"Extensiones"), G_PUZZLE, on == 1 ? T(L"1 activa") : on ? std::to_wstring(on) + T(L" activas") : L"", PAGE_EXT));
        size_t son = 0; for (auto &s : g_scripts) if (s.enabled) son++;
        v.push_back(sub(T(L"Scripts de usuario"), G_CODE, son == 1 ? T(L"1 activo") : son ? std::to_wstring(son) + T(L" activos") : L"", PAGE_SCRIPTS));
        v.push_back(sub(T(L"Marcadores"), G_STAR, L"", PAGE_BOOKMARKS));
        v.push_back(sub(T(L"Buscador"), G_GLOBE, engine_name(g_set.engine), PAGE_ENGINE));
        v.push_back(sub(T(L"Tema"), G_COLOR, g_set.theme == 1 ? T(L"Oscuro") : g_set.theme == 2 ? T(L"Como Windows") : T(L"Claro"), PAGE_THEME));
        v.push_back(sub(T(L"Fuente"), G_FONT, g_set.font.empty() ? T(L"La de Windows") : g_set.font, PAGE_FONT));
        v.push_back(sub(T(L"Borrar datos"), G_ERASE, L"", PAGE_CLEAR));
        v.push_back(item(T(L"Configuración"), G_GEAR, L"", [] { after_menu([] { settings_open(); }); }));
        if (!g_incognito)
            v.push_back(item(T(L"Sincronizar"), G_CLOUD, L"", [] { after_menu([] { settings_open_sync(); }); }));
        v.push_back(item(T(L"Herramientas de desarrollo"), G_DEV, L"F12", [] { after_menu([] { tab_devtools(); }); }));
        if (!g_incognito)
            v.push_back(item(T(L"Buscar actualizaciones"), G_UPDATE, update_version(), [] { after_menu([] { update_check_now(); }); }));
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
    } else if (page == PAGE_CLEAR) {
        v.push_back(back(T(L"Borrar datos")));
        v.push_back(item(T(L"El historial de Pluma"), G_CLOCK, L"", [] { history_clear(); menu_close(); ui_toast(T(L"Historial borrado")); }));
        MItem all = item(T(L"Todo menos las contraseñas"), G_ERASE, L"", [] { data_clear_all(); menu_close(); });
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
        // to another window, as Edge's "Move tab to": a new one, or each of the others (by the tab it shows)
        if (g_tabs.size() > 1)
            v.push_back(item(T(L"Mover a una ventana nueva"), G_NEW_WINDOW_OUT, L"", [id] { after_menu([id] { Tab *t = tab_by_id(id); if (t) tab_to_new_window(t, nullptr); }); }));
        for (auto &w : g_windows) {
            if (w.get() == g_cw) continue;
            int wid = w->id;
            v.push_back(item(T(L"Mover a ") + win_name(w.get()), G_SWITCH, L"", [id, wid] { after_menu([id, wid] {
                Tab *t = tab_by_id(id);
                Window *to = win_by_id(wid);
                if (!t || !to) return;
                Window *from = win_of_tab(t);
                tab_move(t, to, (int)to->tabs.size(), true);
                win_show(to, true);
                if (from && from->tabs.empty()) win_close(from);
            }); }));
        }
        v.push_back(sep());
        v.push_back(item(T(L"Cerrar las de la derecha"), G_X, L"", [id] { after_menu([id] { int i = tab_index(id); while (i >= 0 && (int)g_tabs.size() > i + 1) tab_close((int)g_tabs.size() - 1); }); }));
        v.push_back(item(T(L"Cerrar las demás"), G_X, L"", [id] { after_menu([id] { for (int k = (int)g_tabs.size() - 1; k >= 0; k--) if (g_tabs[k]->id != id) tab_close(k); }); }));
        v.push_back(item(T(L"Cerrar"), G_X, T(L"Ctrl+W"), [id] { after_menu([id] { tab_close(tab_index(id)); }); }));
    } else if (page == PAGE_BOOKMARKS) {
        v.push_back(back(T(L"Marcadores")));
        v.push_back(item(T(L"Administrador de marcadores"), G_STAR, T(L"Ctrl+Mayús+O"), [] { after_menu([] { bookmarks_open(); }); }));
        MItem bar; bar.kind = 3; bar.text = T(L"Mostrar barra de marcadores"); bar.on = g_set.bookmarks_bar; bar.hint = T(L"Ctrl+Mayús+B");
        bar.act = [] { g_set.bookmarks_bar = !g_set.bookmarks_bar; settings_save(); ui_layout(); };
        v.push_back(bar);
        v.push_back(sep());
        v.push_back(item(T(L"Importar de otro navegador"), G_DOWNLOAD, L"", [] { after_menu([] { import_open(); }); }));
    } else if (page == PAGE_STAR) {
        // right click on the star: which folder the page on screen goes in
        v.push_back(note(T(L"Guardar en")));
        Tab *t = active_tab();
        std::wstring url = t ? t->url : L"", title = t ? (t->title.empty() ? url_host(t->url) : t->title) : L"";
        std::function<void(const BmNode &, int)> add = [&](const BmNode &n, int depth) {
            for (auto &k : n.kids) {
                if (!k.folder) continue;
                MItem m; m.kind = 4; m.on = bm_url_in(k.id, url);
                m.text = std::wstring(depth * 2, L' ') + bm_shown_title(k);
                int id = k.id;
                m.act = [id, url, title] { bm_toggle_in(id, url, title); menu_rebuild(); panel_render(g_menu); ui_layout(); };
                v.push_back(m);
                add(k, depth + 1);
            }
        };
        add(bm_root(), 0);
        v.push_back(sep());
        v.push_back(item(T(L"Administrador de marcadores"), G_STAR, L"", [] { after_menu([] { bookmarks_open(); }); }));
    } else if (page == PAGE_BM_OPEN) {
        // a folder on the bookmarks bar: open its bookmarks
        BmNode *f = bm_find(g_menu_tab);
        if (!f) return v;
        bool any = false;
        for (auto &k : f->kids) {
            if (k.folder) continue;
            any = true;
            std::wstring u = k.url, t = k.title.empty() ? url_host(k.url) : k.title;
            v.push_back(item(t, G_GLOBE, L"", [u] { after_menu([u] { Tab *a = active_tab(); if (a && a->url.empty()) tab_navigate(a, u); else tab_new(u, true); }); }));
        }
        if (!any) v.push_back(note(T(L"Carpeta vacía")));
    } else if (page == PAGE_PROFILES) {
        // the avatar's flyout, as Edge's: this profile big, its settings, the other profiles (their window, opened or
        // brought to the front), a new one, incognito
        Profile cur = profile_current();
        MItem head; head.kind = 8; head.enabled = false; head.text = profile_shown_name(cur); head.avatar = true; head.prof = cur;
        v.push_back(head);
        v.push_back(item(T(L"Configuración del perfil"), G_GEAR, L"", [] { after_menu([] { settings_open(L"perfiles"); }); }));
        v.push_back(sep());
        bool any = false;
        for (auto &pr : profiles_list()) {
            std::wstring id = pr.id;
            if (id == g_profile_id) continue;
            if (!any) { v.push_back(note(T(L"Otros perfiles"))); any = true; }
            MItem m = item(profile_shown_name(pr), 0, profile_window(id) ? T(L"Abierto") : L"", [id] { after_menu([id] { profile_open(id); }); });
            m.avatar = true; m.prof = pr;
            v.push_back(m);
        }
        v.push_back(item(T(L"Añadir perfil"), G_ADD, L"", [] { after_menu([] {
            std::wstring id = profile_create(T(L"Perfil") + std::wstring(L" ") + std::to_wstring(profiles_list().size() + 1));
            if (!id.empty()) profile_open(id);
        }); }));
        v.push_back(item(T(L"Nueva ventana de incógnito"), G_INPRIVATE, T(L"Ctrl+Mayús+N"), [] { after_menu([] { app_open_incognito(); }); }));
    } else if (page == PAGE_INCOGNITO) {
        // the incognito badge's flyout, as Edge's InPrivate one: what it is, and a way out
        MItem head; head.kind = 8; head.enabled = false; head.text = T(L"Incógnito"); head.icon = G_INPRIVATE;
        head.hint = T(L"Nada se guarda al cerrar");
        v.push_back(head);
        v.push_back(sep());
        v.push_back(item(T(L"Cerrar ventana de incógnito"), G_X, L"", [] { after_menu([] { app_close_window(); }); }));
    } else if (page == PAGE_DOWNLOADS) {
        // Pluma's downloads (the engine's own flyout took the engine down on "Open file"), newest first
        v.push_back(note(T(L"Descargas")));
        auto rows = downloads_rows();
        if (rows.empty()) v.push_back(note(T(L"Aún no has descargado nada")));
        for (size_t k = 0; k < rows.size() && k < 8; k++) {
            const DownloadRow &d = rows[k];
            int id = d.id;
            MItem m; m.kind = 9; m.text = d.name; m.hint = d.status; m.progress = d.progress; m.dl_state = d.state;
            if (d.state == 1) {
                m.act = [id] { after_menu([id] { download_open(id); }); };
                m.b1 = G_FOLDER; m.act1 = [id] { after_menu([id] { download_show(id); }); };
            } else if (d.state == 0) {
                m.b1 = G_X; m.act1 = [id] { download_cancel(id); };
            }
            v.push_back(m);
        }
        v.push_back(sep());
        v.push_back(item(T(L"Abrir carpeta de descargas"), G_FOLDER, L"", [] { after_menu([] { downloads_open_folder(); }); }));
    } else if (page == PAGE_TRANSLATE) {
        // the translate icon (or the offer): as Edge's, with the page's language, the target and what to remember
        int id = g_menu_tab;
        Tab *t = tab_by_id(id);
        if (!t) return v;
        std::wstring lang = t->page_lang, tl = translate_target(), site = translate_site(t);
        std::wstring tl1 = tl.substr(0, tl.find(L'-'));
        v.push_back(note(lang.empty() ? std::wstring(T(L"Traducir esta página")) : fmt1(T(L"Página en %1"), translate_lang_name(lang, false))));
        if (t->tr_state == 1) v.push_back(note(fmt1(T(L"Traduciendo… %1 %"), std::to_wstring(translate_progress(t)))));
        if (t->tr_state) v.push_back(item(T(L"Mostrar original"), G_UNDO, L"", [id] { after_menu([id] { translate_original(tab_by_id(id)); }); }));
        else v.push_back(item(T(L"Traducir"), G_LANG, translate_lang_name(tl, true), [id] { after_menu([id] { translate_page(tab_by_id(id)); }); }));
        v.push_back(sub(T(L"Traducir a"), G_GLOBE, translate_lang_name(tl, true), PAGE_TR_TARGET));
        v.push_back(sep());
        if (!lang.empty() && lang != tl1) {
            std::wstring name = translate_lang_name(lang, false);
            MItem al; al.kind = 3; al.text = fmt1(T(L"Traducir siempre las páginas en %1"), name); al.on = translate_always(lang);
            al.act = [id, lang] {
                bool on = !translate_always(lang);
                translate_set_always(lang, on);
                Tab *t = tab_by_id(id);
                if (on && t && !t->tr_state) translate_page(t);
            };
            v.push_back(al);
            MItem nv; nv.kind = 3; nv.text = fmt1(T(L"No traducir nunca las páginas en %1"), name); nv.on = translate_never(lang);
            nv.act = [lang] { translate_set_never(lang, !translate_never(lang)); };
            v.push_back(nv);
        }
        if (!site.empty()) {
            MItem ns; ns.kind = 3; ns.text = T(L"No traducir nunca este sitio"); ns.on = translate_never_site(site);
            ns.act = [site] { translate_set_never_site(site, !translate_never_site(site)); };
            v.push_back(ns);
        }
        v.push_back(sep());
        if (!t->tr_state) v.push_back(item(T(L"Ahora no"), G_X, L"", [] { after_menu([] {}); }));
        v.push_back(item(T(L"Configuración de traducción"), G_GEAR, L"", [] { after_menu([] { settings_open(L"idioma"); }); }));
    } else if (page == PAGE_TR_TARGET) {
        MItem b = back(T(L"Traducir a"));
        b.act = [] { menu_go(PAGE_TRANSLATE); };
        v.push_back(b);
        std::wstring tl = translate_target();
        for (auto &c : translate_targets()) {
            MItem m; m.kind = 4; m.text = translate_lang_name(c, true); m.on = c == tl;
            m.act = [c] {
                bool changed = c != translate_target();
                translate_set_target(c);
                Tab *t = tab_by_id(g_menu_tab);
                if (changed && t && t->tr_state) translate_page(t);   // (into the new one now)
                menu_go(PAGE_TRANSLATE);
            };
            v.push_back(m);
        }
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
    bool top = g_menu_page == PAGE_MAIN || g_menu_page == PAGE_TAB || g_menu_page == PAGE_PROFILES || g_menu_page == PAGE_INCOGNITO || g_menu_page == PAGE_TRANSLATE ||
               g_menu_page == PAGE_DOWNLOADS;
    int parent = g_menu_page == PAGE_TR_TARGET ? PAGE_TRANSLATE : PAGE_MAIN;
    if (vk == VK_ESCAPE) { if (!top) menu_go(parent); else after_menu([] {}); return; }
    if (vk == VK_LEFT && !top) { menu_go(parent); return; }
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
// ---------------------------------------------------------------- the avatar's tooltip: the profile's name (as Edge's)
static std::wstring g_tip_text;
static void tip_draw(Panel &p, double now) {
    const Palette &c = pal();
    float pad = S(kPanelPad);
    D2D1_RECT_F card = R(pad, pad - S(3) * p.slide.at(now), p.w - pad, p.hh - pad);
    soft_shadow(p.rt.Get(), p.br.Get(), card, S(6), 0.6f);
    fill(p.rt.Get(), p.br.Get(), card, S(6), c.surface);
    stroke(p.rt.Get(), p.br.Get(), card, S(6), alpha(c.text, dark_theme() ? 0.12f : 0.09f), S(1));
    text(p.rt.Get(), p.br.Get(), g_f.tab.Get(), g_tip_text, R(card.left + S(10), card.top, card.right - S(8), card.bottom), c.text);
}
static void tip_close() { if (g_tip.h && !g_tip.closing) panel_close(g_tip); }
static void tip_show(RECT anchor, const std::wstring &t) {
    // under the button, centred on it; kept on its screen
    g_tip_text = t;
    float pad = S(kPanelPad);
    int w = (int)(std::min(S(320), text_w(g_f.tab.Get(), t) + S(20)) + pad * 2), h = (int)(S(28) + pad * 2);
    int x = (anchor.left + anchor.right) / 2 - w / 2, y = anchor.bottom + (int)S(6) - (int)pad;
    if (keep_on_screen()) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
        x = std::max((int)mi.rcWork.left - (int)pad, std::min(x, (int)mi.rcWork.right - w + (int)pad));
    }
    g_tip.draw = tip_draw;
    panel_open(g_tip, x, y, w, h, false);
}
// ---------------------------------------------------------------- page translation (translate.cpp): its popup and the bubble
static void menu_refresh() {
    // the open menu shows what changed (its height too)
    if (!g_menu.h || g_menu.closing) return;
    menu_rebuild();
    float h = S(12);
    for (auto &m : g_items) h += item_h(m);
    if ((int)(h + S(kPanelPad) * 2) != g_menu.hh) menu_place(false); else panel_render(g_menu);
}
void ui_translate_changed() {
    ui_invalidate();
    if (g_menu.h && !g_menu.closing && (g_menu_page == PAGE_TRANSLATE || g_menu_page == PAGE_MAIN)) menu_refresh();
}
static RECT bar_rect_on_screen(D2D1_RECT_F r) {
    POINT a = { (LONG)r.left, (LONG)r.top }, b = { (LONG)r.right, (LONG)r.bottom };
    ClientToScreen(g_bar, &a); ClientToScreen(g_bar, &b);
    return { a.x, a.y, b.x, b.y };
}
static void open_translate_menu(bool quiet) {
    Tab *t = active_tab();
    if (!t) return;
    open_menu_page(PAGE_TRANSLATE, bar_rect_on_screen(translate_shown() ? translate_rect() : menu_rect()), t->id, quiet);
}
void ui_open_translate_menu() { open_translate_menu(false); }
void ui_open_profile_menu(bool quiet) {
    // the avatar's flyout (in incognito, the badge's); quiet: tests, without the focus
    tip_close();
    open_menu_page(g_incognito ? PAGE_INCOGNITO : PAGE_PROFILES, bar_rect_on_screen(profile_rect()), 0, quiet);
}
void ui_open_downloads(bool quiet) {
    // the downloads panel under ⋯ (Ctrl+J, ⋯ > Descargas, Configuración); quiet: Pluma shows it when a download starts
    // or ends, as Edge its flyout, unless the person is busy elsewhere, and it never takes the focus
    // (open over another window: it comes to this one, the window of the tab the download came from)
    bool open = g_menu.h && !g_menu.closing && g_menu_page == PAGE_DOWNLOADS && GetWindow(g_menu.h, GW_OWNER) == g_win;
    if (quiet) {
        if (open) { if (g_menu_quiet) g_menu_quiet_since = now_ms(); menu_refresh(); return; }   // (news: its 12 s again)
        if (g_menu.h && !g_menu.closing && g_menu_page == PAGE_DOWNLOADS && g_menu_quiet) menu_close();
        if (ui_address_focused() || (g_menu.h && !g_menu.closing) || !IsWindowVisible(g_win) || IsIconic(g_win)) return;
        if (!g_test && GetForegroundWindow() != g_win) return;
    } else if (open && !g_menu_quiet) { menu_close(); return; }   // (Ctrl+J again: away, as Edge)
    open_menu_page(PAGE_DOWNLOADS, bar_rect_on_screen(menu_rect()), 0, quiet);
}
void ui_downloads_changed() {
    // (from an app-wide timer: the panel's own window is made current, or it would move to another one)
    if (!g_menu.h || g_menu.closing || g_menu_page != PAGE_DOWNLOADS) return;
    WinScope ws(win_of_hwnd(GetWindow(g_menu.h, GW_OWNER)));
    menu_refresh();
}
void ui_open_menu_quietly(bool translate) {
    // tests: a menu shown without taking the focus (a test instance never takes it from the user)
    if (translate) { open_translate_menu(true); return; }
    open_menu_page(PAGE_MAIN, bar_rect_on_screen(menu_rect()), 0, true);
}
bool ui_translate_offer(Tab *t) {
    // as Edge's: the popup under the icon, unless the person is busy elsewhere (then the lit icon is enough); it takes
    // the place of another quiet one (the last tab's offer, still there: the new tab is on screen before it goes)
    if (!t || t != active_tab() || !translate_shown() || ui_address_focused() || (g_menu.h && !g_menu.closing && !g_menu_quiet) ||
        !IsWindowVisible(g_win) || IsIconic(g_win)) return false;
    if (!g_test && GetForegroundWindow() != g_win) return false;
    open_translate_menu(true);
    return true;
}
static bool mouse_clicked() {   // a button down anywhere (test instances: never, the mouse is the user's)
    return !g_test && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000));
}
static bool escape_down() { return !g_test && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && GetForegroundWindow() == g_win; }
static void quiet_watch() {
    // the offer closes with a click elsewhere, Esc, another tab, or by itself after 12 s if the mouse never went to it
    if (!g_menu_quiet || !g_menu.h || g_menu.closing) return;
    POINT c; GetCursorPos(&c);
    RECT wr; GetWindowRect(g_menu.h, &wr);
    int pad = (int)S(kPanelPad);
    InflateRect(&wr, -pad, -pad);
    bool over = PtInRect(&wr, c) != FALSE;
    if (over) g_menu_quiet_touched = true;
    Tab *t = active_tab();
    if ((mouse_clicked() && !over) || escape_down() || !t || t->id != g_menu_quiet_tab || !IsWindowVisible(g_win) || IsIconic(g_win) ||
        (!g_menu_quiet_touched && now_ms() - g_menu_quiet_since > 12000))
        menu_close();
}

static std::wstring g_bub_title, g_bub_text;
static bool g_bub_loading;
static RECT g_bub_anchor;
static int g_bub_hot, g_bub_tab, g_bub_tr;   // (g_bub_tr: was the page translated when it opened)
static double g_bub_opened, g_bub_copied = -1e9;
static const float kBubW = 380, kBubHead = 38, kBubMaxBody = 320;
static ComPtr<IDWriteTextLayout> bubble_layout() {
    ComPtr<IDWriteTextLayout> l;
    if (!g_dw || !g_f.menu) return l;
    g_dw->CreateTextLayout(g_bub_text.c_str(), (UINT32)g_bub_text.size(), g_f.menu.Get(), S(kBubW - 30), 100000, &l);
    if (l) {
        l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DWRITE_TRIMMING none = { DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };
        l->SetTrimming(&none, NULL);
    }
    return l;
}
static D2D1_RECT_F bubble_card(const Panel &p) { float pad = S(kPanelPad); return R(pad, pad, p.w - pad, p.hh - pad); }
static D2D1_RECT_F bubble_btn(const Panel &p, int which) {   // 1 copy, 2 close
    D2D1_RECT_F c = bubble_card(p);
    float x = c.right - S(8) - S(30) * (which == 2 ? 1 : 2);
    return R(x, c.top + S(6), x + S(28), c.top + S(34));
}
static void bubble_draw(Panel &p, double now) {
    const Palette &c = pal();
    D2D1_RECT_F card = bubble_card(p);
    card.top -= S(6) * p.slide.at(now); card.bottom -= S(6) * p.slide.at(now);
    panel_card(p, card);
    float dy = card.top - bubble_card(p).top;
    glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), G_LANG, R(card.left + S(8), card.top + S(6), card.left + S(34), card.top + S(34)), c.accent);
    D2D1_RECT_F copy = bubble_btn(p, 1), close = bubble_btn(p, 2);
    copy.top += dy; copy.bottom += dy; close.top += dy; close.bottom += dy;
    text(p.rt.Get(), p.br.Get(), g_f.hint.Get(), g_bub_title, R(card.left + S(36), card.top + S(6), copy.left - S(4), card.top + S(34)), c.sub);
    bool copied = now - g_bub_copied < 1500;
    if (!g_bub_loading) {
        if (g_bub_hot == 1) fill(p.rt.Get(), p.br.Get(), copy, S(7), c.hover);
        glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), copied ? G_CHECK : G_COPY, copy, copied ? c.accent : c.sub);
    }
    if (g_bub_hot == 2) fill(p.rt.Get(), p.br.Get(), close, S(7), c.hover);
    glyph(p.rt.Get(), p.br.Get(), g_f.icon_sm.Get(), G_X, close, c.sub);
    auto l = bubble_layout();
    if (!l) return;
    D2D1_RECT_F body = R(card.left + S(14), card.top + S(kBubHead), card.right - S(14), card.bottom - S(12));
    p.rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
    p.br->SetColor(g_bub_loading ? c.sub : c.text);
    p.rt->DrawTextLayout(D2D1::Point2F(body.left, body.top), l.Get(), p.br.Get());
    p.rt->PopAxisAlignedClip();
}
static void bubble_place() {
    float pad = S(kPanelPad);
    auto l = bubble_layout();
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    float body = std::min(std::max(m.height, S(18)), S(kBubMaxBody));
    int w = (int)(S(kBubW) + pad * 2), hh = (int)(S(kBubHead) + body + S(14) + pad * 2);
    RECT a = g_bub_anchor;
    int x = a.left - (int)pad, y = a.bottom + (int)S(8) - (int)pad;
    if (keep_on_screen()) {
        // under the words, or over them when there is no room below; always on their screen
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromRect(&a, MONITOR_DEFAULTTONEAREST), &mi);
        if (y + hh - pad > mi.rcWork.bottom) y = a.top - (int)S(8) - hh + (int)pad;
        y = std::max((int)mi.rcWork.top - (int)pad, std::min(y, (int)mi.rcWork.bottom - hh + (int)pad));
        x = std::max((int)mi.rcWork.left - (int)pad, std::min(x, (int)mi.rcWork.right - w + (int)pad));
    }
    g_bubble.draw = bubble_draw;
    bool fresh = !g_bubble.h || g_bubble.closing;
    panel_open(g_bubble, x, y, w, hh, false);
    if (fresh && g_bubble.h) SetTimer(g_bubble.h, 5, 120, NULL);
}
static void bubble_close() {
    if (!g_bubble.h || g_bubble.closing) return;
    KillTimer(g_bubble.h, 5);
    panel_close(g_bubble);
}
void ui_bubble(RECT anchor, const std::wstring &title, const std::wstring &text, bool loading) {
    Tab *t = active_tab();
    g_bub_anchor = anchor; g_bub_title = title; g_bub_text = text; g_bub_loading = loading;
    g_bub_hot = 0; g_bub_tab = t ? t->id : 0; g_bub_tr = t ? t->tr_state : 0; g_bub_opened = now_ms(); g_bub_copied = -1e9;
    bubble_place();
}
void ui_bubble_text(const std::wstring &title, const std::wstring &text, bool loading) {
    if (!g_bubble.h || g_bubble.closing) return;
    g_bub_title = title; g_bub_text = text; g_bub_loading = loading;
    bubble_place();
}
static void bubble_watch() {
    // a click elsewhere, Esc, another tab, the whole page being translated or the window going: the bubble goes
    if (!g_bubble.h || g_bubble.closing) return;
    POINT c; GetCursorPos(&c);
    RECT wr; GetWindowRect(g_bubble.h, &wr);
    int pad = (int)S(kPanelPad);
    InflateRect(&wr, -pad, -pad);
    bool over = PtInRect(&wr, c) != FALSE;
    Tab *t = active_tab();
    if ((mouse_clicked() && !over && now_ms() - g_bub_opened > 400) || escape_down() || !t || t->id != g_bub_tab || (!g_bub_tr && t->tr_state) ||
        !IsWindowVisible(g_win) || IsIconic(g_win))
        bubble_close();
}
static void copy_to_clipboard(const std::wstring &s) {
    if (!OpenClipboard(g_win)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}
IStream *ui_glyph_png(wchar_t g) {
    // the page's context menu takes its icons as images: the glyph drawn once, in the menu's ink (it follows the theme)
    static std::unordered_map<int, std::string> cache;
    bool dark = dark_theme();
    int key = (int)g * 2 + (dark ? 1 : 0);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::string png;
        const UINT px = 32;
        ComPtr<IWICBitmap> bmp;
        ComPtr<ID2D1RenderTarget> rt;
        ComPtr<IDWriteTextFormat> f;
        ComPtr<ID2D1SolidColorBrush> br;
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (g_wic && g_d2d && g_dw && SUCCEEDED(g_wic->CreateBitmap(px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
            SUCCEEDED(g_d2d->CreateWicBitmapRenderTarget(bmp.Get(), rp, &rt)) &&
            SUCCEEDED(g_dw->CreateTextFormat(g_icon_family.c_str(), NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"", &f)) &&
            SUCCEEDED(rt->CreateSolidColorBrush(dark ? hex(0xe7eaee) : hex(0x1d2733), &br))) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            rt->DrawText(&g, 1, f.Get(), D2D1::RectF(0, 0, (float)px, (float)px), br.Get());
            if (SUCCEEDED(rt->EndDraw())) {
                IStream *s = SHCreateMemStream(NULL, 0);
                ComPtr<IWICBitmapEncoder> enc;
                ComPtr<IWICBitmapFrameEncode> fr;
                if (s && SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) && SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) && SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->SetSize(px, px)) &&
                    SUCCEEDED(fr->WriteSource(bmp.Get(), NULL)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) {
                    STATSTG st{};
                    LARGE_INTEGER zero{};
                    if (SUCCEEDED(s->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(s->Seek(zero, STREAM_SEEK_SET, NULL))) {
                        png.resize((size_t)st.cbSize.QuadPart);
                        ULONG got = 0;
                        if (FAILED(s->Read(&png[0], (ULONG)png.size(), &got)) || got != png.size()) png.clear();
                    }
                }
                if (s) s->Release();
            }
        }
        it = cache.emplace(key, png).first;
    }
    if (it->second.empty()) return nullptr;
    return SHCreateMemStream((const BYTE *)it->second.data(), (UINT)it->second.size());
}

void ui_blend_panels(BYTE *bits, int w, int h, POINT origin) {
    // tests: the open panels into a picture of the window, with their real transparency (as the screen shows them)
    for (Panel *p : { &g_menu, &g_sug, &g_toast, &g_bubble, &g_tip }) {
        if (!p->h || !p->bmp || !IsWindowVisible(p->h)) continue;
        DIBSECTION ds{};
        if (!GetObjectW(p->bmp, sizeof(ds), &ds) || !ds.dsBm.bmBits) continue;
        float k = std::max(0.0f, std::min(1.0f, p->fade.at(now_ms())));
        for (int y = 0; y < p->hh; y++)
            for (int x = 0; x < p->w; x++) {
                int dx = p->pos.x - origin.x + x, dy = p->pos.y - origin.y + y;
                if (dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
                BYTE *src = (BYTE *)ds.dsBm.bmBits + (y * p->w + x) * 4, *d = bits + (dy * w + dx) * 4;
                float a = src[3] / 255.0f * k;
                for (int c = 0; c < 3; c++) d[c] = (BYTE)std::min(255.0f, src[c] * k + d[c] * (1 - a));
            }
    }
}
void ui_frame() {
    // one animation frame: every window whose bar moves, and the panels fading or sliding
    g_frame_pending = false;
    double now = now_ms();
    bool busy = false;
    std::vector<Window *> ws;
    for (auto &w : g_windows) ws.push_back(w.get());
    for (Window *w : ws) {
        if (!win_alive(w) || !w->ui) continue;
        WinScope scope(w);
        if (g_bar_busy) { paint(); busy |= g_bar_busy; }
    }
    for (Panel *p : { &g_menu, &g_sug, &g_toast, &g_bubble, &g_tip })
        if (p->h && (p->fade.running(now) || p->slide.running(now) || p->closing)) { panel_render(*p); busy |= p->h != NULL; }
    if (!busy) g_anim_on = false;
}
static bool panel_here(const Panel &p) { return p.h && GetWindow(p.h, GW_OWNER) == g_win; }   // over the current window
void ui_close_popups() {
    // (only the current window's: the others' stay as they are)
    if (panel_here(g_sug)) close_suggestions();
    if (panel_here(g_menu) && !g_menu.closing) menu_close();
    if (panel_here(g_bubble) && !g_bubble.closing && now_ms() - g_bub_opened > 500) bubble_close();   // (not the page taking the focus back from its own menu)
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Window *owner = win_of_hwnd(h);   // (a panel's parent is the window it is over)
    if (!owner) return DefWindowProcW(h, msg, wp, lp);
    WinScope ws(owner);
    Panel *p = h == g_menu.h ? &g_menu : h == g_sug.h ? &g_sug : h == g_toast.h ? &g_toast : h == g_bubble.h ? &g_bubble : h == g_tip.h ? &g_tip : nullptr;
    switch (msg) {
    case WM_TIMER:
        if (!p) break;
        if (wp == 2) { KillTimer(h, 2); panel_close(*p); return 0; }
        if (wp == 5 && p == &g_bubble) { bubble_watch(); return 0; }
        if (wp == 6 && p == &g_menu) { quiet_watch(); return 0; }
        if (wp == 7) KillTimer(h, 7);   // (the bubble's "copied" tick, drawn once more)
        panel_render(*p);
        return 0;
    case WM_MOUSEACTIVATE: return p == &g_menu && !g_menu_quiet ? MA_ACTIVATE : MA_NOACTIVATE;
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
        } else if (p == &g_bubble) {
            int hot = inside(bubble_btn(g_bubble, 1), (float)x, (float)y) && !g_bub_loading ? 1 : inside(bubble_btn(g_bubble, 2), (float)x, (float)y) ? 2 : 0;
            if (hot != g_bub_hot) { g_bub_hot = hot; panel_render(g_bubble); }
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
        if (p == &g_bubble && g_bub_hot) { g_bub_hot = 0; panel_render(g_bubble); }
        return 0;
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (p == &g_menu) { int btn = 0, i = menu_hit(x, y, &btn); if (i >= 0) menu_activate(i, btn); else after_menu([] {}); }
        else if (p == &g_bubble) {
            if (g_bub_hot == 1) { copy_to_clipboard(g_bub_text); g_bub_copied = now_ms(); panel_render(g_bubble); SetTimer(h, 7, 1600, NULL); }
            else if (g_bub_hot == 2) bubble_close();
        }
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
    if (h.what == H_PROFILE) SetTimer(g_bar, 35, 500, NULL); else tip_close();
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
    if (!active) { close_suggestions(); tip_close(); }
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
// ---------------------------------------------------------------- a tab dragged between windows (as Chromium's TabDragController)
// Inside its strip a tab only moves along it. Taken out (more than 15 DIP above or below it, or past its ends) it goes
// to a window of its own that follows the mouse; over another window's strip it goes in there, at the slot under the
// mouse. The only tab of a window moves the window itself, and that window closes if the tab goes into another.
// Dropped over nothing, the new window stays where it is. All in screen pixels; the views move alive (tab_move).
struct TabDrag { bool on = false; int tab = 0, floating = 0; POINT off{}; UINT off_dpi = 0; float dx_dip = 0, dy_dip = 0; };
static TabDrag g_tdrag;
static POINT g_test_pointer;   // (tests: where the mouse would be)
static RECT strip_on_screen(Window *w) {
    WinScope ws(w);
    RECT rc; GetClientRect(g_bar, &rc);
    POINT a = { 0, (LONG)-S(15) }, b = { rc.right, (LONG)S(kTabRowH + 15) };   // (Chromium's vertical magnetism)
    ClientToScreen(g_bar, &a); ClientToScreen(g_bar, &b);
    return { a.x, a.y, b.x, b.y };
}
static Window *strip_under(POINT pt, Window *skip) {
    // our windows from the top down: the first whose strip has the point (one that covers the point without its
    // strip there hides what is under it)
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) {
        Window *w = nullptr;
        for (auto &o : g_windows) if (o->hwnd == h) w = o.get();
        if (!w || w == skip || !IsWindowVisible(h) || IsIconic(h) || w->fullscreen) continue;
        RECT r = strip_on_screen(w);
        if (PtInRect(&r, pt)) return w;
        RECT wr; GetWindowRect(h, &wr);
        if (PtInRect(&wr, pt)) return nullptr;
    }
    return nullptr;
}
static int slot_at(Window *w, POINT pt) {
    WinScope ws(w);
    ScreenToClient(g_bar, &pt);
    float c = pt.x - g_tdrag.dx_dip * g_s + g_geo.tab_w / 2;
    int at = (int)((c - g_geo.strip_l) / (g_geo.tab_w + S(kTabGap)));
    return std::max(0, std::min(at, (int)g_tabs.size()));
}
static POINT grab_offset(Window *f) {
    // where the mouse holds the window: on its tab, as far into it as the tab was held
    WinScope ws(f);
    compute_geo();
    float dx = std::max(S(8), std::min(g_tdrag.dx_dip * g_s, g_geo.tab_w - S(8)));
    POINT c = { (LONG)(g_geo.strip_l + dx), (LONG)(g_tdrag.dy_dip * g_s) };
    ClientToScreen(g_bar, &c);
    RECT wr; GetWindowRect(g_win, &wr);
    g_tdrag.off_dpi = GetDpiForWindow(g_win);
    return { c.x - wr.left, c.y - wr.top };
}
static void float_follow(Window *f, POINT pt) {
    if (GetDpiForWindow(f->hwnd) != g_tdrag.off_dpi) g_tdrag.off = grab_offset(f);   // (onto a screen of another scale)
    SetWindowPos(f->hwnd, NULL, pt.x - g_tdrag.off.x, pt.y - g_tdrag.off.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void capture_to(Window *w) { WinScope ws(w); if (GetCapture() != g_bar) SetCapture(g_bar); }
static void drag_in_strip(Window *w, POINT pt) {
    WinScope ws(w);
    g_press = { H_TAB, g_tdrag.tab };
    g_drag_id = g_tdrag.tab;
    g_drag_dx = g_tdrag.dx_dip * g_s;
    g_dragging = true;
    ScreenToClient(g_bar, &pt);
    drag_update((float)pt.x);
}
static void drag_out_of_strip(Window *w) { WinScope ws(w); g_dragging = false; g_press = {}; ui_invalidate(); }
static void bring_up(Window *w) {
    // the window the tab goes into comes to the front (in tests, under the rest, without the focus)
    if (g_test) SetWindowPos(w->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    else SetForegroundWindow(w->hwnd);
}
static void drag_begin(POINT pt) {
    (void)pt;
    g_tdrag = TabDrag();
    g_tdrag.on = true;
    g_tdrag.tab = g_drag_id;
    g_tdrag.dx_dip = g_drag_dx / g_s;
    g_tdrag.dy_dip = g_press_y / g_s;
    if (g_tabs.size() == 1) {   // the window's only tab: the window goes with it
        if (IsZoomed(g_win)) ShowWindow(g_win, SW_RESTORE);
        g_tdrag.floating = g_cw->id;
        g_cw->dragged = true;
        g_tdrag.off = grab_offset(g_cw);
    } else g_dragging = true;
}
static void drag_end() {
    if (!g_tdrag.on) return;
    TabDrag d = g_tdrag;
    g_tdrag = TabDrag();
    for (auto &w : g_windows) { WinScope ws(w.get()); g_dragging = false; g_press = {}; ui_invalidate(); }
    if (Window *f = win_by_id(d.floating)) { f->dragged = false; if (!g_test) SetForegroundWindow(f->hwnd); }
    HWND cap = GetCapture();
    if (cap && win_of_hwnd(cap)) ReleaseCapture();
    session_save_soon();
}
static void drag_mouse(POINT pt) {
    Tab *t = tab_by_id(g_tdrag.tab);
    Window *in = win_of_tab(t);
    if (!t || !in) { drag_end(); return; }
    Window *fl = g_tdrag.floating ? win_by_id(g_tdrag.floating) : nullptr;
    Window *target = strip_under(pt, fl);
    if (fl) {
        if (!target) { float_follow(fl, pt); return; }
        // over another window's strip: the tab goes in there, and its own window goes
        g_tdrag.floating = 0;
        fl->dragged = false;
        tab_move(t, target, slot_at(target, pt), true);
        capture_to(target);
        bring_up(target);
        drag_in_strip(target, pt);
        if (fl->tabs.empty()) {
            ShowWindow(fl->hwnd, SW_HIDE);
            int fid = fl->id;
            ui_post([fid] { Window *w = win_by_id(fid); if (w && w->tabs.empty()) win_close(w); });   // (not under its own messages)
        }
        log_line(L"pestaña arrastrada a la ventana " + std::to_wstring(target->id));
        return;
    }
    RECT strip = strip_on_screen(in);
    if (PtInRect(&strip, pt) && IsWindowVisible(in->hwnd)) { drag_in_strip(in, pt); return; }
    if (target && target != in) {   // straight from one strip into another
        drag_out_of_strip(in);
        tab_move(t, target, slot_at(target, pt), true);
        capture_to(target);
        bring_up(target);
        drag_in_strip(target, pt);
        return;
    }
    // out of every strip: a window of its own, under the mouse, as big as the one it left
    drag_out_of_strip(in);
    if (in->tabs.size() == 1) {
        g_tdrag.floating = in->id;
        in->dragged = true;
        g_tdrag.off = grab_offset(in);
        float_follow(in, pt);
        return;
    }
    RECT r;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (IsZoomed(in->hwnd) || in->fullscreen) { GetWindowPlacement(in->hwnd, &wp); r = in->fullscreen ? in->before_fs.rcNormalPosition : wp.rcNormalPosition; }
    else GetWindowRect(in->hwnd, &r);
    OffsetRect(&r, pt.x - r.left - (LONG)(g_tdrag.dx_dip * 2), pt.y - r.top - (LONG)(g_tdrag.dy_dip));
    Window *nw = win_new(&r);
    SetPropW(nw->hwnd, L"colocada", (HANDLE)1);   // (where the drag puts it, also in tests)
    nw->dragged = true;
    g_tdrag.floating = nw->id;
    tab_move(t, nw, 0, true);
    g_tdrag.off = grab_offset(nw);
    float_follow(nw, pt);
    win_show(nw, !g_test);
    { WinScope ws(nw); tab_activate(g_active); }
    capture_to(nw);
    log_line(L"pestaña sacada a una ventana nueva");
}
static POINT event_point(HWND h, LPARAM lp) {
    // where the mouse was, on the screen: from the message itself (a window following the mouse has moved since its
    // client coordinates were taken); tests send their own coordinates
    if (!g_test) { DWORD mp = GetMessagePos(); return { GET_X_LPARAM(mp), GET_Y_LPARAM(mp) }; }
    POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ClientToScreen(h, &p);
    return p;
}
void ui_test_drag(int step, int a, int b, int c) {
    // tests (pluma-prueba:arrastre...): 0 grab tab a of the current window by its middle, 1 move by (b, c) pixels,
    // 2 move to (b, c) of window a's bar, 3 drop
    if (step == 0) {
        if (a < 0 || a >= (int)g_tabs.size()) return;
        int id = g_tabs[a]->id;
        for (auto &s : tab_slots(now_ms(), true)) if (s.t->id == id) {
            g_press = { H_TAB, id };
            g_drag_id = id;
            g_drag_dx = s.w / 2;
            g_press_x = s.x + s.w / 2;
            g_press_y = S(kTabY + kTabH / 2);
        }
        g_test_pointer = { (LONG)g_press_x, (LONG)g_press_y };
        ClientToScreen(g_bar, &g_test_pointer);
        return;
    }
    if (step == 1) { g_test_pointer.x += b; g_test_pointer.y += c; }
    if (step == 2) {
        Window *w = win_by_id(a);
        if (!w) return;
        POINT p = { b, c };
        ClientToScreen(w->bar, &p);
        g_test_pointer = p;
    }
    if (step == 3) { drag_end(); return; }
    if (!g_tdrag.on) {
        if (g_press.what != H_TAB) return;
        drag_begin(g_test_pointer);
    }
    drag_mouse(g_test_pointer);
}
void ui_open_tab_menu_quietly() {
    // tests: the active tab's menu, under it, without the focus
    Tab *t = active_tab();
    if (!t) return;
    for (auto &s : tab_slots(now_ms(), true)) if (s.t == t) {
        POINT pt = { (LONG)(s.x + s.w / 2), (LONG)S(kTabY + kTabH) };
        ClientToScreen(g_bar, &pt);
        open_menu_page(PAGE_TAB, { pt.x, pt.y, pt.x, pt.y }, t->id, true);
    }
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
    case H_PROFILE: {
        if (g_menu_page_is_profiles() && !g_menu_quiet) { menu_close(); break; }
        ui_open_profile_menu(false);
        break;
    }
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
    case H_NTP_ROW: if (h.id < (int)g_ntp_rows.size()) { Tab *t = active_tab(); std::wstring u = g_ntp_rows[h.id].url; if (t) { tab_navigate(t, u); address_done(u); } else tab_new(u, true); } break;
    case H_NTP_X: if (h.id < (int)g_ntp_rows.size()) ntp_take_off((size_t)h.id); break;
    case H_STAR: ui_toggle_pin(); break;
    case H_READER: reader_toggle(active_tab()); break;
    case H_TRANSLATE: if (g_menu.h && !g_menu.closing && g_menu_page == PAGE_TRANSLATE && !g_menu_quiet) menu_close(); else ui_open_translate_menu(); break;
    case H_BM: {
        BmNode *n = bm_find(h.id);
        if (!n) break;
        if (n->folder) {
            for (auto &b : bm_bar_items()) if (b.id == h.id) {
                D2D1_RECT_F r = bm_btn_rect(b);
                POINT a = { (LONG)r.left, (LONG)r.top }, bb = { (LONG)r.right, (LONG)r.bottom };
                ClientToScreen(g_bar, &a); ClientToScreen(g_bar, &bb);
                open_menu_page(PAGE_BM_OPEN, { a.x, a.y, bb.x, bb.y }, h.id);
                break;
            }
        } else { std::wstring u = n->url; Tab *t = active_tab(); if (t && t->url.empty()) { tab_navigate(t, u); address_done(u); } else tab_new(u, true); }
        break;
    }
    default: break;
    }
}
static LRESULT CALLBACK bar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Window *self = win_of_hwnd(h);
    if (!self) return DefWindowProcW(h, msg, wp, lp);   // (before its window knows it, or once it is closing)
    WinScope ws(self);
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
        if (g_tdrag.on) {
            if (wp & MK_LBUTTON) drag_mouse(event_point(h, lp)); else drag_end();
            return 0;
        }
        if (g_press.what == H_TAB && (wp & MK_LBUTTON) && hypotf(x - g_press_x, y - g_press_y) > S(5)) {
            POINT p = event_point(h, lp);
            drag_begin(p);
            drag_mouse(p);
            return 0;
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
        if (hi.what != H_MENU && hi.what != H_PROFILE) ui_close_popups();
        tip_close();
        g_press = hi;
        g_press_x = x;
        g_press_y = y;
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
        if (g_tdrag.on) { drag_end(); return 0; }
        HitInfo hi = hit_test(x, y);
        HitInfo pressed = g_press;
        g_press = {};
        g_dragging = false;
        if (GetCapture() == h) ReleaseCapture();
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
        if (hi.what == H_STAR) {
            // right click on the star: choose which folder the page goes in
            D2D1_RECT_F s = star_rect();
            POINT a = { (LONG)s.left, (LONG)s.top }, b = { (LONG)s.right, (LONG)s.bottom };
            ClientToScreen(h, &a); ClientToScreen(h, &b);
            open_menu_page(PAGE_STAR, { a.x, a.y, b.x, b.y });
        } else if (hi.what == H_TAB || hi.what == H_TAB_CLOSE || hi.what == H_EXT) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &pt);
            open_menu_page(hi.what == H_EXT ? PAGE_ONE_EXT : PAGE_TAB, { pt.x, pt.y, pt.x, pt.y }, hi.id);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_tdrag.on) {
            Window *o = win_of_hwnd((HWND)lp);
            if (o && (HWND)lp == o->bar) return 0;   // (the drag went on to another of our windows' bars)
            drag_end();
            return 0;
        }
        if (g_dragging) { g_dragging = false; ui_invalidate(); }
        return 0;
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
    case WM_TIMER:
        if (wp == 34) {   // the mouse rests on a tab that was let go: it starts loading before the click
            KillTimer(h, 34);
            if (g_hover.what == H_TAB) tab_warm(tab_by_id(g_hover.id));
        }
        if (wp == 35) {   // the mouse rests on the avatar: the profile's name
            KillTimer(h, 35);
            if (g_hover.what == H_PROFILE && !g_incognito && !g_menu_page_is_profiles())
                tip_show(bar_rect_on_screen(profile_rect()), profile_shown_name(profile_current()));
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
        if (vk == 'N' && shift) { app_open_incognito(); return true; }   // the incognito window, as in Chrome
        if (vk == 'N') { win_open_new(); return true; }   // a new window, as in every browser
        if (vk == 'T') { tab_new(L"", true); return true; }
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
        if (vk == 'O' && shift) { bookmarks_open(); return true; }
        if (vk == 'B' && shift) { g_set.bookmarks_bar = !g_set.bookmarks_bar; settings_save(); ui_layout(); return true; }
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
        if (vk == VK_F9) { reader_toggle(active_tab()); return true; }
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
    load_fonts();
    g_theme.snap(dark_theme() ? 1.0f : 0.0f);

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
    start_animation_thread();
}
void ui_window_init() {
    // the current window's own: its bar, its address field, its scale and the text sizes for it
    g_s = GetDpiForWindow(g_win) / 96.0f;
    make_formats();
    BOOL dark = dark_theme();
    DwmSetWindowAttribute(g_win, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    RECT rc; GetClientRect(g_win, &rc);
    g_bar = CreateWindowExW(0, L"PlumaBarra", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, rc.right, rc.bottom, g_win, NULL, g_inst, NULL);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 100, 20, g_bar, NULL, g_inst, NULL);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_edit_font, FALSE);
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 8000, 0);
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    if (!g_edit_proc) g_edit_proc = old;   // (the same EDIT procedure for every window)
    compute_geo();
    if (g_windows.size() > 1) ui_taskbar_badge();
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
    for (auto &w : g_windows) {
        WinScope ws(w.get());
        DwmSetWindowAttribute(g_win, 20, &dark, sizeof(dark));
        InvalidateRect(g_win, NULL, FALSE);
        if (g_edit) InvalidateRect(g_edit, NULL, TRUE);
        ui_invalidate();
    }
}
void ui_font_changed() {
    load_fonts();
    for (auto &w : g_windows) {
        WinScope ws(w.get());
        make_formats();
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
        compute_geo();
        ui_invalidate();
    }
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

// ---------------------------------------------------------------- the taskbar: which profile a window is (as Edge's badge)
void ui_taskbar_badge() {
    static ComPtr<ITaskbarList3> tb;
    if (!tb) {
        if (FAILED(CoCreateInstance(__uuidof(TaskbarList), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tb)))) return;
        tb->HrInit();
    }
    static HICON badge;
    if (g_incognito || profiles_list().size() < 2) { for (auto &w : g_windows) tb->SetOverlayIcon(w->hwnd, NULL, NULL); return; }
    // its avatar, drawn once into an icon
    const int n = 32;
    Profile p = profile_current();
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (!g_wic || FAILED(g_wic->CreateBitmap(n, n, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) ||
        FAILED(g_d2d->CreateWicBitmapRenderTarget(bmp.Get(), rp, &rt))) return;
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(hex(0), &br);
    auto f = make_format(false, 18, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    br->SetColor(hex(0xffffff));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(n / 2.f, n / 2.f), n / 2.f, n / 2.f), br.Get());   // (a thin ring, over any icon)
    br->SetColor(rgb_of(p.color));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(n / 2.f, n / 2.f), n / 2.f - 1.5f, n / 2.f - 1.5f), br.Get());
    text(rt.Get(), br.Get(), f.Get(), initial_of(profile_shown_name(p)), R(0, 0, (float)n, (float)n), hex(0xffffff));
    rt->EndDraw();
    std::vector<BYTE> px(n * n * 4);
    if (FAILED(bmp->CopyPixels(NULL, n * 4, (UINT)px.size(), px.data()))) return;
    for (size_t i = 0; i < px.size(); i += 4) {   // icons take straight alpha
        BYTE a = px[i + 3];
        if (a && a < 255) for (int k = 0; k < 3; k++) px[i + k] = (BYTE)std::min(255, px[i + k] * 255 / a);
    }
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh); bh.bV5Width = n; bh.bV5Height = -n; bh.bV5Planes = 1; bh.bV5BitCount = 32; bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00ff0000; bh.bV5GreenMask = 0x0000ff00; bh.bV5BlueMask = 0x000000ff; bh.bV5AlphaMask = 0xff000000;
    void *bits = nullptr;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color) return;
    memcpy(bits, px.data(), px.size());
    HBITMAP mask = CreateBitmap(n, n, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    if (!icon) return;
    std::wstring name = profile_shown_name(p);
    for (auto &w : g_windows) tb->SetOverlayIcon(w->hwnd, icon, name.c_str());
    if (badge) DestroyIcon(badge);
    badge = icon;
}

// ---------------------------------------------------------------- the state of each window's bar
// (one per window, made at its first use; the names above are these fields of the current window, g_cw)
#undef g_s
#undef g_rt
#undef g_br
#undef g_f
#undef g_edit_font
#undef g_ghosts
#undef g_icons
#undef g_host_icons
#undef g_ext_icons
#undef g_sheet_k
#undef g_notice_h
#undef g_addr_focus
#undef g_menu_btn_open
#undef g_sheet_from
#undef g_sheet_to
#undef g_frozen_w
#undef g_hover
#undef g_press
#undef g_btn_hover
#undef g_caption_hot
#undef g_caption_down
#undef g_tracking
#undef g_dragging
#undef g_active_window
#undef g_drag_id
#undef g_drag_dx
#undef g_drag_x
#undef g_press_x
#undef g_press_y
#undef g_notice
#undef g_addr_text
#undef g_ntp_rows
#undef g_ntp_pins
#undef g_ntp_filled
#undef g_typed
#undef g_geo
#undef g_strip_end
#undef g_bar_busy
struct UiWin {
    float g_s = 1;
    ComPtr<ID2D1HwndRenderTarget> g_rt{};
    ComPtr<ID2D1SolidColorBrush> g_br{};
    Formats g_f{};
    HFONT g_edit_font = NULL;
    std::vector<Ghost> g_ghosts{};
    std::unordered_map<int, ComPtr<ID2D1Bitmap>> g_icons{};
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> g_host_icons{};
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> g_ext_icons{};
    Anim g_sheet_k{};
    Anim g_notice_h{};
    Anim g_addr_focus{};
    Anim g_menu_btn_open{};
    int g_sheet_from = 0;
    int g_sheet_to = 0;
    float g_frozen_w = 0;
    HitInfo g_hover{};
    HitInfo g_press{};
    std::unordered_map<int, Anim> g_btn_hover{};
    int g_caption_hot = 0;
    int g_caption_down = 0;
    bool g_tracking = false;
    bool g_dragging = false;
    bool g_active_window = true;
    int g_drag_id = 0;
    float g_drag_dx = 0;
    float g_drag_x = 0;
    float g_press_x = 0;
    float g_press_y = 0;
    Notice g_notice{};
    std::wstring g_addr_text{};
    std::vector<HistoryEntry> g_ntp_rows{};
    bool g_ntp_pins = false;
    bool g_ntp_filled = false;
    std::wstring g_typed{};
    Geo g_geo{};
    float g_strip_end = 0;
    bool g_bar_busy = false;
};
static UiWin *UI() { if (!g_cw->ui) g_cw->ui = new UiWin(); return (UiWin *)g_cw->ui; }
static float &ui_g_s() { return UI()->g_s; }
static ComPtr<ID2D1HwndRenderTarget> &ui_g_rt() { return UI()->g_rt; }
static ComPtr<ID2D1SolidColorBrush> &ui_g_br() { return UI()->g_br; }
static Formats &ui_g_f() { return UI()->g_f; }
static HFONT &ui_g_edit_font() { return UI()->g_edit_font; }
static std::vector<Ghost> &ui_g_ghosts() { return UI()->g_ghosts; }
static std::unordered_map<int, ComPtr<ID2D1Bitmap>> &ui_g_icons() { return UI()->g_icons; }
static std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> &ui_g_host_icons() { return UI()->g_host_icons; }
static std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> &ui_g_ext_icons() { return UI()->g_ext_icons; }
static Anim &ui_g_sheet_k() { return UI()->g_sheet_k; }
static Anim &ui_g_notice_h() { return UI()->g_notice_h; }
static Anim &ui_g_addr_focus() { return UI()->g_addr_focus; }
static Anim &ui_g_menu_btn_open() { return UI()->g_menu_btn_open; }
static int &ui_g_sheet_from() { return UI()->g_sheet_from; }
static int &ui_g_sheet_to() { return UI()->g_sheet_to; }
static float &ui_g_frozen_w() { return UI()->g_frozen_w; }
static HitInfo &ui_g_hover() { return UI()->g_hover; }
static HitInfo &ui_g_press() { return UI()->g_press; }
static std::unordered_map<int, Anim> &ui_g_btn_hover() { return UI()->g_btn_hover; }
static int &ui_g_caption_hot() { return UI()->g_caption_hot; }
static int &ui_g_caption_down() { return UI()->g_caption_down; }
static bool &ui_g_tracking() { return UI()->g_tracking; }
static bool &ui_g_dragging() { return UI()->g_dragging; }
static bool &ui_g_active_window() { return UI()->g_active_window; }
static int &ui_g_drag_id() { return UI()->g_drag_id; }
static float &ui_g_drag_dx() { return UI()->g_drag_dx; }
static float &ui_g_drag_x() { return UI()->g_drag_x; }
static float &ui_g_press_x() { return UI()->g_press_x; }
static float &ui_g_press_y() { return UI()->g_press_y; }
static Notice &ui_g_notice() { return UI()->g_notice; }
static std::wstring &ui_g_addr_text() { return UI()->g_addr_text; }
static std::vector<HistoryEntry> &ui_g_ntp_rows() { return UI()->g_ntp_rows; }
static bool &ui_g_ntp_pins() { return UI()->g_ntp_pins; }
static bool &ui_g_ntp_filled() { return UI()->g_ntp_filled; }
static std::wstring &ui_g_typed() { return UI()->g_typed; }
static Geo &ui_g_geo() { return UI()->g_geo; }
static float &ui_g_strip_end() { return UI()->g_strip_end; }
static bool &ui_g_bar_busy() { return UI()->g_bar_busy; }
void ui_window_free() {
    // the window goes (one of several): its panels, its drawing and what it measured
    for (Panel *p : { &g_menu, &g_sug, &g_toast, &g_bubble, &g_tip })
        if (p->h && GetWindow(p->h, GW_OWNER) == g_win) panel_free(*p);
    UiWin *u = (UiWin *)g_cw->ui;
    if (!u) return;
    if (u->g_edit_font) DeleteObject(u->g_edit_font);
    delete u;
    g_cw->ui = nullptr;
}
