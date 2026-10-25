// Pluma: YouTube videos in Pluma's own player, so the heavy YouTube page can sleep while the video plays: about 75 MB
// for the video instead of 300-380 for the page (measured 25/10). The engine is libmpv (the same as Zas's), in Pluma's
// own motor-video folder: Pluma depends on no other app.
// The page opens and starts playing as always. From inside that page (its cookies, its visitor id) Pluma asks YouTube
// for the streams of its visionOS client, which come as plain downloads with no protocol of their own and no
// JavaScript challenge (0.2 s, against 7-12 s for yt-dlp). mpv loads them a second ahead, paused; when the page
// reaches that second it pauses and mpv goes on, so there is no cut. The page then freezes and gives its memory back.
// The controls are YouTube's own as of 2026 (its "delhi" player: icons, sizes and colours read from the page itself).
// The "page" button goes back to the page at the same second.
#include "pluma.h"
#include "json.h"
#include <cstdint>
#include <map>
#include <thread>
#include <dwrite_3.h>
#include <winhttp.h>

// ---------------------------------------------------------------- libmpv, loaded at run time
typedef struct mpv_handle mpv_handle;
enum { MPV_FORMAT_NONE = 0, MPV_FORMAT_STRING = 1, MPV_FORMAT_FLAG = 3, MPV_FORMAT_INT64 = 4, MPV_FORMAT_DOUBLE = 5 };
enum { MPV_EVENT_NONE = 0, MPV_EVENT_SHUTDOWN = 1, MPV_EVENT_END_FILE = 7, MPV_EVENT_FILE_LOADED = 8, MPV_EVENT_PLAYBACK_RESTART = 21,
       MPV_EVENT_PROPERTY_CHANGE = 22 };
enum { MPV_END_FILE_REASON_ERROR = 4 };
struct mpv_event { int event_id; int error; uint64_t reply_userdata; void *data; };
struct mpv_event_property { const char *name; int format; void *data; };
struct mpv_event_end_file { int reason; int error; };
static struct {
    mpv_handle *(*create)();
    int (*initialize)(mpv_handle *);
    void (*terminate_destroy)(mpv_handle *);
    int (*set_option_string)(mpv_handle *, const char *, const char *);
    int (*command)(mpv_handle *, const char **);
    int (*command_async)(mpv_handle *, uint64_t, const char **);
    int (*set_property)(mpv_handle *, const char *, int, void *);
    int (*observe_property)(mpv_handle *, uint64_t, const char *, int);
    mpv_event *(*wait_event)(mpv_handle *, double);
    void (*set_wakeup_callback)(mpv_handle *, void (*)(void *), void *);
} M;
static HMODULE g_mpv;
static bool g_mpv_failed;
// Pluma's own copy, next to the exe (it comes in the package; tools\motor-video.ps1 fetches it). Loaded with the first
// video only: until then it costs nothing.
static std::wstring engine_path() { return g_root + L"\\motor-video\\libmpv-2.dll"; }
bool video_available() { return PathFileExistsW(engine_path().c_str()) != FALSE; }
static bool load_mpv() {
    if (g_mpv) return true;
    if (g_mpv_failed) return false;
    std::wstring path = engine_path();
    if (!PathFileExistsW(path.c_str())) { g_mpv_failed = true; log_line(L"vídeo propio: falta motor-video\\libmpv-2.dll"); return false; }
    g_mpv = LoadLibraryExW(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_mpv) { g_mpv_failed = true; log_line(L"vídeo propio: no se pudo cargar el motor"); return false; }
#define LOAD(field, name) *(FARPROC *)&M.field = GetProcAddress(g_mpv, name)
    LOAD(create, "mpv_create"); LOAD(initialize, "mpv_initialize"); LOAD(terminate_destroy, "mpv_terminate_destroy");
    LOAD(set_option_string, "mpv_set_option_string"); LOAD(command, "mpv_command"); LOAD(command_async, "mpv_command_async");
    LOAD(set_property, "mpv_set_property");
    LOAD(observe_property, "mpv_observe_property"); LOAD(wait_event, "mpv_wait_event"); LOAD(set_wakeup_callback, "mpv_set_wakeup_callback");
#undef LOAD
    if (!M.create || !M.initialize || !M.command || !M.command_async || !M.wait_event || !M.set_wakeup_callback) {
        g_mpv_failed = true; FreeLibrary(g_mpv); g_mpv = NULL; return false;
    }
    return true;
}

// ---------------------------------------------------------------- one video
// one entry of YouTube's HLS list: a video variant (its own playlist) or an audio rendition, by group
struct Fmt { std::string url, group, local; int h = 0, fps = 0; long long br = 0; bool audio = false, main_audio = true; std::wstring codec; };
struct Cap { std::string url, local; std::wstring name, lang; };
enum Stage { ST_WAITING, ST_ON };
struct Video {
    int tab = 0;
    std::wstring id, title, author;
    std::vector<Fmt> fmts;
    std::vector<Cap> caps;
    int vf = -1, af = -1, sub = -1;
    bool auto_quality = true;
    mpv_handle *mpv = nullptr;
    HWND wnd = NULL;
    Stage stage = ST_WAITING;
    double pos = 0, dur = 0, cached = 0, start_at = 0, volume = 100, speed = 1, gain_db = 0;
    bool paused = true, eof = false, buffering = false, muted = false, restarted = false, loaded = false;
    double asked = 0, last_move = 0, bezel_at = -1e9, vol_text_at = -1e9;
    int bezel_icon = 0, serial = 0;
};
static int g_serial;
static Video *V(Tab *t) { return t ? (Video *)t->video : nullptr; }
static HWND g_msg;   // a window of its own for mpv's events (they come from mpv's threads) and the timers
static const UINT WM_VIDEO_EVENTS = WM_APP + 60, WM_VIDEO_FAILED = WM_APP + 61, WM_VIDEO_GONE = WM_APP + 62;
static const UINT_PTR kAnimTimer = 7, kCtlTimer = 8, kWatchTimer = 0x100000, kSyncTimer = 0x200000, kFreezeTimer = 0x300000;

// ---------------------------------------------------------------- drawing: YouTube's look
static ComPtr<ID2D1Factory> g_d2d;
static ComPtr<IDWriteFactory> g_dw;
static ComPtr<IDWriteFontCollection> g_roboto;
static std::wstring g_family = L"Roboto";
static float S(float v) { return v * ui_scale(); }
static D2D1_COLOR_F rgba(float r, float g, float b, float a) { return { r, g, b, a }; }
static const D2D1_COLOR_F kWhite = { 1, 1, 1, 1 }, kText = { 238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 1 }, kRed = { 1, 0, 51 / 255.0f, 1 },
                          kPill = { 0, 0, 0, 0.3f };

// SVG paths (YouTube's icons come from the page as SVG): M L H V C A Z, absolute and relative
static ComPtr<ID2D1PathGeometry> svg(const char *d) {
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> sk;
    if (FAILED(g_d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&sk))) return nullptr;
    sk->SetFillMode(D2D1_FILL_MODE_WINDING);
    float x = 0, y = 0, sx = 0, sy = 0;
    bool open = false;
    char cmd = 0;
    const char *p = d;
    auto skip = [&] { while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++; };
    auto num = [&]() -> float { skip(); char *e = NULL; float v = strtof(p, &e); if (e == p) { p++; return 0; } p = e; return v; };
    auto flag = [&]() -> bool { skip(); bool f = *p == '1'; if (*p) p++; return f; };
    auto begin = [&] { if (!open) { sk->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED); open = true; sx = x; sy = y; } };
    while (*p) {
        skip();
        if (!*p) break;
        if (isalpha((unsigned char)*p)) cmd = *p++;
        bool rel = islower((unsigned char)cmd) != 0;
        switch (tolower(cmd)) {
        case 'm': {
            float nx = num(), ny = num();
            if (rel) { nx += x; ny += y; }
            if (open) sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false; x = nx; y = ny;
            begin();
            cmd = rel ? 'l' : 'L';   // more pairs after a move are lines
            break;
        }
        case 'l': { float nx = num(), ny = num(); if (rel) { nx += x; ny += y; } begin(); sk->AddLine(D2D1::Point2F(nx, ny)); x = nx; y = ny; break; }
        case 'h': { float nx = num(); if (rel) nx += x; begin(); sk->AddLine(D2D1::Point2F(nx, y)); x = nx; break; }
        case 'v': { float ny = num(); if (rel) ny += y; begin(); sk->AddLine(D2D1::Point2F(x, ny)); y = ny; break; }
        case 'c': {
            float x1 = num(), y1 = num(), x2 = num(), y2 = num(), nx = num(), ny = num();
            if (rel) { x1 += x; y1 += y; x2 += x; y2 += y; nx += x; ny += y; }
            begin();
            sk->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(nx, ny)));
            x = nx; y = ny;
            break;
        }
        case 'a': {
            float rx = num(), ry = num(), rot = num();
            bool large = flag(), sweep = flag();
            float nx = num(), ny = num();
            if (rel) { nx += x; ny += y; }
            begin();
            sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(nx, ny), D2D1::SizeF(rx, ry), rot,
                                        sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                        large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            x = nx; y = ny;
            break;
        }
        case 'z': if (open) { sk->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; } x = sx; y = sy; break;
        default: p++; break;
        }
    }
    if (open) sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    sk->Close();
    return g;
}
enum Icon { I_PLAY, I_PAUSE, I_REPLAY, I_SPEAKER, I_WAVE1, I_WAVE2, I_MUTED, I_CC_ON, I_CC_OFF, I_GEAR, I_PAGE, I_FULL, I_FULL_EXIT,
            I_SPEED, I_QUALITY, I_CHEV_R, I_CHEV_L, I_CHECK, I_COUNT };
struct IconDef { float vb; const char *d; };
static const IconDef kIcons[I_COUNT] = {
    // play and pause: YouTube's, on its 36 grid
    { 36, "M 17 8.6 L 10.89 4.99 C 9.39 4.11 7.5 5.19 7.5 6.93 L 7.5 29.06 C 7.5 30.8 9.39 31.88 10.89 31 L 17 27.4 L 17 8.6 Z "
          "M 17 8.6 V 27.4 L 33 18 V 18 L 17 8.6 Z" },
    { 36, "M 12.75 4.5 L 9.75 4.5 C 9.15 4.5 8.58 4.73 8.15 5.15 C 7.73 5.58 7.5 6.15 7.5 6.75 L 7.5 29.25 C 7.5 29.84 7.73 30.41 8.15 30.84 "
          "C 8.58 31.26 9.15 31.5 9.75 31.5 L 12.75 31.5 C 13.34 31.5 13.91 31.26 14.34 30.84 C 14.76 30.41 15 29.84 15 29.25 L 15 6.75 "
          "C 15 6.15 14.76 5.58 14.34 5.15 C 13.91 4.73 13.34 4.5 12.75 4.5 Z M 26.25 4.5 L 23.25 4.5 C 22.65 4.5 22.08 4.73 21.65 5.15 "
          "C 21.23 5.58 21 6.15 21 6.75 V 29.25 C 21 29.84 21.23 30.41 21.65 30.84 C 22.08 31.26 22.65 31.5 23.25 31.5 L 26.25 31.5 "
          "C 26.84 31.5 27.41 31.26 27.84 30.84 C 28.26 30.41 28.5 29.84 28.5 29.25 V 6.75 C 28.5 6.15 28.26 5.58 27.84 5.15 "
          "C 27.41 4.73 26.84 4.5 26.25 4.5 Z" },
    // replay (the video ended)
    { 24, "M12 5V1L7 6l5 5V7a6 6 0 1 1-6 6H4a8 8 0 1 0 8-8z" },
    // volume: the speaker, its two waves (one below half), muted
    { 24, "M 11.60 2.08 L 11.48 2.14 L 3.91 6.68 C 3.02 7.21 2.28 7.97 1.77 8.87 C 1.26 9.77 1.00 10.79 1 11.83 V 12.16 L 1.01 12.56 "
          "C 1.07 13.52 1.37 14.46 1.87 15.29 C 2.38 16.12 3.08 16.81 3.91 17.31 L 11.48 21.85 C 11.63 21.94 11.80 21.99 11.98 21.99 "
          "C 12.16 22.00 12.33 21.95 12.49 21.87 C 12.64 21.78 12.77 21.65 12.86 21.50 C 12.95 21.35 13 21.17 13 21 V 3 "
          "C 12.99 2.83 12.95 2.67 12.87 2.52 C 12.80 2.37 12.68 2.25 12.54 2.16 C 12.41 2.07 12.25 2.01 12.08 2.00 "
          "C 11.92 1.98 11.75 2.01 11.60 2.08 Z" },
    { 24, "M 15.53 7.05 C 15.35 7.22 15.25 7.45 15.24 7.70 C 15.23 7.95 15.31 8.19 15.46 8.38 L 15.53 8.46 L 15.70 8.64 "
          "C 16.09 9.06 16.39 9.55 16.61 10.08 L 16.70 10.31 C 16.90 10.85 17 11.42 17 12 L 16.99 12.24 C 16.96 12.73 16.87 13.22 16.70 13.68 "
          "L 16.61 13.91 C 16.36 14.51 15.99 15.07 15.53 15.53 C 15.35 15.72 15.25 15.97 15.26 16.23 C 15.26 16.49 15.37 16.74 15.55 16.92 "
          "C 15.73 17.11 15.98 17.21 16.24 17.22 C 16.50 17.22 16.76 17.12 16.95 16.95 C 17.6 16.29 18.11 15.52 18.46 14.67 L 18.59 14.35 "
          "C 18.82 13.71 18.95 13.03 18.99 12.34 L 19 12 C 18.99 11.19 18.86 10.39 18.59 9.64 L 18.46 9.32 C 18.15 8.57 17.72 7.89 17.18 7.3 "
          "L 16.95 7.05 L 16.87 6.98 C 16.68 6.82 16.43 6.74 16.19 6.75 C 15.94 6.77 15.71 6.87 15.53 7.05 Z" },
    { 24, "M18.36 4.22C18.18 4.39 18.08 4.62 18.07 4.87C18.05 5.12 18.13 5.36 18.29 5.56L18.36 5.63L18.66 5.95C19.36 6.72 19.91 7.60 20.31 8.55"
          "L20.47 8.96C20.82 9.94 21 10.96 21 11.99L20.98 12.44C20.94 13.32 20.77 14.19 20.47 15.03L20.31 15.44C19.86 16.53 19.19 17.52 18.36 18.36"
          "C18.17 18.55 18.07 18.80 18.07 19.07C18.07 19.33 18.17 19.59 18.36 19.77C18.55 19.96 18.80 20.07 19.07 20.07C19.33 20.07 19.59 19.96 19.77 19.77"
          "C20.79 18.75 21.61 17.54 22.16 16.20L22.35 15.70C22.72 14.68 22.93 13.62 22.98 12.54L23 12C22.99 10.73 22.78 9.48 22.35 8.29L22.16 7.79"
          "C21.67 6.62 20.99 5.54 20.15 4.61L19.77 4.22L19.70 4.15C19.51 3.99 19.26 3.91 19.02 3.93C18.77 3.94 18.53 4.04 18.36 4.22 Z" },
    { 24, "M11.60 2.08L11.48 2.14L3.91 6.68C3.02 7.21 2.28 7.97 1.77 8.87C1.26 9.77 1.00 10.79 1 11.83V12.16L1.01 12.56C1.07 13.52 1.37 14.46 1.87 15.29"
          "C2.38 16.12 3.08 16.81 3.91 17.31L11.48 21.85C11.63 21.94 11.80 21.99 11.98 21.99C12.16 22.00 12.33 21.95 12.49 21.87C12.64 21.78 12.77 21.65 12.86 21.50"
          "C12.95 21.35 13 21.17 13 21V3C12.99 2.83 12.95 2.67 12.87 2.52C12.80 2.37 12.68 2.25 12.54 2.16C12.41 2.07 12.25 2.01 12.08 2.00"
          "C11.92 1.98 11.75 2.01 11.60 2.08ZM4.94 8.4V8.40L11 4.76V19.23L4.94 15.6C4.38 15.26 3.92 14.80 3.58 14.25C3.24 13.70 3.05 13.07 3.00 12.43"
          "L3 12.17V11.83C2.99 11.14 3.17 10.46 3.51 9.86C3.85 9.25 4.34 8.75 4.94 8.4ZM21.29 8.29L19 10.58L16.70 8.29L16.63 8.22C16.43 8.07 16.19 7.99 15.95 8.00"
          "C15.70 8.01 15.47 8.12 15.29 8.29C15.12 8.47 15.01 8.70 15.00 8.95C14.99 9.19 15.07 9.43 15.22 9.63L15.29 9.70L17.58 12L15.29 14.29"
          "C15.19 14.38 15.12 14.49 15.06 14.61C15.01 14.73 14.98 14.87 14.98 15.00C14.98 15.13 15.01 15.26 15.06 15.39C15.11 15.51 15.18 15.62 15.28 15.71"
          "C15.37 15.81 15.48 15.88 15.60 15.93C15.73 15.98 15.86 16.01 15.99 16.01C16.12 16.01 16.26 15.98 16.38 15.93C16.50 15.87 16.61 15.80 16.70 15.70"
          "L19 13.41L21.29 15.70L21.36 15.77C21.56 15.93 21.80 16.01 22.05 15.99C22.29 15.98 22.53 15.88 22.70 15.70C22.88 15.53 22.98 15.29 22.99 15.05"
          "C23.00 14.80 22.93 14.56 22.77 14.36L22.70 14.29L20.41 12L22.70 9.70C22.80 9.61 22.87 9.50 22.93 9.38C22.98 9.26 23.01 9.12 23.01 8.99"
          "C23.01 8.86 22.98 8.73 22.93 8.60C22.88 8.48 22.81 8.37 22.71 8.28C22.62 8.18 22.51 8.11 22.39 8.06C22.26 8.01 22.13 7.98 22.00 7.98"
          "C21.87 7.98 21.73 8.01 21.61 8.06C21.49 8.12 21.38 8.19 21.29 8.29Z" },
    // captions: filled while they are on, outlined while off (as YouTube)
    { 24, "M21 3H3C2.46 3 1.96 3.21 1.58 3.58C1.21 3.96 1 4.46 1 5V19C1 19.53 1.21 20.03 1.58 20.41C1.96 20.78 2.46 21 3 21H21C21.53 21 22.03 20.78 22.41 20.41"
          "C22.78 20.03 23 19.53 23 19V5C23 4.46 22.78 3.96 22.41 3.58C22.03 3.21 21.53 3 21 3ZM6 11H8C8.26 11 8.51 11.10 8.70 11.29C8.89 11.48 9 11.73 9 12"
          "C9 12.26 8.89 12.51 8.70 12.70C8.51 12.89 8.26 13 8 13H6C5.73 13 5.48 12.89 5.29 12.70C5.10 12.51 5 12.26 5 12C5 11.73 5.10 11.48 5.29 11.29"
          "C5.48 11.10 5.73 11 6 11ZM12 11H18C18.26 11 18.51 11.10 18.70 11.29C18.89 11.48 19 11.73 19 12C19 12.26 18.89 12.51 18.70 12.70"
          "C18.51 12.89 18.26 13 18 13H12C11.73 13 11.48 12.89 11.29 12.70C11.10 12.51 11 12.26 11 12C11 11.73 11.10 11.48 11.29 11.29"
          "C11.48 11.10 11.73 11 12 11ZM16 15H18C18.26 15 18.51 15.10 18.70 15.29C18.89 15.48 19 15.73 19 16C19 16.26 18.89 16.51 18.70 16.70"
          "C18.51 16.89 18.26 17 18 17H16C15.73 17 15.48 16.89 15.29 16.70C15.10 16.51 15 16.26 15 16C15 15.73 15.10 15.48 15.29 15.29"
          "C15.48 15.10 15.73 15 16 15ZM6 15H12C12.26 15 12.51 15.10 12.70 15.29C12.89 15.48 13 15.73 13 16C13 16.26 12.89 16.51 12.70 16.70"
          "C12.51 16.89 12.26 17 12 17H6C5.73 17 5.48 16.89 5.29 16.70C5.10 16.51 5 16.26 5 16C5 15.73 5.10 15.48 5.29 15.29C5.48 15.10 5.73 15 6 15Z" },
    { 24, "M21.20 3.01L21 3H3L2.79 3.01C2.30 3.06 1.84 3.29 1.51 3.65C1.18 4.02 .99 4.50 1 5V19L1.01 19.20C1.05 19.66 1.26 20.08 1.58 20.41"
          "C1.91 20.73 2.33 20.94 2.79 20.99L3 21H21L21.20 20.98C21.66 20.94 22.08 20.73 22.41 20.41C22.73 20.08 22.94 19.66 22.99 19.20L23 19V5"
          "C23.00 4.50 22.81 4.02 22.48 3.65C22.15 3.29 21.69 3.06 21.20 3.01ZM3 19V5H21V19H3ZM8 11H6C5.73 11 5.48 11.10 5.29 11.29C5.10 11.48 5 11.73 5 12"
          "C5 12.26 5.10 12.51 5.29 12.70C5.48 12.89 5.73 13 6 13H8C8.26 13 8.51 12.89 8.70 12.70C8.89 12.51 9 12.26 9 12C9 11.73 8.89 11.48 8.70 11.29"
          "C8.51 11.10 8.26 11 8 11ZM18 11H12C11.73 11 11.48 11.10 11.29 11.29C11.10 11.48 11 11.73 11 12C11 12.26 11.10 12.51 11.29 12.70"
          "C11.48 12.89 11.73 13 12 13H18C18.26 13 18.51 12.89 18.70 12.70C18.89 12.51 19 12.26 19 12C19 11.73 18.89 11.48 18.70 11.29"
          "C18.51 11.10 18.26 11 18 11ZM18 15H16C15.73 15 15.48 15.10 15.29 15.29C15.10 15.48 15 15.73 15 16C15 16.26 15.10 16.51 15.29 16.70"
          "C15.48 16.89 15.73 17 16 17H18C18.26 17 18.51 16.89 18.70 16.70C18.89 16.51 19 16.26 19 16C19 15.73 18.89 15.48 18.70 15.29"
          "C18.51 15.10 18.26 15 18 15ZM12 15H6C5.73 15 5.48 15.10 5.29 15.29C5.10 15.48 5 15.73 5 16C5 16.26 5.10 16.51 5.29 16.70"
          "C5.48 16.89 5.73 17 6 17H12C12.26 17 12.51 16.89 12.70 16.70C12.89 16.51 13 16.26 13 16C13 15.73 12.89 15.48 12.70 15.29"
          "C12.51 15.10 12.26 15 12 15Z" },
    // settings
    { 24, "M12.84 1H11.15C10.72 .99 10.30 1.14 9.95 1.40C9.60 1.66 9.35 2.02 9.23 2.44L9.19 2.61C9.11 3.00 8.96 3.38 8.73 3.71C8.51 4.04 8.22 4.33 7.89 4.55"
          "L7.75 4.64C7.37 4.85 6.96 4.98 6.53 5.02C6.11 5.06 5.68 5.01 5.27 4.87C4.86 4.73 4.42 4.73 4.00 4.86C3.59 5.00 3.23 5.26 2.99 5.62L2.89 5.77"
          "L2.05 7.23C1.82 7.63 1.73 8.10 1.81 8.55C1.88 9.01 2.12 9.43 2.47 9.73L2.58 9.84C3.15 10.39 3.50 11.15 3.50 12L3.49 12.16C3.47 12.56 3.37 12.95 3.19 13.31"
          "C3.01 13.67 2.77 13.99 2.47 14.26C2.12 14.56 1.88 14.98 1.81 15.43C1.73 15.89 1.82 16.36 2.05 16.76L2.89 18.22L2.99 18.37C3.24 18.73 3.59 18.99 4.01 19.13"
          "C4.42 19.26 4.86 19.26 5.27 19.12L5.42 19.07C5.81 18.96 6.21 18.93 6.61 18.98C7.01 19.03 7.40 19.15 7.75 19.36L7.89 19.44C8.22 19.66 8.51 19.95 8.73 20.28"
          "C8.96 20.61 9.11 20.99 9.19 21.38C9.28 21.84 9.52 22.24 9.88 22.54C10.24 22.83 10.69 23.00 11.15 23H12.84C13.30 23.00 13.75 22.83 14.11 22.54"
          "C14.47 22.24 14.71 21.84 14.80 21.38C14.89 20.96 15.06 20.56 15.31 20.21C15.55 19.86 15.88 19.57 16.25 19.36L16.39 19.28C16.75 19.10 17.14 18.99 17.54 18.96"
          "C17.94 18.94 18.34 18.99 18.72 19.12L18.89 19.17C19.31 19.27 19.75 19.24 20.15 19.07C20.55 18.90 20.88 18.60 21.10 18.23L21.95 16.76"
          "C22.18 16.36 22.26 15.89 22.19 15.43C22.11 14.98 21.88 14.56 21.53 14.26C21.23 13.99 20.98 13.67 20.80 13.31C20.63 12.95 20.52 12.56 20.50 12.16"
          "L20.50 12C20.50 11.57 20.59 11.14 20.77 10.75C20.94 10.36 21.20 10.01 21.53 9.73C21.88 9.43 22.11 9.01 22.19 8.55C22.26 8.10 22.18 7.63 21.95 7.23"
          "L21.10 5.76C20.88 5.39 20.55 5.09 20.15 4.92C19.76 4.75 19.31 4.72 18.89 4.82L18.72 4.87C18.34 5.00 17.94 5.05 17.54 5.03C17.14 5.00 16.75 4.89 16.4 4.71"
          "L16.25 4.63C15.88 4.42 15.56 4.13 15.31 3.78C15.06 3.43 14.89 3.03 14.80 2.61C14.71 2.15 14.47 1.74 14.11 1.45C13.75 1.16 13.30 .99 12.84 1Z"
          "M11.15 3H12.84C12.98 3.70 13.26 4.36 13.68 4.94C14.09 5.52 14.63 6.01 15.25 6.37C15.87 6.72 16.55 6.94 17.26 7.01C17.97 7.08 18.69 6.99 19.37 6.76"
          "L20.21 8.23C19.67 8.69 19.24 9.27 18.94 9.92C18.65 10.57 18.50 11.28 18.5 12C18.50 12.71 18.65 13.42 18.95 14.07C19.24 14.72 19.67 15.29 20.21 15.76"
          "L19.37 17.23C18.69 16.99 17.97 16.91 17.26 16.98C16.55 17.05 15.86 17.27 15.25 17.63C14.63 17.98 14.09 18.47 13.68 19.05C13.26 19.63 12.98 20.29 12.84 21"
          "H11.15C11.01 20.29 10.73 19.63 10.31 19.05C9.90 18.47 9.36 17.98 8.75 17.62C8.13 17.27 7.44 17.05 6.73 16.98C6.02 16.91 5.30 16.99 4.62 17.23"
          "L3.78 15.76C4.32 15.29 4.75 14.71 5.05 14.06C5.34 13.41 5.49 12.71 5.5 12C5.50 11.28 5.34 10.57 5.05 9.92C4.75 9.27 4.32 8.69 3.78 8.23"
          "L4.62 6.76C5.30 7.00 6.02 7.08 6.73 7.01C7.44 6.94 8.13 6.72 8.75 6.37C9.36 6.01 9.90 5.52 10.31 4.94C10.73 4.36 11.01 3.70 11.15 3Z"
          "M12.00 8C10.94 8 9.92 8.42 9.17 9.17C8.42 9.92 8.00 10.93 8.00 12C8.00 13.06 8.42 14.07 9.17 14.82C9.92 15.57 10.94 16 12.00 16"
          "C13.06 16 14.08 15.57 14.83 14.82C15.58 14.07 16.00 13.06 16.00 12C16.00 10.93 15.58 9.92 14.83 9.17C14.08 8.42 13.06 8 12.00 8Z"
          "M12.00 10H12L12.20 10.01C12.69 10.06 13.15 10.29 13.48 10.65C13.81 11.02 14.00 11.50 14 12L13.99 12.20C13.95 12.58 13.80 12.95 13.55 13.25"
          "C13.31 13.55 12.98 13.78 12.62 13.90C12.25 14.02 11.85 14.03 11.48 13.93C11.11 13.83 10.77 13.62 10.51 13.34C10.25 13.05 10.08 12.69 10.02 12.31"
          "C9.96 11.93 10.01 11.54 10.17 11.18C10.32 10.83 10.58 10.53 10.91 10.32C11.23 10.11 11.61 10.00 12 10Z" },
    // the YouTube page (Pluma's own button, drawn in the same style): a window with lines of text
    { 24, "M3 3H21C22.1 3 23 3.9 23 5V19C23 20.1 22.1 21 21 21H3C1.9 21 1 20.1 1 19V5C1 3.9 1.9 3 3 3Z M3 8V19H21V8H3Z "
          "M6 11H18V13H6Z M6 15H14V17H6Z" },
    // fullscreen, and leaving it (the same arrows, turned inwards)
    { 24, "M10 3H3V10C3 10.26 3.10 10.51 3.29 10.70C3.48 10.89 3.73 11 4 11C4.26 11 4.51 10.89 4.70 10.70C4.89 10.51 5 10.26 5 10V6.41L9.29 10.70"
          "L9.36 10.77C9.56 10.92 9.80 11.00 10.04 10.99C10.29 10.98 10.52 10.87 10.70 10.70C10.87 10.52 10.98 10.29 10.99 10.04C11.00 9.80 10.92 9.56 10.77 9.36"
          "L10.70 9.29L6.41 5H10C10.26 5 10.51 4.89 10.70 4.70C10.89 4.51 11 4.26 11 4C11 3.73 10.89 3.48 10.70 3.29C10.51 3.10 10.26 3 10 3Z"
          "M20 13C19.73 13 19.48 13.10 19.29 13.29C19.10 13.48 19 13.73 19 14V17.58L14.70 13.29L14.63 13.22C14.43 13.07 14.19 12.99 13.95 13.00"
          "C13.70 13.01 13.47 13.12 13.29 13.29C13.12 13.47 13.01 13.70 13.00 13.95C12.99 14.19 13.07 14.43 13.22 14.63L13.29 14.70L17.58 19H14"
          "C13.73 19 13.48 19.10 13.29 19.29C13.10 19.48 13 19.73 13 20C13 20.26 13.10 20.51 13.29 20.70C13.48 20.89 13.73 21 14 21H21V14"
          "C21 13.73 20.89 13.48 20.70 13.29C20.51 13.10 20.26 13 20 13Z" },
    { 24, "M9 4H11V11H4V9H9Z M4.41 2.99L10.21 8.79L8.79 10.21L2.99 4.41Z M15 20H13V13H20V15H15Z M19.59 21.01L13.79 15.21L15.21 13.79L21.01 19.59Z" },
    // the settings menu: speed, quality, its arrows and the tick
    { 24, "M12 1c1.44 0 2.87.28 4.21.83a11 11 0 0 1 3.45 2.27l-1.81 1.05A9 9 0 0 0 3 12a9 9 0 0 0 18-.00l-.01-.44a8.99 8.99 0 0 0-.14-1.20"
          "l1.81-1.05A11.00 11.00 0 0 1 10.51 22.9 11 11 0 0 1 12 1Zm7.08 6.25-7.96 3.25a1.74 1.74 0 1 0 1.73 2.99l6.8-5.26a.57.57 0 0 0-.56-.98Z" },
    { 24, "M9 3C8.11 2.99 7.25 3.29 6.54 3.83C5.84 4.38 5.34 5.14 5.12 6H3C2.73 6 2.48 6.10 2.29 6.29C2.10 6.48 2 6.73 2 7C2 7.26 2.10 7.51 2.29 7.70"
          "C2.48 7.89 2.73 8 3 8H5.12C5.34 8.85 5.84 9.61 6.55 10.16C7.25 10.70 8.11 10.99 9 10.99C9.88 10.99 10.74 10.70 11.44 10.16C12.15 9.61 12.65 8.85 12.87 8"
          "H21C21.26 8 21.51 7.89 21.70 7.70C21.89 7.51 22 7.26 22 7C22 6.73 21.89 6.48 21.70 6.29C21.51 6.10 21.26 6 21 6H12.87C12.65 5.14 12.15 4.38 11.45 3.83"
          "C10.74 3.29 9.88 2.99 9 3ZM9 5C9.53 5 10.03 5.21 10.41 5.58C10.78 5.96 11 6.46 11 7C11 7.53 10.78 8.03 10.41 8.41C10.03 8.78 9.53 9 9 9"
          "C8.46 9 7.96 8.78 7.58 8.41C7.21 8.03 7 7.53 7 7C7 6.46 7.21 5.96 7.58 5.58C7.96 5.21 8.46 5 9 5ZM15 13C14.11 12.99 13.25 13.29 12.54 13.83"
          "C11.84 14.38 11.34 15.14 11.12 16H3C2.73 16 2.48 16.10 2.29 16.29C2.10 16.48 2 16.73 2 17C2 17.26 2.10 17.51 2.29 17.70C2.48 17.89 2.73 18 3 18"
          "H11.12C11.34 18.85 11.84 19.61 12.55 20.16C13.25 20.70 14.11 20.99 15 20.99C15.88 20.99 16.74 20.70 17.44 20.16C18.15 19.61 18.65 18.85 18.87 18"
          "H21C21.26 18 21.51 17.89 21.70 17.70C21.89 17.51 22 17.26 22 17C22 16.73 21.89 16.48 21.70 16.29C21.51 16.10 21.26 16 21 16H18.87"
          "C18.65 15.14 18.15 14.38 17.45 13.83C16.74 13.29 15.88 12.99 15 13ZM15 15C15.53 15 16.03 15.21 16.41 15.58C16.78 15.96 17 16.46 17 17"
          "C17 17.53 16.78 18.03 16.41 18.41C16.03 18.78 15.53 19 15 19C14.46 19 13.96 18.78 13.58 18.41C13.21 18.03 13 17.53 13 17C13 16.46 13.21 15.96 13.58 15.58"
          "C13.96 15.21 14.46 15 15 15Z" },
    { 24, "M9.4 18 8 16.6 12.6 12 8 7.4 9.4 6l6 6z" },
    { 24, "M15.4 7.4 14 6l-6 6 6 6 1.4-1.4L10.8 12z" },
    { 24, "M9 16.2 4.8 12l-1.4 1.4L9 19 21 7l-1.4-1.4z" } };
static ComPtr<ID2D1PathGeometry> g_icons[I_COUNT];
static void draw_icon(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, Icon ic, float cx, float cy, float size, D2D1_COLOR_F c) {
    // size: the icon's whole box (24 px for YouTube's buttons), centred on cx, cy
    if (!g_icons[ic]) return;
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    float vb = kIcons[ic].vb, k = size / vb;
    rt->SetTransform(D2D1::Matrix3x2F::Translation(-vb / 2, -vb / 2) * D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(cx, cy) * old);
    b->SetColor(c);
    rt->FillGeometry(g_icons[ic].Get(), b);
    rt->SetTransform(old);
}
static void draw_volume_icon(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, const Video *v, float cx, float cy, float size, D2D1_COLOR_F c) {
    if (v->muted || v->volume <= 0) { draw_icon(rt, b, I_MUTED, cx, cy, size, c); return; }
    draw_icon(rt, b, I_SPEAKER, cx, cy, size, c);
    draw_icon(rt, b, I_WAVE1, cx, cy, size, c);
    if (v->volume >= 50) draw_icon(rt, b, I_WAVE2, cx, cy, size, c);
}
static ComPtr<IDWriteTextFormat> font(float px, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    ComPtr<IDWriteTextFormat> f;
    g_dw->CreateTextFormat(g_family.c_str(), g_roboto.Get(), w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"es-es", &f);
    if (!f) return f;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    ComPtr<IDWriteInlineObject> dots;
    g_dw->CreateEllipsisTrimmingSign(f.Get(), &dots);
    f->SetTrimming(&tr, dots.Get());
    return f;
}
static ComPtr<IDWriteTextFormat> f_time, f_tip, f_key, f_menu, f_menu_r, f_head, f_title, f_author, f_big;
static float g_fonts_scale;
static void make_fonts() {
    if (g_fonts_scale == ui_scale()) return;
    g_fonts_scale = ui_scale();
    f_time = font(S(14), DWRITE_FONT_WEIGHT_MEDIUM);
    f_tip = font(S(12.98f), DWRITE_FONT_WEIGHT_MEDIUM);
    f_key = font(S(11), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    f_menu = font(S(14), DWRITE_FONT_WEIGHT_NORMAL);
    f_menu_r = font(S(14), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    f_head = font(S(14), DWRITE_FONT_WEIGHT_MEDIUM);
    f_title = font(S(18), DWRITE_FONT_WEIGHT_NORMAL);
    f_author = font(S(14), DWRITE_FONT_WEIGHT_NORMAL);
    f_big = font(S(17), DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
}
static void text(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, IDWriteTextFormat *f, const std::wstring &s, D2D1_RECT_F r, D2D1_COLOR_F c, bool shadow = false) {
    if (!f || s.empty()) return;
    if (shadow) {
        // YouTube's text-shadow (0 0 2px rgba(0,0,0,.5)), close enough without a blur
        b->SetColor(rgba(0, 0, 0, 0.35f * c.a));
        D2D1_RECT_F sh = { r.left, r.top + S(1), r.right, r.bottom + S(1) };
        rt->DrawText(s.c_str(), (UINT32)s.size(), f, sh, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    b->SetColor(c);
    rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static float text_width(IDWriteTextFormat *f, const std::wstring &s) {
    ComPtr<IDWriteTextLayout> l;
    if (!f || s.empty() || FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m; l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}
static std::wstring clock_text(double s) {
    if (s < 0 || s != s) s = 0;
    long long t = (long long)s;
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf_s(b, L"%lld:%02lld", t / 60, t % 60);
    return b;
}
static std::wstring speed_text(double s) {
    if (fabs(s - 1) < 0.01) return L"Normal";
    wchar_t b[16]; swprintf_s(b, L"%g", s);
    std::wstring r = b;
    for (auto &c : r) if (c == L'.') c = L',';
    return r;
}
static std::wstring quality_text(int h) { return std::to_wstring(h) + L"p" + (h >= 2160 ? L" 4K" : h >= 1080 ? L" HD" : L""); }

// ---------------------------------------------------------------- layered windows over the video
struct Layer {
    HWND h = NULL; int w = 0, hh = 0;
    HDC dc = NULL; HBITMAP bmp = NULL; HGDIOBJ old = NULL; void *bits = NULL;
    ComPtr<ID2D1DCRenderTarget> rt; ComPtr<ID2D1SolidColorBrush> br;
    POINT pos{}; bool shown = false; float alpha = 1;
};
static Layer g_bottom, g_top, g_menu, g_center;
static Anim g_ctl;              // the controls: 1 shown, 0 hidden
static Anim g_vol_open;         // the volume slider opening
static Anim g_bar_hot;          // the progress bar growing under the mouse
static int g_hot = 0;           // control under the mouse
static bool g_drag_seek, g_drag_vol;
static float g_mouse_x = -1;    // over the progress bar
static int g_menu_page = 0;     // 0 closed, 1 main, 2 quality, 3 speed, 4 captions
static int g_menu_hot = -1;
static POINT g_last_cursor = { -1, -1 };
enum { C_NONE, C_PLAY, C_MUTE, C_VOLBAR, C_CC, C_GEAR, C_PAGE, C_FULL, C_BAR };
static void layer_place(Layer &l, int x, int y, int w, int hh, bool click_through) {
    if (l.w != w || l.hh != hh) {
        l.rt.Reset(); l.br.Reset();
        if (l.dc) { SelectObject(l.dc, l.old); DeleteDC(l.dc); l.dc = NULL; }
        if (l.bmp) { DeleteObject(l.bmp); l.bmp = NULL; }
        l.w = w; l.hh = hh;
    }
    l.pos = { x, y };
    if (!l.h) l.h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (click_through ? WS_EX_TRANSPARENT : 0),
                                    L"PlumaControlesVideo", L"", WS_POPUP, x, y, w, hh, g_win, NULL, g_inst, NULL);
}
static void layer_show(Layer &l, const std::function<void(ID2D1RenderTarget *, ID2D1SolidColorBrush *)> &draw, float alpha) {
    if (!l.h || l.w <= 0 || l.hh <= 0) return;
    if (!l.dc) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = l.w; bi.bmiHeader.biHeight = -l.hh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(NULL);
        l.dc = CreateCompatibleDC(screen);
        ReleaseDC(NULL, screen);
        l.bmp = CreateDIBSection(l.dc, &bi, DIB_RGB_COLORS, &l.bits, NULL, 0);
        l.old = SelectObject(l.dc, l.bmp);
    }
    if (!l.rt) {
        // drawn by the CPU: a handful of pills and icons, and no graphics device of its own for each layer
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(g_d2d->CreateDCRenderTarget(&rp, &l.rt))) return;
        l.rt->CreateSolidColorBrush(kWhite, &l.br);
        l.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    RECT r = { 0, 0, l.w, l.hh };
    l.rt->BindDC(l.dc, &r);
    l.rt->BeginDraw();
    l.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw(l.rt.Get(), l.br.Get());
    if (l.rt->EndDraw() == D2DERR_RECREATE_TARGET) { l.rt.Reset(); l.br.Reset(); }
    l.alpha = std::max(0.0f, std::min(1.0f, alpha));
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)lroundf(255 * l.alpha), AC_SRC_ALPHA };
    SIZE sz = { l.w, l.hh };
    POINT zero = { 0, 0 };
    UpdateLayeredWindow(l.h, NULL, &l.pos, &sz, l.dc, &zero, 0, &bf, ULW_ALPHA);
    if (!l.shown) { SetWindowPos(l.h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); l.shown = true; }
}
static void layer_hide(Layer &l) { if (l.h && l.shown) { ShowWindow(l.h, SW_HIDE); l.shown = false; } }

// The bottom controls, as YouTube lays them out (CSS px, measured on its page): the progress bar 59 px above the
// player's bottom, 12 px in from each side; a row of 40 px pills 8 px above the bottom: play, volume, time on the left;
// captions, settings, page, fullscreen in one pill on the right.
static const float kBottomH = 130;
static float bar_y(const Layer &l) { return l.hh - S(59); }
static D2D1_RECT_F bar_hit(const Layer &l) { return { S(12), bar_y(l) - S(10), l.w - S(12), bar_y(l) + S(7) }; }
static float row_top(const Layer &l) { return l.hh - S(48); }
static D2D1_RECT_F play_rect(const Layer &l) { return { S(12), row_top(l), S(52), row_top(l) + S(40) }; }
static D2D1_RECT_F mute_rect(const Layer &l) { return { S(64), row_top(l), S(104), row_top(l) + S(40) }; }
static D2D1_RECT_F vol_pill(const Layer &l) { D2D1_RECT_F m = mute_rect(l); m.right += S(71) * g_vol_open.at(now_ms()); return m; }
static D2D1_RECT_F vol_slider(const Layer &l) { D2D1_RECT_F m = mute_rect(l); return { m.right, m.top, m.right + S(52), m.bottom }; }
static int right_buttons(const Video *v) { return v->caps.empty() ? 3 : 4; }
static D2D1_RECT_F right_btn(const Layer &l, int i) {   // 0 fullscreen, 1 page, 2 settings, 3 captions
    float x1 = l.w - S(12) - S(4) - i * S(48);
    return { x1 - S(48), row_top(l), x1, row_top(l) + S(40) };
}
static D2D1_RECT_F right_pill(const Layer &l, const Video *v) {
    return { l.w - S(12) - S(8) - right_buttons(v) * S(48), row_top(l), l.w - S(12), row_top(l) + S(40) };
}
static bool in(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void pill(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, D2D1_RECT_F r, D2D1_COLOR_F c) {
    float rad = (r.bottom - r.top) / 2;
    b->SetColor(c);
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b);
}

// ---------------------------------------------------------------- the player
static void post_events(void *ctx) { PostMessageW(g_msg, WM_VIDEO_EVENTS, 0, (LPARAM)(INT_PTR)ctx); }
static void mpv_cmd(Video *v, std::initializer_list<const char *> args) {
    if (!v || !v->mpv) return;
    std::vector<const char *> a(args);
    a.push_back(NULL);
    M.command(v->mpv, a.data());
}
static void set_flag(Video *v, const char *name, bool on) { if (v && v->mpv) { int f = on ? 1 : 0; M.set_property(v->mpv, name, MPV_FORMAT_FLAG, &f); } }
static void set_double(Video *v, const char *name, double d) { if (v && v->mpv) M.set_property(v->mpv, name, MPV_FORMAT_DOUBLE, &d); }
static void apply_volume(Video *v) {
    // YouTube's volume is the element's gain (linear); mpv's is cubic. Same loudness at the swap, and YouTube's own
    // loudness normalisation (it lowers loud videos) carried over as mpv's volume-gain
    set_double(v, "volume", 100.0 * cbrt(std::max(0.0, v->volume) / 100.0));
    set_double(v, "volume-gain", v->gain_db);
}
static std::string escaped(const std::string &s) { return "%" + std::to_string(s.size()) + "%" + s; }   // mpv's quoting for option values
static void load_streams(Video *v, double start, bool paused) {
    // the playlists Pluma already read (saved next to its data): mpv goes straight to the pieces
    if (!v->mpv || v->vf < 0) return;
    char st[64]; sprintf_s(st, "%.3f", start);
    std::string opts = std::string("start=") + st + ",pause=" + (paused ? "yes" : "no");
    const Fmt &vf = v->fmts[v->vf];
    if (v->af >= 0) { const Fmt &af = v->fmts[v->af]; opts += ",audio-file=" + escaped(af.local.empty() ? af.url : af.local); }
    // ~10 s of video ahead, at least 4 MB: at 1080p that is the 4 MB measured; 4K at 60 fps (28 Mb/s, pieces of
    // 12 MB, which download here at 24-30 Mb/s) stopped to load with 4 MB (25/10)
    long long mb = std::max(4LL, (vf.br / 8 * 10) >> 20);
    opts += ",demuxer-max-bytes=" + std::to_string(mb) + "MiB";
    v->restarted = v->loaded = false;
    mpv_cmd(v, { "loadfile", (vf.local.empty() ? vf.url : vf.local).c_str(), "replace", "-1", opts.c_str() });
}
static bool http_get(const std::wstring &url, std::string &out);
static bool safe_playlist(const std::string &text);
static void fetch_playlists(Video *v, std::vector<int> which, std::function<void(Video *)> then) {
    // YouTube's playlist server takes ~1 s per list: Pluma reads the ones it needs at once, on threads of its own,
    // and mpv opens them from disk (it starts in ~1.5 s instead of 4-5; measured 25/10)
    std::vector<std::pair<int, std::string>> todo;
    for (int i : which) if (i >= 0 && v->fmts[i].local.empty()) todo.push_back({ i, v->fmts[i].url });
    int id = v->tab, serial = v->serial;
    std::wstring dir = g_state + L"\\cache-video";
    std::wstring vid = v->id;
    std::thread([todo, id, serial, dir, vid, then] {
        std::vector<std::string> paths(todo.size());
        std::vector<std::thread> th;
        for (size_t k = 0; k < todo.size(); k++)
            th.emplace_back([&, k] {
                std::string text;
                if (!http_get(wide(todo[k].second), text) || text.rfind("#EXTM3U", 0) != 0 || !safe_playlist(text)) return;
                std::wstring p = dir + L"\\" + vid + L"-" + std::to_wstring(todo[k].first) + L".m3u8";
                if (write_file(p, text)) paths[k] = utf8(p);
            });
        for (auto &t : th) t.join();
        ui_post([todo, paths, id, serial, then] {
            Video *v = V(tab_by_id(id));
            if (!v || v->serial != serial) return;
            for (size_t k = 0; k < todo.size(); k++) if (!paths[k].empty()) v->fmts[todo[k].first].local = paths[k];
            then(v);
        });
    }).detach();
}
static void tame(Video *v) {
    // mpv draws in a window of its own inside ours; disabled, its mouse and keys come to ours (Pluma's input)
    if (!v || !v->wnd) return;
    for (HWND c = GetWindow(v->wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        if (IsWindowEnabled(c)) EnableWindow(c, FALSE);
}
static RECT video_rect() {
    RECT rc; GetClientRect(g_win, &rc);
    rc.top = (LONG)ui_band_height();
    if (rc.top > rc.bottom) rc.top = rc.bottom;
    return rc;
}
static LRESULT CALLBACK video_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static bool start_player(Video *v, double start) {
    if (!load_mpv()) return false;
    RECT r = video_rect();
    // shown under the page while it waits: mpv draws its first frame there, so the swap shows no black
    v->wnd = CreateWindowExW(0, L"PlumaVideo", L"", WS_CHILD | WS_CLIPSIBLINGS, r.left, r.top, r.right - r.left, r.bottom - r.top, g_win, NULL, g_inst, NULL);
    if (!v->wnd) return false;
    SetWindowLongPtrW(v->wnd, GWLP_USERDATA, v->tab);
    if (tab_by_id(v->tab) == active_tab()) SetWindowPos(v->wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    v->mpv = M.create();
    if (!v->mpv) return false;
    char wid[32]; sprintf_s(wid, "%lld", (long long)(INT_PTR)v->wnd);
    std::string fonts = utf8(g_root + L"\\fuentes");
    std::wstring cache = g_state + L"\\cache-video";
    SHCreateDirectoryExW(NULL, cache.c_str(), NULL);
    std::string shaders = utf8(cache);
    struct { const char *k; std::string v; } opts[] = {
        { "wid", wid }, { "config", "no" }, { "load-scripts", "no" }, { "ytdl", "no" }, { "osc", "no" }, { "osd-level", "0" },
        { "input-default-bindings", "no" }, { "input-vo-keyboard", "no" }, { "input-cursor", "no" }, { "cursor-autohide", "no" },
        { "window-dragging", "no" }, { "terminal", "no" }, { "msg-level", "all=no" },
        // the shaders, compiled once and kept: with them on disk mpv never starts its compiler (72 MB playing,
        // against 92 compiling them each time; measured 25/10)
        { "hwdec", "d3d11va" }, { "vo", "gpu" }, { "gpu-api", "d3d11" }, { "gpu-shader-cache", "yes" }, { "gpu-shader-cache-dir", shaders },
        // a small buffer: YouTube's HLS at 1080p, 93 MB playing with 4 MB (110 with 10 MB; measured 25/10), still two
        // pieces of ~5 s ahead
        { "cache", "yes" }, { "demuxer-max-bytes", "4MiB" }, { "demuxer-max-back-bytes", "512KiB" }, { "demuxer-readahead-secs", "10" },
        // YouTube serves these streams only in closed ranges (an open "bytes=0-" gets a 403, seen 25/10): pieces of
        // 10 MB, as yt-dlp asks for them
        { "curl-max-request-size", "10MiB" }, { "curl-buffer-size", "1MiB" },
        { "keep-open", "yes" }, { "idle", "yes" }, { "background-color", "#000000" }, { "sub-auto", "no" },
        // (no looking around the folder; and not autoload-files=no, which leaves the audio file unselected: mute)
        { "audio-file-auto", "no" }, { "cover-art-auto", "no" },
        // captions as YouTube draws them: white on a dark box
        { "sub-fonts-dir", fonts }, { "sub-font", "Roboto" }, { "sub-font-size", "34" }, { "sub-color", "#FFFFFF" },
        { "sub-border-style", "background-box" }, { "sub-back-color", "#BF080808" }, { "sub-shadow-offset", "0" }, { "sub-margin-y", "44" } };
    for (auto &o : opts) M.set_option_string(v->mpv, o.k, o.v.c_str());
    if (g_dev) { M.set_option_string(v->mpv, "log-file", utf8(g_state + L"\\mpv.log").c_str()); M.set_option_string(v->mpv, "msg-level", "all=v"); }   // tests
    if (M.initialize(v->mpv) < 0) { M.terminate_destroy(v->mpv); v->mpv = nullptr; return false; }
    static const char *props[] = { "time-pos", "duration", "pause", "eof-reached", "paused-for-cache", "demuxer-cache-time", "mute", "speed" };
    static const int fmt[] = { MPV_FORMAT_DOUBLE, MPV_FORMAT_DOUBLE, MPV_FORMAT_FLAG, MPV_FORMAT_FLAG, MPV_FORMAT_FLAG, MPV_FORMAT_DOUBLE,
                               MPV_FORMAT_FLAG, MPV_FORMAT_DOUBLE };
    for (int i = 0; i < 8; i++) M.observe_property(v->mpv, (uint64_t)i, props[i], fmt[i]);
    M.set_wakeup_callback(v->mpv, post_events, (void *)(INT_PTR)v->tab);
    apply_volume(v);
    set_flag(v, "mute", v->muted);
    if (fabs(v->speed - 1) > 0.01) set_double(v, "speed", v->speed);
    load_streams(v, start, true);
    return true;
}
static void stop_player(Video *v) {
    // mpv winds down on a thread of its own (it can take a moment); its window goes once it has
    if (!v) return;
    HWND w = v->wnd;
    v->wnd = NULL;
    if (w) ShowWindow(w, SW_HIDE);
    if (v->mpv) {
        mpv_handle *h = v->mpv;
        v->mpv = nullptr;
        M.set_wakeup_callback(h, NULL, NULL);
        HWND msgw = g_msg;
        std::thread([h, w, msgw] { M.terminate_destroy(h); if (w) PostMessageW(msgw, WM_VIDEO_GONE, (WPARAM)w, 0); }).detach();
    } else if (w) DestroyWindow(w);
}
static void hide_controls_now() {
    layer_hide(g_bottom); layer_hide(g_top); layer_hide(g_menu); layer_hide(g_center);
    g_menu_page = 0; g_hot = C_NONE; g_mouse_x = -1;
}

// ---------------------------------------------------------------- choosing the streams
static int pick_video(const Video *v, int max_h) {
    // the tallest at or under max_h (and its highest frame rate), in a codec the graphics decode: VP9, else H.264,
    // else AV1 (HDR VP9 last: not every screen shows it)
    int best = -1;
    long long best_score = -1;
    for (size_t i = 0; i < v->fmts.size(); i++) {
        const Fmt &f = v->fmts[i];
        if (f.audio || f.h > max_h || f.h <= 0) continue;
        int pref = f.codec == L"vp09" ? 4 : f.codec == L"avc1" ? 3 : f.codec == L"av01" ? 2 : 1;
        long long score = (long long)f.h * 100000 + f.fps * 100 + pref;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0 && max_h < 100000) return pick_video(v, 100000);
    return best;
}
static int pick_audio(const Video *v) {
    // the audio of the chosen variant's group: the original track (not a dubbed one)
    if (v->vf < 0) return -1;
    const std::string &g = v->fmts[v->vf].group;
    int best = -1; long long top = -1;
    for (size_t i = 0; i < v->fmts.size(); i++) {
        const Fmt &f = v->fmts[i];
        if (!f.audio || (!g.empty() && f.group != g)) continue;
        long long score = (f.main_audio ? 1 : 0);
        if (score > top) { top = score; best = (int)i; }
    }
    return best;
}
// What mpv opens comes from a web page: only https addresses of Google's video servers (and YouTube's captions)
static bool google_url(const std::string &u, bool captions = false) {
    if (u.rfind("https://", 0) != 0) return false;
    std::string host = u.substr(8, u.find_first_of("/?#", 8) - 8);
    if (captions) return host == "www.youtube.com" && u.compare(8 + host.size(), 14, "/api/timedtext") == 0;
    const std::string dom = ".googlevideo.com";
    return host.size() > dom.size() && host.compare(host.size() - dom.size(), dom.size(), dom) == 0 &&
           host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") == std::string::npos;
}
static bool safe_playlist(const std::string &text) {
    // every address in a playlist mpv will open (pieces, its initial piece) on those servers too
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] != '#') { if (!google_url(line)) return false; continue; }
        size_t u = line.find("URI=\"");
        if (u != std::string::npos) {
            size_t e = line.find('"', u + 5);
            if (e == std::string::npos || !google_url(line.substr(u + 5, e - u - 5))) return false;
        }
    }
    return true;
}
// YouTube's HLS master list: the variants (one line of attributes, then its address) and the audio renditions
static std::string attr(const std::string &line, const char *name) {
    std::string key = std::string(name) + "=";
    size_t p = 0;
    while ((p = line.find(key, p)) != std::string::npos) {
        if (p == 0 || line[p - 1] == ',' || line[p - 1] == ':') break;
        p += key.size();
    }
    if (p == std::string::npos) return "";
    p += key.size();
    if (p < line.size() && line[p] == '"') { size_t e = line.find('"', p + 1); return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1); }
    size_t e = line.find(',', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
static std::vector<Fmt> parse_master(const std::string &m3u8) {
    std::vector<Fmt> out;
    size_t pos = 0;
    std::string pending;
    while (pos < m3u8.size()) {
        size_t nl = m3u8.find('\n', pos);
        std::string line = m3u8.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? m3u8.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#EXT-X-MEDIA:", 0) == 0 && attr(line, "TYPE") == "AUDIO") {
            Fmt f; f.audio = true; f.url = attr(line, "URI"); f.group = attr(line, "GROUP-ID");
            std::string name = attr(line, "NAME"), def = attr(line, "DEFAULT");
            f.main_audio = def.empty() || def == "YES" || name.find("original") != std::string::npos;
            if (google_url(f.url)) out.push_back(f);
        } else if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) pending = line;
        else if (!pending.empty() && !line.empty() && line[0] != '#') {
            Fmt f; f.url = line; f.group = attr(pending, "AUDIO");
            f.br = _atoi64(attr(pending, "BANDWIDTH").c_str());
            std::string res = attr(pending, "RESOLUTION");
            size_t x = res.find('x');
            if (x != std::string::npos) f.h = atoi(res.c_str() + x + 1);
            f.fps = (int)lround(atof(attr(pending, "FRAME-RATE").c_str()));
            std::string codecs = attr(pending, "CODECS");
            std::string first = codecs.substr(0, codecs.find(','));
            std::string tag = first.substr(0, first.find('.'));
            if (tag == "vp09" && first.rfind("vp09.00", 0) != 0) tag = "vp09hdr";   // profile 2: 10-bit HDR
            f.codec = wide(tag);
            if (f.h > 0 && google_url(f.url)) out.push_back(f);
            pending.clear();
        }
    }
    return out;
}
static bool http_get(const std::wstring &url, std::string &out) {
    // a plain GET (Pluma's own, from a worker thread): for YouTube's HLS list, which the page cannot read itself
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + (uc.lpszExtraInfo ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : L"");
    HINTERNET s = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        char buf[16384]; DWORD got = 0;
        while (WinHttpReadData(r, buf, sizeof(buf), &got) && got > 0 && out.size() < (4 << 20)) out.append(buf, got);
        ok = code == 200 && !out.empty();
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
static int target_height(const Video *v) {
    // as YouTube's "automatic": the smallest quality that fills the player (1080p on a 1080p screen, 1440p on a 2K
    // one...), or the best there is. Higher ones, from the menu
    RECT r = video_rect();
    int h = (int)((r.bottom - r.top) * 0.9), best = 0, fit = 0;
    for (auto &f : v->fmts) {
        if (f.audio || f.h <= 0) continue;
        best = std::max(best, f.h);
        if (f.h >= h && (!fit || f.h < fit)) fit = f.h;
    }
    return fit ? fit : best ? best : 1080;
}
static std::vector<int> each_quality(const Video *v) {
    // the variant each quality would play, and its audio: their lists are read ahead, so a change does not wait
    std::vector<int> out;
    std::vector<int> hs;
    for (auto &f : v->fmts) if (!f.audio && f.h > 0 && std::find(hs.begin(), hs.end(), f.h) == hs.end()) hs.push_back(f.h);
    Video tmp;
    tmp.fmts = v->fmts;
    for (int h : hs) {
        tmp.vf = pick_video(&tmp, h);
        if (tmp.vf < 0 || tmp.fmts[tmp.vf].h != h) continue;
        out.push_back(tmp.vf);
        int a = pick_audio(&tmp);
        if (a >= 0 && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    }
    return out;
}

// ---------------------------------------------------------------- painting
static const wchar_t *tip_text(Video *v, int c, const wchar_t **key) {
    *key = NULL;
    switch (c) {
    case C_PLAY: *key = L"k"; return v->eof ? L"Volver a reproducir" : v->paused ? L"Reproducir" : L"Pausa";
    case C_MUTE: *key = L"m"; return v->muted || v->volume <= 0 ? L"Activar sonido" : L"Silenciar";
    case C_CC: *key = L"c"; return L"Subtítulos";
    case C_GEAR: return L"Configuración";
    case C_PAGE: return L"Página de YouTube";
    case C_FULL: *key = L"f"; return g_fullscreen ? L"Salir de pantalla completa" : L"Pantalla completa";
    }
    return NULL;
}
static D2D1_RECT_F hot_rect(const Layer &l, const Video *v, int c) {
    switch (c) {
    case C_PLAY: return play_rect(l);
    case C_MUTE: return mute_rect(l);
    case C_CC: return right_btn(l, 3);
    case C_GEAR: return right_btn(l, 2);
    case C_PAGE: return right_btn(l, 1);
    case C_FULL: return right_btn(l, 0);
    }
    return { 0, 0, 0, 0 };
}
static void tooltip(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b, float cx, float bottom, float lw, const std::wstring &label, const wchar_t *key) {
    // YouTube's: a translucent box with the label and, after it, the key in a little keycap
    float tw = text_width(f_tip.Get(), label), kw = key ? std::max(S(18), text_width(f_key.Get(), key) + S(8)) : 0;
    float w = S(18) + tw + (key ? S(6) + kw : 0), h = S(27);
    float x = std::max(S(12), std::min(lw - S(12) - w, cx - w / 2));
    D2D1_RECT_F r = { x, bottom - h, x + w, bottom };
    b->SetColor(rgba(0, 0, 0, 0.3f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, S(8), S(8)), b);
    text(rt, b, f_tip.Get(), label, D2D1::RectF(x + S(9), r.top, x + S(9) + tw + S(2), r.bottom), kText, true);
    if (key) {
        D2D1_RECT_F k = { x + S(9) + tw + S(6), r.top + S(4.5f), x + S(9) + tw + S(6) + kw, r.bottom - S(4.5f) };
        b->SetColor(rgba(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.85f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left + S(0.5f), k.top + S(0.5f), k.right - S(0.5f), k.bottom - S(0.5f)), S(4), S(4)), b, S(1.2f));
        std::wstring ks = key;
        for (auto &c : ks) c = towupper(c);
        text(rt, b, f_key.Get(), ks, k, kText);
    }
}
static void paint_bottom(Video *v, float alpha) {
    Layer &l = g_bottom;
    double now = now_ms();
    layer_show(l, [&](ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b) {
        // progress bar: dark track, white buffered part, red played part (pink at the far end), red dot
        float hot = g_bar_hot.at(now);
        float th = S(4) + S(2) * hot, y = bar_y(l);
        float x0 = S(12), x1 = l.w - S(12), wbar = x1 - x0;
        double dur = v->dur > 0 ? v->dur : 1;
        float played = (float)std::max(0.0, std::min(1.0, v->pos / dur));
        float buffered = (float)std::max(0.0, std::min(1.0, (v->pos + v->cached) / dur));
        D2D1_RECT_F track = { x0, y - th / 2, x1, y + th / 2 };
        b->SetColor(rgba(40 / 255.0f, 40 / 255.0f, 40 / 255.0f, 0.6f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(track, th / 2, th / 2), b);
        b->SetColor(rgba(1, 1, 1, 0.4f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, track.top, x0 + wbar * buffered, track.bottom), th / 2, th / 2), b);
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP st[3] = { { 0, kRed }, { 0.8f, kRed }, { 1, rgba(1, 39 / 255.0f, 145 / 255.0f, 1) } };
        rt->CreateGradientStopCollection(st, 3, &stops);
        ComPtr<ID2D1LinearGradientBrush> grad;
        if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, 0), D2D1::Point2F(x1, 0)), stops.Get(), &grad);
        if (grad && played > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, track.top, x0 + wbar * played, track.bottom), th / 2, th / 2), grad.Get());
        b->SetColor(kRed);
        float dot = S(6) + S(1) * hot;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + wbar * played, y), dot, dot), b);
        // the left: play, volume (+ slider), time
        D2D1_RECT_F pr = play_rect(l);
        pill(rt, b, pr, kPill);
        if (g_hot == C_PLAY) pill(rt, b, pr, rgba(1, 1, 1, 0.1f));
        draw_icon(rt, b, v->eof ? I_REPLAY : v->paused ? I_PLAY : I_PAUSE, (pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2, S(24), kWhite);
        D2D1_RECT_F vp = vol_pill(l), mr = mute_rect(l);
        float open = g_vol_open.at(now);
        pill(rt, b, vp, kPill);
        if (g_hot == C_MUTE) pill(rt, b, mr, rgba(1, 1, 1, 0.1f));
        draw_volume_icon(rt, b, v, (mr.left + mr.right) / 2, (mr.top + mr.bottom) / 2, S(24), kWhite);
        if (open > 0.02f) {
            D2D1_RECT_F sl = vol_slider(l);
            float cy = (sl.top + sl.bottom) / 2, k = v->muted ? 0 : (float)(std::max(0.0, std::min(100.0, v->volume)) / 100.0);
            float hx = sl.left + S(6) + (S(52) - S(12)) * k;
            rt->PushAxisAlignedClip(D2D1::RectF(sl.left, sl.top, vp.right - S(4), sl.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
            b->SetColor(rgba(1, 1, 1, 0.2f * open)); rt->FillRectangle(D2D1::RectF(hx, cy - S(1.5f), sl.right, cy + S(1.5f)), b);
            b->SetColor(rgba(1, 1, 1, open)); rt->FillRectangle(D2D1::RectF(sl.left, cy - S(1.5f), hx, cy + S(1.5f)), b);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hx, cy), S(6), S(6)), b);
            rt->PopAxisAlignedClip();
        }
        std::wstring tm = clock_text(v->pos) + L" / " + clock_text(v->dur);
        float tw = text_width(f_time.Get(), tm);
        D2D1_RECT_F tp = { vp.right + S(8), pr.top, vp.right + S(8) + tw + S(32), pr.bottom };
        pill(rt, b, tp, kPill);
        text(rt, b, f_time.Get(), tm, D2D1::RectF(tp.left + S(16), tp.top, tp.right, tp.bottom), kText);
        // the right: captions, settings, page, fullscreen, in one pill
        pill(rt, b, right_pill(l, v), kPill);
        int hot_btn = g_menu_page ? C_GEAR : g_hot;
        for (int c : { C_FULL, C_PAGE, C_GEAR, C_CC }) {
            if (c == C_CC && v->caps.empty()) continue;
            D2D1_RECT_F r = hot_rect(l, v, c);
            float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            if (hot_btn == c) { b->SetColor(rgba(1, 1, 1, 0.1f)); rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), S(20), S(20)), b); }
            Icon ic = c == C_FULL ? (g_fullscreen ? I_FULL_EXIT : I_FULL) : c == C_PAGE ? I_PAGE : c == C_GEAR ? I_GEAR : v->sub >= 0 ? I_CC_ON : I_CC_OFF;
            draw_icon(rt, b, ic, cx, cy, S(24), kWhite);
        }
        // what is under the mouse: the time on the bar, or the button's name
        if ((g_hot == C_BAR || g_drag_seek) && g_mouse_x >= 0) {
            double t = (std::max(x0, std::min(x1, g_mouse_x)) - x0) / wbar * dur;
            tooltip(rt, b, std::max(x0, std::min(x1, g_mouse_x)), y - S(14), (float)l.w, clock_text(t), NULL);
        } else if (!g_menu_page && g_hot != C_NONE && g_hot != C_VOLBAR) {
            const wchar_t *key = NULL;
            const wchar_t *label = tip_text(v, g_hot, &key);
            D2D1_RECT_F r = hot_rect(l, v, g_hot);
            if (label) tooltip(rt, b, (r.left + r.right) / 2, row_top(l) - S(22), (float)l.w, label, key);
        }
    }, alpha);
}
static void paint_top(Video *v, float alpha) {
    layer_show(g_top, [&](ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b) {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP st[2] = { { 0, rgba(0, 0, 0, 0.5f) }, { 1, rgba(0, 0, 0, 0) } };
        rt->CreateGradientStopCollection(st, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> grad;
        if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, (float)g_top.hh)), stops.Get(), &grad);
        if (grad) rt->FillRectangle(D2D1::RectF(0, 0, (float)g_top.w, (float)g_top.hh), grad.Get());
        text(rt, b, f_title.Get(), v->title, D2D1::RectF(S(24), S(14), g_top.w - S(24), S(40)), kWhite, true);
        text(rt, b, f_author.Get(), v->author, D2D1::RectF(S(24), S(40), g_top.w - S(24), S(62)), rgba(1, 1, 1, 0.8f), true);
    }, alpha);
}
struct MenuRow { Icon icon; std::wstring label, value; bool on; };
static std::vector<MenuRow> menu_rows(Video *v) {
    std::vector<MenuRow> rows;
    if (g_menu_page == 1) {
        if (!v->caps.empty()) rows.push_back({ I_CC_OFF, L"Subtítulos", v->sub >= 0 ? v->caps[v->sub].name : L"Desactivados", false });
        rows.push_back({ I_SPEED, L"Velocidad de reproducción", speed_text(v->speed), false });
        std::wstring q = v->vf >= 0 ? std::to_wstring(v->fmts[v->vf].h) + L"p" : L"";
        rows.push_back({ I_QUALITY, L"Calidad", v->auto_quality ? L"Automática (" + q + L")" : quality_text(v->vf >= 0 ? v->fmts[v->vf].h : 0), false });
    } else if (g_menu_page == 2) {
        std::vector<int> hs;
        for (auto &f : v->fmts) if (!f.audio && f.h > 0 && std::find(hs.begin(), hs.end(), f.h) == hs.end()) hs.push_back(f.h);
        std::sort(hs.rbegin(), hs.rend());
        for (int h : hs) rows.push_back({ I_COUNT, quality_text(h), L"", !v->auto_quality && v->vf >= 0 && v->fmts[v->vf].h == h });
        rows.push_back({ I_COUNT, L"Automática", L"", v->auto_quality });
    } else if (g_menu_page == 3) {
        for (double s : { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 }) rows.push_back({ I_COUNT, speed_text(s), L"", fabs(v->speed - s) < 0.01 });
    } else if (g_menu_page == 4) {
        rows.push_back({ I_COUNT, L"Desactivados", L"", v->sub < 0 });
        for (size_t i = 0; i < v->caps.size(); i++) rows.push_back({ I_COUNT, v->caps[i].name, L"", (int)i == v->sub });
    }
    return rows;
}
static float menu_head() { return g_menu_page > 1 ? S(57) : 0; }
static void paint_menu(Video *v) {
    if (!g_menu_page) { layer_hide(g_menu); return; }
    auto rows = menu_rows(v);
    RECT vr = video_rect();
    POINT o = { vr.left, vr.top }; ClientToScreen(g_win, &o);
    // as wide as its texts (YouTube's grows too), right-aligned over the progress bar
    float wl = 0, wv = 0;
    for (auto &r : rows) { wl = std::max(wl, text_width(f_menu.Get(), r.label)); wv = std::max(wv, text_width(f_menu.Get(), r.value)); }
    int w = g_menu_page == 1 ? (int)(S(8) + S(56) + wl + S(30) + wv + S(32) + S(8) + S(12)) : (int)(S(8) + S(35) + wl + S(20) + S(8) + S(12));
    w = std::max(w, (int)S(g_menu_page == 1 ? 300 : 250));
    int h = (int)(S(16) + menu_head() + rows.size() * S(48));
    int maxh = (vr.bottom - vr.top) - (int)S(62) - (int)S(12);
    if (h > maxh) h = maxh;
    int x = o.x + (vr.right - vr.left) - (int)S(12) - w, y = o.y + (vr.bottom - vr.top) - (int)S(62) - h;
    layer_place(g_menu, x, y, w, h, false);
    layer_show(g_menu, [&](ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b) {
        b->SetColor(rgba(0, 0, 0, 0.6f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, (float)w, (float)h), S(12), S(12)), b);
        float y0 = S(8);
        if (g_menu_page > 1) {
            static const wchar_t *titles[] = { L"", L"", L"Calidad", L"Velocidad de reproducción", L"Subtítulos" };
            if (g_menu_hot == -2) { b->SetColor(rgba(1, 1, 1, 0.1f)); rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(S(24), S(28)), S(16), S(16)), b); }
            draw_icon(rt, b, I_CHEV_L, S(24), S(28), S(24), kWhite);
            text(rt, b, f_head.Get(), titles[g_menu_page], D2D1::RectF(S(48), S(8), w - S(16), S(48)), kText);
            b->SetColor(rgba(1, 1, 1, 0.2f));
            rt->FillRectangle(D2D1::RectF(0, S(56), (float)w, S(57)), b);
            y0 = S(57) + S(8);
        }
        for (size_t i = 0; i < rows.size(); i++) {
            D2D1_RECT_F r = { S(8), y0 + i * S(48), w - S(8), y0 + (i + 1) * S(48) };
            if (r.bottom > h) break;
            if ((int)i == g_menu_hot) { b->SetColor(rgba(1, 1, 1, 0.1f)); rt->FillRoundedRectangle(D2D1::RoundedRect(r, S(8), S(8)), b); }
            float cy = (r.top + r.bottom) / 2;
            if (g_menu_page == 1) {
                draw_icon(rt, b, rows[i].icon, r.left + S(8) + S(12), cy, S(24), kWhite);
                text(rt, b, f_menu.Get(), rows[i].label, D2D1::RectF(r.left + S(56), r.top, r.right - S(30) - wv - S(32), r.bottom), kText);
                text(rt, b, f_menu_r.Get(), rows[i].value, D2D1::RectF(r.left + S(56), r.top, r.right - S(32), r.bottom), rgba(1, 1, 1, 0.7f));
                draw_icon(rt, b, I_CHEV_R, r.right - S(16), cy, S(20), rgba(1, 1, 1, 0.7f));
            } else {
                if (rows[i].on) draw_icon(rt, b, I_CHECK, r.left + S(19), cy, S(18), kWhite);
                text(rt, b, f_menu.Get(), rows[i].label, D2D1::RectF(r.left + S(35), r.top, r.right - S(20), r.bottom), kText);
            }
        }
    }, 1);
}
static void paint_center(Video *v) {
    double now = now_ms(), k = (now - v->bezel_at) / 500.0, kt = (now - v->vol_text_at) / 900.0;
    bool spin = v->buffering && !v->paused;
    if (k >= 1 && !spin && kt >= 1) { layer_hide(g_center); return; }
    RECT vr = video_rect();
    POINT o = { vr.left, vr.top }; ClientToScreen(g_win, &o);
    int sz = (int)S(200);
    layer_place(g_center, o.x + (vr.right - vr.left - sz) / 2, o.y + (vr.bottom - vr.top - sz) / 2, sz, sz, true);
    layer_show(g_center, [&](ID2D1RenderTarget *rt, ID2D1SolidColorBrush *b) {
        float c = sz / 2.0f;
        if (k < 1) {
            // YouTube's "bezel": a dark circle with the icon that grows to twice its size as it fades (0.5 s)
            float grow = 1 + (float)k, a = (float)(1 - k);
            b->SetColor(rgba(0, 0, 0, 0.6f * a));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), S(39) * grow, S(39) * grow), b);
            Icon ic = (Icon)v->bezel_icon;
            float box = (kIcons[ic].vb == 36 ? S(54) : S(36)) * grow;
            if (ic == I_SPEAKER) draw_volume_icon(rt, b, v, c, c, box, rgba(1, 1, 1, a));
            else draw_icon(rt, b, ic, c, c, box, rgba(1, 1, 1, a));
        } else if (spin) {
            ComPtr<ID2D1PathGeometry> g; ComPtr<ID2D1GeometrySink> sk;
            static ComPtr<ID2D1StrokeStyle> round;
            if (!round) g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), NULL, 0, &round);
            if (SUCCEEDED(g_d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                float r = S(28), a0 = (float)fmod(now / 1000.0, 1.0) * 6.2832f, a1 = a0 + 4.2f;
                sk->BeginFigure(D2D1::Point2F(c + r * cosf(a0), c + r * sinf(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(c + r * cosf(a1), c + r * sinf(a1)), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->Close();
                b->SetColor(kWhite);
                rt->DrawGeometry(g.Get(), b, S(5), round.Get());
            }
        }
        if (kt < 1) {
            // the volume, as a number, over the bezel
            wchar_t s[16]; swprintf_s(s, L"%d %%", v->muted ? 0 : (int)lround(v->volume));
            float a = kt < 0.7 ? 1.0f : (float)(1 - (kt - 0.7) / 0.3);
            D2D1_RECT_F r = { c - S(40), S(4), c + S(40), S(36) };
            b->SetColor(rgba(0, 0, 0, 0.5f * a));
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, S(16), S(16)), b);
            text(rt, b, f_big.Get(), s, r, rgba(1, 1, 1, a));
        }
    }, 1);
}
static void paint_all(Video *v) {
    if (!v || v->stage != ST_ON) return;
    Tab *t = active_tab();
    if (!t || V(t) != v || !IsWindowVisible(g_win) || IsIconic(g_win)) { hide_controls_now(); return; }
    make_fonts();
    RECT vr = video_rect();
    POINT o = { vr.left, vr.top }; ClientToScreen(g_win, &o);
    int w = vr.right - vr.left, h = vr.bottom - vr.top;
    double now = now_ms();
    float a = g_ctl.at(now);
    if (a <= 0.01f && !g_ctl.running(now)) { layer_hide(g_bottom); layer_hide(g_top); }
    else {
        layer_place(g_bottom, o.x, o.y + h - (int)S(kBottomH), w, (int)S(kBottomH), false);
        paint_bottom(v, a);
        if (!g_fullscreen) layer_hide(g_top);   // in the window, the tab already says what it is
        else { layer_place(g_top, o.x, o.y, w, (int)S(76), true); paint_top(v, a); }
    }
    paint_menu(v);
    paint_center(v);
}
static void CALLBACK anim_tick(HWND, UINT, UINT_PTR id, DWORD) {
    Video *v = V(active_tab());
    double now = now_ms();
    bool controls = g_ctl.running(now) || g_vol_open.running(now) || g_bar_hot.running(now);
    bool center = v && (now - v->bezel_at < 520 || now - v->vol_text_at < 920 || (v->buffering && !v->paused));
    if (v && controls) paint_all(v);
    else if (v && v->stage == ST_ON) paint_center(v);
    if (!controls && !center) KillTimer(g_msg, id);
}
static void animate() { SetTimer(g_msg, kAnimTimer, 15, anim_tick); }
static void caption_room(Video *v, bool controls) {
    // the captions go up over the controls while they show, and back down (as YouTube's); mpv's margin is in
    // pixels of a 720-line screen
    if (!v->mpv) return;
    RECT r = video_rect();
    double k = 720.0 / std::max(1L, r.bottom - r.top);
    int64_t m = (int64_t)lround((controls ? S(72) : S(30)) * k);
    M.set_property(v->mpv, "sub-margin-y", MPV_FORMAT_INT64, &m);
}
static void show_controls(Video *v) {
    v->last_move = now_ms();
    if (g_ctl.to < 1) { g_ctl.go(1, 100); animate(); caption_room(v, true); }
    paint_all(v);
}
static void hide_controls(Video *v) {
    if (g_ctl.to > 0) { g_ctl.go(0, 250); animate(); caption_room(v, false); }
    g_hot = C_NONE;
    // and the mouse, as YouTube hides it over a playing video
    POINT pt; GetCursorPos(&pt);
    if (v->wnd && WindowFromPoint(pt) == v->wnd) SetCursor(NULL);
}
static void bezel(Video *v, Icon ic) { v->bezel_icon = ic; v->bezel_at = now_ms(); animate(); }

// ---------------------------------------------------------------- actions (buttons, keys)
static void toggle_pause(Video *v) {
    if (v->eof) { mpv_cmd(v, { "seek", "0", "absolute+exact" }); set_flag(v, "pause", false); v->eof = false; bezel(v, I_PLAY); show_controls(v); return; }
    bezel(v, v->paused ? I_PLAY : I_PAUSE);
    set_flag(v, "pause", !v->paused);
}
static void seek_abs(Video *v, double t, bool exact) {
    char s[32]; sprintf_s(s, "%.3f", std::max(0.0, std::min(t, v->dur > 1 ? v->dur - 0.2 : t)));
    mpv_cmd(v, { "seek", s, exact ? "absolute+exact" : "absolute+keyframes" });
}
static void seek_rel(Video *v, double d) { seek_abs(v, v->pos + d, true); v->pos = std::max(0.0, v->pos + d); show_controls(v); }
static void set_volume(Video *v, double vol, bool from_key) {
    v->volume = std::max(0.0, std::min(100.0, vol));
    apply_volume(v);
    if (v->muted && v->volume > 0) set_flag(v, "mute", false);
    if (from_key) { v->vol_text_at = now_ms(); bezel(v, I_SPEAKER); }
    paint_all(v);
}
static void add_sub(Video *v) {
    // YouTube's captions as WebVTT (only once a file is loaded; a new file drops them). Pluma downloads them itself
    // and mpv reads them from disk, without waiting: mpv's own request took 10 s once, and a waiting command would
    // have held Pluma's window all that time (25/10)
    mpv_cmd(v, { "sub-remove" });
    if (v->sub < 0 || v->sub >= (int)v->caps.size()) return;
    Cap &c = v->caps[v->sub];
    if (!c.local.empty()) {
        const char *a[] = { "sub-add", c.local.c_str(), "select", NULL };
        M.command_async(v->mpv, 0, a);
        return;
    }
    std::string url = c.url;
    size_t f = url.find("&fmt=");
    if (f != std::string::npos) { size_t e = url.find('&', f + 1); url = url.substr(0, f) + (e == std::string::npos ? "" : url.substr(e)); }
    url += "&fmt=vtt";
    int id = v->tab, serial = v->serial, which = v->sub;
    std::wstring path = g_state + L"\\cache-video\\" + v->id + L"-sub" + std::to_wstring(which) + L".vtt";
    std::thread([url, id, serial, which, path] {
        std::string text;
        bool ok = http_get(wide(url), text) && write_file(path, text);
        ui_post([ok, id, serial, which, path] {
            Video *v = V(tab_by_id(id));
            if (!ok || !v || v->serial != serial || which >= (int)v->caps.size()) return;
            v->caps[which].local = utf8(path);
            if (v->sub == which && v->loaded) add_sub(v);
        });
    }).detach();
}
static void set_sub(Video *v, int i) {
    v->sub = i;
    if (v->loaded) add_sub(v);
    paint_all(v);
}
static void set_quality(Video *v, int h) {
    int f = h > 0 ? pick_video(v, h) : pick_video(v, target_height(v));
    v->auto_quality = h <= 0;
    if (f < 0 || f == v->vf) { paint_all(v); return; }
    // the new list first, while this one goes on playing; then the same second in the new quality
    fetch_playlists(v, { f }, [f](Video *v) {
        v->vf = f;
        int a = pick_audio(v);
        auto go = [](Video *v) { load_streams(v, v->pos, v->paused); paint_all(v); };
        if (a != v->af) { v->af = a; fetch_playlists(v, { a }, go); }
        else go(v);
    });
    paint_all(v);
}
static void toggle_mute(Video *v) { bezel(v, v->muted ? I_SPEAKER : I_MUTED); set_flag(v, "mute", !v->muted); v->muted = !v->muted; paint_all(v); }
static bool video_key(Video *v, UINT vk, bool ctrl, bool shift, bool alt) {
    if (ctrl || alt) return false;
    switch (vk) {
    case VK_SPACE: case 'K': toggle_pause(v); return true;
    case 'J': seek_rel(v, -10); return true;
    case 'L': seek_rel(v, 10); return true;
    case VK_LEFT: seek_rel(v, -5); return true;
    case VK_RIGHT: seek_rel(v, 5); return true;
    case VK_UP: set_volume(v, v->volume + 5, true); return true;
    case VK_DOWN: set_volume(v, v->volume - 5, true); return true;
    case 'M': toggle_mute(v); return true;
    case 'F': set_fullscreen(!g_fullscreen); return true;
    case 'C': if (!v->caps.empty()) set_sub(v, v->sub >= 0 ? -1 : 0); return true;
    case VK_HOME: seek_abs(v, 0, true); show_controls(v); return true;
    case VK_END: seek_abs(v, v->dur, true); show_controls(v); return true;
    case VK_ESCAPE:
        if (g_menu_page) { g_menu_page = 0; paint_all(v); return true; }
        if (g_fullscreen) { set_fullscreen(false); return true; }
        return false;
    }
    if (!shift && ((vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9))) {
        int d = vk >= VK_NUMPAD0 ? (int)(vk - VK_NUMPAD0) : (int)(vk - '0');
        seek_abs(v, v->dur * d / 10.0, true);
        show_controls(v);
        return true;
    }
    return false;
}
static void key_char(Video *v, wchar_t c) {
    // YouTube's speed keys are characters (Shift+, and Shift+. on its keyboard; < and > on a Spanish one)
    if (c != L'<' && c != L'>') return;
    static const double steps[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 };
    int i = 0;
    while (i < 7 && steps[i] < v->speed - 0.01) i++;
    i = std::max(0, std::min(7, i + (c == L'>' ? 1 : -1)));
    v->speed = steps[i];
    set_double(v, "speed", v->speed);
    show_controls(v);
}

// ---------------------------------------------------------------- mpv's events
static void on_property(Video *v, uint64_t code, mpv_event_property *p) {
    if (!p->data) return;
    double d = p->format == MPV_FORMAT_DOUBLE ? *(double *)p->data : 0;
    bool f = p->format == MPV_FORMAT_FLAG ? *(int *)p->data != 0 : false;
    Tab *t = tab_by_id(v->tab);
    switch (code) {
    case 0: {
        bool repaint = fabs(d - v->pos) >= 0.25 || (long long)d != (long long)v->pos;
        v->pos = d;
        if (repaint && v->stage == ST_ON && g_ctl.at(now_ms()) > 0) paint_all(v);
        break;
    }
    case 1: if (d > 0) v->dur = d; break;
    case 2:
        v->paused = f;
        if (v->stage == ST_ON) {
            if (t) { t->audible = !f && !v->muted; ui_invalidate(); }
            if (f) show_controls(v); else paint_all(v);
        }
        break;
    case 3:
        v->eof = f;
        if (f && v->stage == ST_ON && t) {
            // the end: back to the page, which shows what YouTube shows there (and plays the next one if it does).
            // An end well before the end is the stream failing: the page takes over at that second
            if (v->dur > 0 && v->pos < v->dur - 3) PostMessageW(g_msg, WM_VIDEO_FAILED, VIDEO_FAILED, (LPARAM)v->tab);
            else PostMessageW(g_msg, WM_VIDEO_FAILED, VIDEO_ENDED, (LPARAM)v->tab);
        }
        break;
    case 4: v->buffering = f; if (f) animate(); else if (v->stage == ST_ON) paint_center(v); break;
    case 5: v->cached = std::max(0.0, d - v->pos); break;
    case 6: v->muted = f; if (t && v->stage == ST_ON) { t->audible = !v->paused && !f; ui_invalidate(); } break;
    case 7: if (d > 0) v->speed = d; break;
    }
}
static void sync_tick(Tab *t, Video *v);
static LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_VIDEO_EVENTS) {
        Tab *t = tab_by_id((int)lp);
        Video *v = V(t);
        if (!v || !v->mpv) return 0;
        while (v->mpv) {
            mpv_event *e = M.wait_event(v->mpv, 0);
            if (!e || e->event_id == MPV_EVENT_NONE) break;
            if (e->event_id == MPV_EVENT_PROPERTY_CHANGE) on_property(v, e->reply_userdata, (mpv_event_property *)e->data);
            else if (e->event_id == MPV_EVENT_PLAYBACK_RESTART) { v->restarted = true; tame(v); if (v->stage == ST_WAITING) sync_tick(t, v); }
            else if (e->event_id == MPV_EVENT_FILE_LOADED) { v->loaded = true; tame(v); if (v->sub >= 0) add_sub(v); }
            else if (e->event_id == MPV_EVENT_END_FILE) {
                auto *ef = (mpv_event_end_file *)e->data;
                if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                    // the streams did not play (YouTube refused them): the page goes on as if nothing had happened
                    log_line(L"vídeo propio: mpv no pudo con las direcciones (error " + std::to_wstring(ef->error) + L")");
                    PostMessageW(g_msg, WM_VIDEO_FAILED, VIDEO_FAILED, (LPARAM)v->tab);
                    break;
                }
            }
        }
        return 0;
    }
    if (msg == WM_VIDEO_FAILED) { Tab *t = tab_by_id((int)lp); if (t && V(t)) video_back_to_page(t, (int)wp); return 0; }
    if (msg == WM_VIDEO_GONE) { DestroyWindow((HWND)wp); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- the mouse: over the video, over the controls
static int hit(Video *v, float x, float y) {
    Layer &l = g_bottom;
    if (in(bar_hit(l), x, y)) return C_BAR;
    if (in(play_rect(l), x, y)) return C_PLAY;
    if (in(mute_rect(l), x, y)) return C_MUTE;
    if (g_vol_open.at(now_ms()) > 0.5f && in(vol_slider(l), x, y)) return C_VOLBAR;
    if (in(right_btn(l, 0), x, y)) return C_FULL;
    if (in(right_btn(l, 1), x, y)) return C_PAGE;
    if (in(right_btn(l, 2), x, y)) return C_GEAR;
    if (!v->caps.empty() && in(right_btn(l, 3), x, y)) return C_CC;
    return C_NONE;
}
static double bar_time(Video *v, float x) {
    float x0 = S(12), x1 = g_bottom.w - S(12);
    return std::max(0.0, std::min(1.0, (double)(x - x0) / std::max(1.0f, x1 - x0))) * v->dur;
}
static double vol_at(float x) {
    D2D1_RECT_F sl = vol_slider(g_bottom);
    return std::max(0.0, std::min(100.0, (x - sl.left - S(6)) / (S(52) - S(12)) * 100.0));
}
static void menu_click(Video *v, int row) {
    if (row == -2) { g_menu_page = 1; return; }   // the header: back
    if (row < 0) return;
    if (g_menu_page == 1) {
        int which = row + (v->caps.empty() ? 1 : 0);   // 0 captions, 1 speed, 2 quality
        g_menu_page = which == 0 ? 4 : which == 1 ? 3 : 2;
    } else if (g_menu_page == 2) {
        auto rows = menu_rows(v);
        if (row < (int)rows.size()) set_quality(v, row == (int)rows.size() - 1 ? 0 : _wtoi(rows[row].label.c_str()));
        g_menu_page = 0;
    } else if (g_menu_page == 3) {
        static const double sp[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 };
        if (row < 8) { v->speed = sp[row]; set_double(v, "speed", v->speed); }
        g_menu_page = 0;
    } else if (g_menu_page == 4) {
        set_sub(v, row - 1);
        g_menu_page = 0;
    }
}
static void track_leave(HWND h) {
    // (not in tests: they move the mouse with messages, and Windows, seeing the real one elsewhere, says it left)
    if (g_test) return;
    TRACKMOUSEEVENT tm = { sizeof(tm), TME_LEAVE, h, 0 };
    TrackMouseEvent(&tm);
}
static void focus_video(Video *v) { if (v && v->wnd && GetForegroundWindow() == g_win) SetFocus(v->wnd); }
static bool cursor_moved(HWND h, LPARAM lp) {
    // Windows also sends a "mouse moved" when a window appears or goes under a still mouse: that is no movement
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ClientToScreen(h, &pt);
    if (pt.x == g_last_cursor.x && pt.y == g_last_cursor.y) return false;
    g_last_cursor = pt;
    return true;
}
static LRESULT CALLBACK layer_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    Video *v = V(active_tab());
    if (!v || v->stage != ST_ON) return DefWindowProcW(h, msg, wp, lp);
    float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
    if (msg == WM_SETCURSOR) {
        bool hand = h == g_menu.h ? g_menu_hot != -1 : (g_hot != C_NONE);
        SetCursor(LoadCursorW(NULL, hand ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    if (h == g_menu.h) {
        float y0 = S(8) + menu_head();
        int row = g_menu_page > 1 && y < S(57) ? -2 : y < y0 ? -1 : (int)((y - y0) / S(48));
        if (msg == WM_MOUSEMOVE) {
            if (row != g_menu_hot) { g_menu_hot = row; paint_menu(v); }
            track_leave(h);
            v->last_move = now_ms();
        } else if (msg == WM_MOUSELEAVE) { g_menu_hot = -1; paint_menu(v); }
        else if (msg == WM_LBUTTONUP) { menu_click(v, row); g_menu_hot = -1; paint_all(v); }
        return 0;
    }
    if (h != g_bottom.h) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEMOVE: {
        track_leave(h);
        bool moved = cursor_moved(h, lp);
        int c = hit(v, x, y);
        if (g_drag_seek) { g_mouse_x = x; v->pos = bar_time(v, x); seek_abs(v, v->pos, false); }
        else if (g_drag_vol) set_volume(v, vol_at(x), false);
        bool vol_zone = c == C_MUTE || c == C_VOLBAR;
        if (vol_zone && g_vol_open.to < 1) { g_vol_open.go(1, 200); animate(); }
        if (!vol_zone && !g_drag_vol && g_vol_open.to > 0) { g_vol_open.go(0, 200); animate(); }
        bool bar = c == C_BAR || g_drag_seek;
        if (bar && g_bar_hot.to < 1) { g_bar_hot.go(1, 200); animate(); }
        if (!bar && g_bar_hot.to > 0) { g_bar_hot.go(0, 200); animate(); }
        g_hot = c;
        g_mouse_x = bar ? x : -1;
        if (moved || g_drag_seek || g_drag_vol) show_controls(v); else paint_all(v);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!g_drag_seek && !g_drag_vol) {
            g_hot = C_NONE; g_mouse_x = -1;
            if (g_vol_open.to > 0) g_vol_open.go(0, 200);
            if (g_bar_hot.to > 0) g_bar_hot.go(0, 200);
            animate();
            paint_all(v);
        }
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        focus_video(v);
        int c = hit(v, x, y);
        if (c == C_BAR) { g_drag_seek = true; SetCapture(h); g_mouse_x = x; v->pos = bar_time(v, x); seek_abs(v, v->pos, false); paint_all(v); }
        else if (c == C_VOLBAR) { g_drag_vol = true; SetCapture(h); set_volume(v, vol_at(x), false); }
        else if (c == C_NONE && msg == WM_LBUTTONDBLCLK) { toggle_pause(v); set_fullscreen(!g_fullscreen); }
        return 0;
    }
    case WM_LBUTTONUP: {
        int c = hit(v, x, y);
        if (g_drag_seek) { g_drag_seek = false; ReleaseCapture(); v->pos = bar_time(v, x); seek_abs(v, v->pos, true); paint_all(v); return 0; }
        if (g_drag_vol) { g_drag_vol = false; ReleaseCapture(); return 0; }
        if (c != C_GEAR && g_menu_page) { g_menu_page = 0; paint_all(v); if (c == C_NONE) return 0; }
        if (c == C_PLAY) toggle_pause(v);
        else if (c == C_MUTE) toggle_mute(v);
        else if (c == C_FULL) set_fullscreen(!g_fullscreen);
        else if (c == C_PAGE) { video_back_to_page(active_tab(), VIDEO_ASKED); return 0; }
        else if (c == C_GEAR) { g_menu_page = g_menu_page ? 0 : 1; g_menu_hot = -1; }
        else if (c == C_CC) set_sub(v, v->sub >= 0 ? -1 : 0);
        else if (c == C_NONE) toggle_pause(v);
        paint_all(v);
        return 0;
    }
    case WM_CAPTURECHANGED: g_drag_seek = g_drag_vol = false; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static LRESULT CALLBACK video_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Tab *t = tab_by_id((int)GetWindowLongPtrW(h, GWLP_USERDATA));
    Video *v = V(t);
    bool on = v && v->stage == ST_ON && v->wnd == h;
    switch (msg) {
    case WM_ERASEBKGND: { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_SETCURSOR:
        if (on && LOWORD(lp) == HTCLIENT) { SetCursor(g_ctl.to > 0 || v->paused ? LoadCursorW(NULL, IDC_ARROW) : NULL); return TRUE; }
        break;
    case WM_MOUSEMOVE:
        if (on && cursor_moved(h, lp)) show_controls(v);
        return 0;
    case WM_LBUTTONDOWN:
        if (on) { SetFocus(h); if (g_menu_page) { g_menu_page = 0; paint_all(v); SetPropW(h, L"menu", (HANDLE)1); } }
        return 0;
    case WM_LBUTTONUP:
        // a click pauses or plays; the two clicks of a double click do both, and the double click is fullscreen (as YouTube)
        if (on) { if (GetPropW(h, L"menu")) RemovePropW(h, L"menu"); else toggle_pause(v); }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (on) set_fullscreen(!g_fullscreen);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (on && video_key(v, (UINT)wp, ctrl, shift, alt)) return 0;
        if (handle_shortcut((UINT)wp, ctrl, shift, alt)) return 0;
        break;
    }
    case WM_CHAR: if (on) key_char(v, (wchar_t)wp); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void CALLBACK ctl_tick(HWND, UINT, UINT_PTR id, DWORD) {
    // the controls go a moment after the mouse stops (not while paused, nor over them), and at once when it leaves
    Video *v = V(active_tab());
    if (!v || v->stage != ST_ON) { KillTimer(g_msg, id); return; }
    // nothing of the web is on screen: its processes, asleep, give back what they take again (149 MB in all
    // watching, 123 with this; measured 25/10). On a thread of its own, and never the player's process
    static double last_trim = 0;
    if (now_ms() - last_trim > 20000 && active_tab()->suspended) { last_trim = now_ms(); tabs_trim(TRIM_IDLE); }
    if (g_ctl.to <= 0 || v->paused || v->eof || g_menu_page || g_drag_seek || g_drag_vol) return;
    POINT pt;
    if (g_test) pt = g_last_cursor;   // tests move the mouse with messages: it rests where they left it
    else GetCursorPos(&pt);
    HWND under = WindowFromPoint(pt);
    bool over_controls = under == g_bottom.h || under == g_menu.h;
    bool over_video = under == v->wnd || (under && GetParent(under) == v->wnd);
    double idle = now_ms() - v->last_move;
    if (over_controls) return;
    if (idle > 2600 || (!over_video && idle > 600)) hide_controls(v);
}

// ---------------------------------------------------------------- taking over from the page, and back
static void eval(Tab *t, const std::wstring &js, std::function<void(const Json &)> done) {
    // Runtime.evaluate through WebView2's DevTools channel: waits for promises, gives back plain values
    if (!t || !t->view) return;
    std::wstring params = L"{\"expression\":" + json_escape(js) + L",\"awaitPromise\":true,\"returnByValue\":true}";
    int id = t->id;
    t->view->CallDevToolsProtocolMethod(L"Runtime.evaluate", params.c_str(), Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
        [id, done](HRESULT hr, LPCWSTR json) -> HRESULT {
            if (!tab_by_id(id)) return S_OK;
            Json r;
            if (SUCCEEDED(hr) && json) r = json_parse(json)[L"result"][L"value"];
            done(r);
            return S_OK;
        }).Get());
}
// 1 when the page's own player is playing the video itself (not an ad), near the top of the page
static const wchar_t *kState = LR"JS((() => {
  const p = document.getElementById('movie_player');
  if (!p || !p.getPlayerState) return 0;
  if (p.classList.contains('ad-showing') || p.classList.contains('ad-interrupting')) return 2;
  if (scrollY > 300) return 3;
  return p.getPlayerState() === 1 && p.getCurrentTime() > 0.3 ? 1 : 0;
})())JS";
static const wchar_t *kExtract = LR"JS((async () => {
  const id = new URLSearchParams(location.search).get('v');
  const p = document.getElementById('movie_player');
  if (!id || !window.ytcfg || !p || !p.getCurrentTime) return { error: 'sin reproductor' };
  const ctx = ytcfg.get('INNERTUBE_CONTEXT') || {}, c0 = ctx.client || {}, visitor = c0.visitorData;
  const client = { clientName: 'VISIONOS', clientVersion: '1.02', deviceMake: 'Apple', deviceModel: 'RealityDevice17,1',
                   osName: 'visionOS', osVersion: '26.5.23O471', hl: c0.hl || 'es', gl: c0.gl || 'ES', visitorData: visitor };
  const ask = async (credentials) => {
    const r = await fetch('/youtubei/v1/player?prettyPrint=false', { method: 'POST', credentials,
      headers: { 'Content-Type': 'application/json', 'X-YouTube-Client-Name': '101', 'X-YouTube-Client-Version': '1.02', 'X-Goog-Visitor-Id': visitor || '' },
      body: JSON.stringify({ context: { client }, videoId: id, contentCheckOk: true, racyCheckOk: true }) });
    return r.json();
  };
  let j = await ask('include');
  if ((j.playabilityStatus || {}).status !== 'OK') j = await ask('omit');
  const ps = j.playabilityStatus || {}, vd = j.videoDetails || {};
  if (ps.status !== 'OK') return { error: ps.status || 'sin estado' };
  if (vd.isLive || vd.isPostLiveDvr) return { error: 'directo' };
  // its HLS list: small pieces, each a request of its own (its direct addresses stop after ~20 MB without a token)
  const hls = (j.streamingData || {}).hlsManifestUrl || '';
  if (!hls) return { error: 'sin lista HLS' };
  const tracks = (((j.captions || {}).playerCaptionsTracklistRenderer || {}).captionTracks || []).map((t) => ({
    url: t.baseUrl, lang: t.languageCode || '',
    name: (t.name && (t.name.simpleText || (t.name.runs || []).map((x) => x.text).join(''))) || t.languageCode }));
  const el = document.querySelector('#movie_player video');
  const cc = (p.getOption && p.getOption('captions', 'track')) || {};
  return { id, title: vd.title || document.title, author: vd.author || '', length: +vd.lengthSeconds || 0, hls, tracks,
           time: p.getCurrentTime(), state: p.getPlayerState(), volume: p.getVolume ? p.getVolume() : 100,
           muted: p.isMuted ? p.isMuted() : false, gain: el ? el.volume : 1, speed: p.getPlaybackRate ? p.getPlaybackRate() : 1,
           cc: cc.languageCode || '' };
})())JS";
static void freeze_page(int id) {
    // the page, paused where it was, freezes and gives its memory back. (Not stopVideo: after it YouTube's player
    // ignores the seek back and starts again from 0; seen 25/10)
    Tab *t = tab_by_id(id);
    if (!t || !t->video_mode || !t->view) return;
    eval(t, L"(() => { const p = document.getElementById('movie_player'); if (p && p.pauseVideo) p.pauseVideo(); return 1; })()", [id](const Json &) {
        Tab *t = tab_by_id(id);
        if (!t || !t->video_mode || !t->view) return;
        ComPtr<ICoreWebView2_19> v19;
        if (SUCCEEDED(t->view.As(&v19))) v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
        ComPtr<ICoreWebView2_3> v3;
        if (SUCCEEDED(t->view.As(&v3)))
            v3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>([id](HRESULT hr, BOOL ok) -> HRESULT {
                Tab *t = tab_by_id(id);
                // with nothing of the web on screen, the engine and its graphics give back what they hold too
                if (t && t->video_mode && SUCCEEDED(hr) && ok) { t->suspended = true; tabs_trim(TRIM_IDLE); }
                return S_OK;
            }).Get());
    });
}
static void place_window(Video *v, bool on_screen) {
    if (!v->wnd) return;
    if (!on_screen) { ShowWindow(v->wnd, SW_HIDE); return; }
    RECT r = video_rect();
    SetWindowPos(v->wnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetWindowPos(g_bar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static void switch_now(Tab *t, Video *v, bool playing) {
    // the same instant: the page pauses, mpv plays, the player comes up over the page
    eval(t, L"(() => { const p = document.getElementById('movie_player'); if (p && p.pauseVideo) p.pauseVideo();"
            L" if (document.fullscreenElement) document.exitFullscreen(); return 1; })()", [](const Json &) {});
    v->stage = ST_ON;
    t->video_mode = true;
    if (playing) set_flag(v, "pause", false);
    v->paused = !playing;
    t->audible = playing && !v->muted;
    if (t == active_tab()) {
        place_window(v, IsWindowVisible(g_win) != FALSE);
        if (t->ctrl) t->ctrl->put_IsVisible(FALSE);
        tame(v);
        focus_video(v);
        g_ctl.snap(0);
        show_controls(v);
    }
    SetTimer(g_msg, kCtlTimer, 250, ctl_tick);
    log_line(L"vídeo propio: " + v->title + L" a " + std::to_wstring(v->vf >= 0 ? v->fmts[v->vf].h : 0) + L"p " + (v->vf >= 0 ? v->fmts[v->vf].codec : L"") +
             L", " + std::to_wstring((int)(now_ms() - v->asked)) + L" ms desde que se pidió");
    SetTimer(g_msg, kFreezeTimer + t->id, 2500, [](HWND, UINT, UINT_PTR tid, DWORD) { KillTimer(g_msg, tid); freeze_page((int)(tid - kFreezeTimer)); });
    // the other qualities' lists, ahead (YouTube's list server takes 1-3 s for each)
    fetch_playlists(v, each_quality(v), [](Video *) {});
    ui_invalidate();
}
static void CALLBACK sync_timer(HWND, UINT, UINT_PTR tid, DWORD) {
    KillTimer(g_msg, tid);
    Tab *t = tab_by_id((int)(tid - kSyncTimer));
    if (V(t)) sync_tick(t, V(t));
}
static void sync_tick(Tab *t, Video *v) {
    // mpv waits, paused, a second ahead of the page; when the page gets there, the two swap
    if (!t || v->stage != ST_WAITING || !v->restarted) return;
    int id = t->id;
    eval(t, L"(() => { const p = document.getElementById('movie_player'); return p && p.getCurrentTime ? [p.getCurrentTime(), p.getPlayerState()] : null; })()",
         [id](const Json &r) {
             Tab *t = tab_by_id(id);
             Video *v = V(t);
             if (!v || v->stage != ST_WAITING) return;
             if (r.kind != Json::ARR || r.arr.size() < 2) { video_destroy(t); return; }
             double pt = r.arr[0].n;
             int st = (int)r.arr[1].n;
             if (st == 0) { video_destroy(t); return; }   // the video ended meanwhile
             if (st == 2) {
                 // paused on the page: take over, paused at its second
                 seek_abs(v, pt, true);
                 v->pos = pt;
                 switch_now(t, v, false);
                 return;
             }
             double gap = v->start_at - pt;
             if (st == 1 && gap <= 0.04) {
                 if (gap < -0.35) {
                     // mpv was late: ahead again (a seek within the lists already read takes under a second)
                     v->start_at = pt + 1.5 * v->speed; v->restarted = false; seek_abs(v, v->start_at, true);
                     if (g_dev) log_line(L"vídeo propio: mpv llegó tarde, otra vez por delante");
                     return;
                 }
                 switch_now(t, v, true);
                 return;
             }
             // close (or the page is loading): look again just before the moment
             int wait = st == 1 ? (int)std::max(8.0, std::min(200.0, gap * 1000 - 25)) : 150;
             SetTimer(g_msg, kSyncTimer + id, wait, sync_timer);
         });
}
static std::wstring watch_id(const std::wstring &url) {
    size_t q = url.find(L"youtube.com/watch");
    if (q == std::wstring::npos || q > 20) return L"";
    size_t v = url.find(L"v=", q);
    while (v != std::wstring::npos && url[v - 1] != L'?' && url[v - 1] != L'&') v = url.find(L"v=", v + 2);
    if (v == std::wstring::npos) return L"";
    std::wstring id = url.substr(v + 2);
    return id.substr(0, id.find_first_of(L"&#"));
}
static std::map<int, double> g_watch;   // tabs on a YouTube video, waiting for it to play: until when
static void watch(Tab *t);
static void video_try(Tab *t) {
    if (!t || !t->view || t->video || !g_set.native_video) return;
    std::wstring vid = watch_id(t->url);
    if (vid.empty() || vid == t->video_declined || !load_mpv()) return;
    auto *v = new Video();
    v->tab = t->id;
    v->id = vid;
    v->asked = now_ms();
    t->video = v;
    int id = t->id;
    eval(t, kExtract, [id](const Json &r) {
        Tab *t = tab_by_id(id);
        Video *v = V(t);
        if (!v || v->stage != ST_WAITING || v->mpv) return;
        auto give_up = [&](const std::wstring &why) { log_line(L"vídeo propio: no (" + why + L")"); t->video = nullptr; delete v; };
        if (r.kind != Json::OBJ) { give_up(L"sin respuesta"); return; }
        if (r[L"error"].is_str()) { give_up(r[L"error"].s); return; }
        if (r[L"id"].s != v->id) { give_up(L"la página ya está en otro vídeo"); return; }
        v->title = r[L"title"].s; v->author = r[L"author"].s; v->dur = r[L"length"].n;
        for (auto &c : r[L"tracks"].arr)
            if (google_url(utf8(c[L"url"].s), true)) v->caps.push_back({ utf8(c[L"url"].s), "", c[L"name"].s, c[L"lang"].s });
        v->volume = r[L"volume"].kind == Json::NUM ? r[L"volume"].n : 100;
        v->muted = r[L"muted"].b;
        v->speed = r[L"speed"].kind == Json::NUM && r[L"speed"].n > 0 ? r[L"speed"].n : 1;
        double gain = r[L"gain"].kind == Json::NUM ? r[L"gain"].n : 1;
        v->gain_db = v->volume > 0 && gain > 0 ? std::max(-30.0, std::min(0.0, 20 * log10(gain / (v->volume / 100.0)))) : 0;
        std::wstring cc = r[L"cc"].s;
        if (!cc.empty()) for (size_t i = 0; i < v->caps.size(); i++) if (v->caps[i].lang == cc) { v->sub = (int)i; break; }   // (added once loaded)
        double pt = r[L"time"].n, at = now_ms();
        std::wstring hls = r[L"hls"].s;
        if (!google_url(utf8(hls))) { give_up(L"la lista no es de los servidores de Google"); return; }
        int serial = v->serial = ++g_serial;
        // the HLS list, from here (the page may not read it), then the player
        std::thread([id, serial, hls, pt, at] {
            std::string m3u8;
            bool ok = http_get(hls, m3u8);
            ui_post([id, serial, ok, m3u8, pt, at] {
                Tab *t = tab_by_id(id);
                Video *v = V(t);
                if (!v || v->serial != serial || v->stage != ST_WAITING || v->mpv) return;
                auto give_up = [&](const std::wstring &why) { log_line(L"vídeo propio: no (" + why + L")"); t->video = nullptr; delete v; };
                if (!ok) { give_up(L"no se pudo leer la lista HLS"); return; }
                v->fmts = parse_master(m3u8);
                v->vf = pick_video(v, target_height(v));
                v->af = pick_audio(v);
                if (v->vf < 0 || v->af < 0) { give_up(L"no hay formatos que valgan"); return; }
                fetch_playlists(v, { v->vf, v->af }, [pt, at](Video *v) {
                    if (v->stage != ST_WAITING || v->mpv) return;
                    // ahead of where the page is now: mpv takes ~1.5 s to start with the lists already here
                    v->start_at = pt + ((now_ms() - at) / 1000.0 + 2.0) * v->speed;
                    if (!start_player(v, v->start_at)) {
                        log_line(L"vídeo propio: no arrancó el motor");
                        Tab *t = tab_by_id(v->tab);
                        stop_player(v); if (t) t->video = nullptr; delete v;
                        return;
                    }
                    log_line(L"vídeo propio: listas en " + std::to_wstring((int)(now_ms() - v->asked)) + L" ms");
                });
            });
        }).detach();
    });
}
static void CALLBACK watch_tick(HWND, UINT, UINT_PTR tid, DWORD) {
    KillTimer(g_msg, tid);
    int id = (int)(tid - kWatchTimer);
    Tab *t = tab_by_id(id);
    auto it = g_watch.find(id);
    if (it == g_watch.end()) return;
    if (!t || !t->view || t->video || !g_set.native_video || now_ms() > it->second || watch_id(t->url).empty()) { g_watch.erase(it); return; }
    if (t != active_tab() || t->suspended || !IsWindowVisible(g_win)) { SetTimer(g_msg, tid, 1000, watch_tick); return; }
    eval(t, kState, [id](const Json &r) {
        Tab *t = tab_by_id(id);
        if (!t || !g_watch.count(id)) return;
        if (r.kind == Json::NUM && r.n == 1) { g_watch.erase(id); video_try(t); return; }
        SetTimer(g_msg, kWatchTimer + id, 400, watch_tick);
    });
}
static void watch(Tab *t) {
    g_watch[t->id] = now_ms() + 60000;
    SetTimer(g_msg, kWatchTimer + t->id, 400, watch_tick);
}
void video_page_changed(Tab *t) {
    // the tab's address changed (YouTube moves between videos without loading a page), or it came on screen
    if (!t || !g_msg) return;
    std::wstring id = watch_id(t->url);
    Video *v = V(t);
    if (v && v->id != id && v->stage == ST_WAITING) video_destroy(t);
    if (id.empty() || t->video || !g_set.native_video || !video_available() || id == t->video_declined) return;
    if (!g_watch.count(t->id)) watch(t);
}
void video_back_to_page(Tab *t, int how) {
    Video *v = V(t);
    if (!t || !v) return;
    double pos = v->pos, vol = v->volume, speed = v->speed;
    bool was_on = v->stage == ST_ON, playing = was_on && !v->paused, muted = v->muted;
    if (how == VIDEO_ASKED || how == VIDEO_FAILED) t->video_declined = v->id;
    KillTimer(g_msg, kFreezeTimer + t->id);
    KillTimer(g_msg, kSyncTimer + t->id);
    stop_player(v);
    if (was_on) hide_controls_now();
    t->video = nullptr;
    t->video_mode = false;
    delete v;
    if (!t->view) return;
    if (was_on) {
        if (t->suspended) { ComPtr<ICoreWebView2_3> v3; if (SUCCEEDED(t->view.As(&v3))) v3->Resume(); t->suspended = false; }
        ComPtr<ICoreWebView2_19> v19;
        if (SUCCEEDED(t->view.As(&v19))) v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        if (how != VIDEO_LEAVE) {
            // the page's player where Pluma's was: same second, volume, speed, playing or not
            // (at the end, only the seek: the page then shows its end screen, and plays the next video if it would)
            wchar_t js[400];
            swprintf_s(js, L"(() => { const p = document.getElementById('movie_player'); if (!p || !p.seekTo) return 0;"
                           L" p.setVolume(%d); %s p.setPlaybackRate(%g); p.seekTo(%.2f, true); %s return 1; })()",
                       (int)lround(vol), muted ? L"p.mute();" : L"p.unMute();", speed, how == VIDEO_ENDED ? std::max(0.0, pos) + 5 : pos,
                       how == VIDEO_ENDED ? L"" : playing || how == VIDEO_FAILED ? L"p.playVideo();" : L"p.pauseVideo();");
            eval(t, js, [](const Json &) {});
        }
        t->audible = false;
        if (t == active_tab()) tab_activate(g_active);
    }
    ui_invalidate();
}
void video_destroy(Tab *t) {
    // the tab goes, or its page: the player goes with it (without touching the page)
    Video *v = V(t);
    if (!v) return;
    if (v->stage == ST_ON) t->saved_media = v->pos;
    KillTimer(g_msg, kFreezeTimer + t->id);
    KillTimer(g_msg, kSyncTimer + t->id);
    bool was_on = v->stage == ST_ON;
    stop_player(v);
    if (was_on && (t == active_tab())) hide_controls_now();
    t->video = nullptr;
    t->video_mode = false;
    if (was_on) t->audible = false;
    delete v;
}
void video_layout() {
    // only the tab on screen shows its player; the others keep it out of sight (and playing, if they are)
    Tab *act = active_tab();
    bool visible = IsWindowVisible(g_win) && !IsIconic(g_win);
    for (auto &up : g_tabs) {
        Video *v = V(up.get());
        if (!v || !v->wnd) continue;
        if (v->stage == ST_ON) place_window(v, up.get() == act && visible);
        else if (up.get() != act) ShowWindow(v->wnd, SW_HIDE);
        else {
            RECT r = video_rect();
            SetWindowPos(v->wnd, HWND_BOTTOM, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
    Video *v = V(act);
    if (v && v->stage == ST_ON && visible) { paint_all(v); SetTimer(g_msg, kCtlTimer, 250, ctl_tick); }
    else hide_controls_now();
}
void video_debug_shot(const std::wstring &path) {
    // tests: the window as the screen shows it, the player's controls blended in with their real transparency (a
    // picture taken from outside gets the layered windows opaque)
    RECT rc; GetClientRect(g_win, &rc);
    int w = rc.right, h = rc.bottom;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(g_win, mdc, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mdc, old);
    POINT o = { 0, 0 }; ClientToScreen(g_win, &o);
    for (Layer *l : { &g_bottom, &g_top, &g_center, &g_menu }) {
        if (!l->shown || !l->bits) continue;
        for (int y = 0; y < l->hh; y++)
            for (int x = 0; x < l->w; x++) {
                int dx = l->pos.x - o.x + x, dy = l->pos.y - o.y + y;
                if (dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
                BYTE *s = (BYTE *)l->bits + (y * l->w + x) * 4, *d = (BYTE *)bits + (dy * w + dx) * 4;
                float a = s[3] / 255.0f * l->alpha;
                for (int k = 0; k < 3; k++) d[k] = (BYTE)std::min(255.0f, s[k] * l->alpha + d[k] * (1 - a));
            }
    }
    for (int i = 0; i < w * h; i++) ((BYTE *)bits)[i * 4 + 3] = 255;
    ComPtr<IWICImagingFactory> wic; ComPtr<IWICBitmap> wb; ComPtr<IWICStream> st; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> fr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4, (BYTE *)bits, &wb)) &&
        SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) && SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) && SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->SetSize(w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&fmt);
        fr->WriteSource(wb.Get(), NULL);
        fr->Commit(); enc->Commit();
    }
    DeleteObject(bmp); DeleteDC(mdc);
}
bool video_on(Tab *t) { Video *v = V(t); return v && v->stage == ST_ON; }
double video_position(Tab *t) { Video *v = V(t); return v ? v->pos : 0; }
bool video_focus() { Video *v = V(active_tab()); if (!v || v->stage != ST_ON || !v->wnd) return false; SetFocus(v->wnd); return true; }
void video_toggle_mute(Tab *t) { Video *v = V(t); if (v && v->stage == ST_ON) toggle_mute(v); }
void video_settings_changed() {
    // the setting was turned off: every video goes back to its page; on: the one on screen is looked at
    if (!g_set.native_video) {
        for (auto &up : g_tabs) if (up->video) video_back_to_page(up.get(), VIDEO_ASKED);
    } else if (active_tab()) video_page_changed(active_tab());
}
void video_init() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = msg_proc; wc.hInstance = g_inst; wc.lpszClassName = L"PlumaVideoMensajes";
    RegisterClassExW(&wc);
    g_msg = CreateWindowExW(0, L"PlumaVideoMensajes", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL);
    WNDCLASSEXW vw = { sizeof(vw) };
    vw.style = CS_DBLCLKS;
    vw.lpfnWndProc = video_wnd_proc; vw.hInstance = g_inst; vw.lpszClassName = L"PlumaVideo";
    vw.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&vw);
    WNDCLASSEXW lw = { sizeof(lw) };
    lw.style = CS_DBLCLKS;
    lw.lpfnWndProc = layer_proc; lw.hInstance = g_inst; lw.lpszClassName = L"PlumaControlesVideo";
    lw.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&lw);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)g_dw.GetAddressOf());
    std::wstring path = g_root + L"\\fuentes\\Roboto.ttf";
    ComPtr<IDWriteFactory5> f5;
    ComPtr<IDWriteFontSetBuilder1> b; ComPtr<IDWriteFontFile> file; ComPtr<IDWriteFontSet> set; ComPtr<IDWriteFontCollection1> coll;
    if (PathFileExistsW(path.c_str()) && SUCCEEDED(g_dw.As(&f5)) && SUCCEEDED(f5->CreateFontSetBuilder(&b)) &&
        SUCCEEDED(f5->CreateFontFileReference(path.c_str(), NULL, &file)) && SUCCEEDED(b->AddFontFile(file.Get())) &&
        SUCCEEDED(b->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll)))
        g_roboto = coll;
    else g_family = L"Segoe UI";
    for (int i = 0; i < I_COUNT; i++) g_icons[i] = svg(kIcons[i].d);
    // the playlists and captions of earlier videos (their addresses expire within hours)
    std::wstring dir = g_state + L"\\cache-video\\";
    for (const wchar_t *pat : { L"*.m3u8", L"*.vtt" }) {
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((dir + pat).c_str(), &fd);
        if (fh == INVALID_HANDLE_VALUE) continue;
        do DeleteFileW((dir + fd.cFileName).c_str()); while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    g_ctl.snap(0);
    g_vol_open.snap(0);
    g_bar_hot.snap(0);
}
