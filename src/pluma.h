// Pluma: a light, fast web browser for Windows.
//
// One native window (C++, Win32) whose bar is drawn with Direct2D, and one WebView2 (Edge's engine, part of Windows
// and kept up to date by it) per tab that is actually in use. Nothing of the interface is a web page, so the bar costs
// no extra process. Fast like Zas: a single instance stays in memory, hidden, with the engine warm.
//   Pluma.exe [url]        opens the url (or the window) in the one in memory, starting it if needed
//   Pluma.exe --oculto     starts it hidden, ready (for Windows startup)
//   Pluma.exe --salir      closes the one in memory for good
// PLUMA_INSTANCIA=<name> runs a separate instance (own mutex, window class and data), off the screen, for tests;
// PLUMA_DEPURAR=<port> opens the DevTools protocol on that port.
#pragma once
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <cmath>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

struct Json;   // json.h; the bookmarks/import pages take and give JSON

// ---------------------------------------------------------------- small things (main.cpp)
double now_ms();
std::string utf8(const std::wstring &s);
std::wstring wide(const std::string &s);
void log_line(const std::wstring &s);
bool read_file(const std::wstring &p, std::string &out);
bool write_file(const std::wstring &p, const std::string &data);
std::wstring lower(std::wstring s);
std::wstring trim(const std::wstring &s);
std::wstring url_host(const std::wstring &url);   // "https://www.x.com/a" -> "www.x.com"
std::wstring url_decode(const std::wstring &s);
std::wstring url_encode(const std::wstring &s);
std::wstring json_escape(const std::wstring &s);  // a JSON string literal, quotes included
std::wstring hash_hex(const std::wstring &s);
bool starts_with(const std::wstring &s, const wchar_t *p);
bool ends_with_i(const std::wstring &s, const wchar_t *p);
std::vector<std::vector<std::wstring>> tsv_read(const std::wstring &file);   // a tab file in g_state, split into rows/fields

// One value that moves towards a target with ease-out (no overshoot). Everything that moves in the bar is one of these.
struct Anim {
    float from = 0, to = 0;
    double t0 = 0, dur = 0;
    float at(double t) const {
        if (dur <= 0 || t >= t0 + dur) return to;
        double k = (t - t0) / dur;
        k = 1 - (1 - k) * (1 - k) * (1 - k);
        return from + (to - from) * (float)k;
    }
    bool running(double t) const { return dur > 0 && t < t0 + dur; }
    void go(float target, double d);   // d: ms at full motion (scaled down to 0 when Windows animations are off)
    void snap(float v) { from = to = v; dur = 0; }
};
extern bool g_motion;   // Windows "Show animations"

// ---------------------------------------------------------------- settings and app state
struct Settings {
    std::wstring engine = L"google";   // google, duckduckgo, bing, startpage, brave
    int theme = 2;                     // 0 light (as Mapea), 1 dark, 2 as Windows (the default)
    int update = 2;                    // 0 never, 1 tell me, 2 automatic (the default; update.cpp)
    std::wstring lang;                 // the interface's language: es, en, pt, fr, de, it (empty = Windows's)
    std::wstring font;                 // the bar's typeface: a family installed in Windows or one in fuentes\ (empty = Windows's)
    int sleep_s = 45;                  // a hidden tab is frozen after this
    int discard_s = 60;                // ... and unloaded after this (0 = never), except the most recent ones:
    int keep_recent = 2;               // the last tabs looked at stay frozen (to come back at once) for
    int recent_discard_min = 20;       // this long
    bool spare = false;                // a view made ahead of time (the next page starts 35 ms sooner; costs ~10 MB)
    bool resident = true;              // closing the window keeps the engine warm (about 75 MB)
    bool native_video = true;          // YouTube videos in Pluma's own player, the page asleep behind it (video.cpp)
    bool suggest_top = true;           // the new tab page suggests the most visited sites while there are none of the person's own
    bool bookmarks_bar = false;        // a row of bookmarks under the toolbar (Ctrl+Shift+B); off by default
    std::vector<std::wstring> schemes = { L"ytmp3" };   // external apps opened without asking
};
extern Settings g_set;

// ---------------------------------------------------------------- updates (update.cpp)
const wchar_t *update_version();   // the release this is ("dev" for a local build)
void update_finish();              // at start: what the last update left
void update_init();                // checks from now on, if the build and the settings allow it
void update_check_now();           // the menu's "check now"
void update_on_hide();             // the window was closed: a waiting update goes in
void update_on_quit();
// ---------------------------------------------------------------- the settings page (settings.cpp)
bool settings_page_url(const std::wstring &u);              // https://pluma.local/...: Pluma's own page
std::wstring settings_page_display(const std::wstring &u);  // ... shown as pluma://...
std::wstring settings_page_from_typed(const std::wstring &typed);   // pluma://settings typed -> its url ("" if not)
void settings_open();                // opens it (or goes to the tab that has it)
void settings_attach(ICoreWebView2 *v);   // every view: serves the page and listens to it
void settings_page_refresh();        // the open settings pages show what changed
void settings_page_refresh_soon();
void data_clear_all();               // cookies, cache and history (not passwords)
void ui_ntp_changed();               // the new tab page's rows are read again
void ui_open_menu_page(const std::wstring &which);   // the menu, open at "ext" or "scripts"
std::wstring lang_name(const wchar_t *code);         // a language by its own name ("" = Windows's)             // Pluma closes for good: a waiting update goes in for the next start

// ---------------------------------------------------------------- languages (lang.cpp)
const wchar_t *T(const wchar_t *es);   // a text of the interface, written in Spanish, in the person's language
const wchar_t *lang_code();            // es, en, pt, fr, de, it
void settings_load();
void settings_save();
std::wstring search_url(const std::wstring &q);
std::wstring engine_name(const std::wstring &id);

struct HistoryEntry { std::wstring url, title; int visits = 0; long long last = 0; };
extern std::vector<HistoryEntry> g_history;
void history_load();
void history_add(const std::wstring &url, const std::wstring &title);
void history_title(const std::wstring &url, const std::wstring &title);
bool history_import(const std::wstring &url, const std::wstring &title, int visits, long long last);   // from another browser
void history_flush();
void history_clear();
struct Suggestion { std::wstring url, title; bool search = false; };
std::vector<Suggestion> history_suggest(const std::wstring &typed, std::wstring *inline_completion);
std::vector<HistoryEntry> history_top(int n);
// the new tab page: the person's own pages (added with the address's star, Ctrl+D), and sites taken off its suggestions
extern std::vector<HistoryEntry> g_pins;    // the pages shown on the new tab page: a view of the "Tus páginas" folder
bool pins_has(const std::wstring &url);
bool pins_toggle(const std::wstring &url, const std::wstring &title);   // true: added
void pins_add(const std::wstring &url, const std::wstring &title, size_t at);
void pins_remove(const std::wstring &url);
void history_hide_site(const std::wstring &url, bool hide);           // off the suggestions (or back on)
void hidden_load();                                                   // the sites taken off the suggestions (ocultas.tsv)

// ---------------------------------------------------------------- bookmarks (bookmarks.cpp): marcadores.json, a tree of folders
struct BmNode {
    int id = 0;
    int role = 0;                   // 0 normal, 1 the bookmarks bar, 2 the new tab page's "Tus páginas"
    bool folder = false;
    std::wstring title, url;
    long long added = 0;            // epoch seconds
    std::vector<BmNode> kids;       // for folders
};
extern BmNode g_bm_root;
void bookmarks_load();              // reads marcadores.json; migrates paginas.tsv the first time
void bookmarks_save();
int bm_bar_folder();                // id of "Barra de marcadores"
int bm_pins_folder();              // id of "Tus páginas" (the new tab page)
BmNode *bm_find(int id);           // a node by its id, or nullptr
int bm_add(int parent, const std::wstring &url, const std::wstring &title);   // a bookmark; -1 if its url is already there
int bm_add_folder(int parent, const std::wstring &title);
void bm_remove(int id);
void bm_rename(int id, const std::wstring &title);
void bm_move(int id, int new_parent, int at);
bool bm_url_in(int folder, const std::wstring &url);
void bm_toggle_in(int folder, const std::wstring &url, const std::wstring &title);   // add it to the folder, or take it out
int bm_count();                     // how many bookmarks (not folders) in the whole tree
int bm_new_id();                    // a fresh node id (for import, which builds a subtree in one go)
BmNode &bm_root();                  // the root folder (its kids are the top-level folders)
void bm_commit();                   // after a batch change: save, resync the new tab page, refresh the open pages
void bookmarks_open();              // the manager page (pluma://bookmarks)
void import_open();                 // the wizard (pluma://import)
// the pages Pluma serves from the exe that are not the settings page (bookmarks.cpp answers them)
std::wstring bm_page_state(const std::wstring &path, const Json &texts);
void bm_page_do(ICoreWebView2 *v, const std::wstring &path, const Json &j);
void bm_pages_refresh();            // the open bookmarks/import pages show what changed

// ---------------------------------------------------------------- import from another browser (import.cpp, used by bookmarks.cpp)
struct ImportProfile {
    std::wstring browser;           // "Edge", "Brave", "Vivaldi", "Opera", "Firefox"
    std::wstring name;              // the profile's display name
    std::wstring path;              // the profile's folder
    bool firefox = false;
    bool running = false;           // the browser is open (its files are copied before being read)
    int bookmarks = 0, history = 0, passwords = 0;   // counts found (estimates)
};
std::vector<ImportProfile> import_scan();           // the browsers and profiles found
struct ImportResult { int bookmarks = 0, history = 0, passwords = 0; std::wstring note; };
ImportResult import_do(const ImportProfile &p, bool bookmarks, bool history, bool passwords);
ImportResult import_passwords_csv(const std::wstring &path);   // a CSV exported by another browser
int passwords_store_kind();          // 0 unknown, 10 writable (v10/DPAPI), 20 app-bound (not writable)

void set_fullscreen(bool on);
void app_quit();                          // closes Pluma for good (not just the window)
void app_close_window();                  // what the window's X does
std::wstring app_name();                  // "Pluma", or "Pluma (Incógnito)": the window's title
void app_open_incognito(const std::wstring &url = L"");   // the incognito window (or a tab in it, from there)
void app_restart(bool hidden);            // starts Pluma.exe again (after an update) and closes this one
std::wstring fixup_input(const std::wstring &typed);   // what was typed in the address bar -> the url to load

extern HINSTANCE g_inst;
extern HWND g_win, g_bar, g_edit;
extern std::wstring g_root, g_state;   // the exe's folder; %LOCALAPPDATA%\Pluma (or its test folder)
extern bool g_test, g_dev, g_quitting, g_fullscreen;
extern bool g_incognito;   // the incognito window (--incognito): every tab InPrivate, all dark, nothing kept
extern bool g_test_visible;   // a test instance on the screen (PLUMA_A_LA_VISTA), to record what the user would see
extern double g_open_asked;               // when a page was asked for (start, or from memory): for the log
extern ComPtr<ICoreWebView2Environment> g_env;

// ---------------------------------------------------------------- tabs (tabs.cpp)
struct Tab {
    int id = 0;
    std::wstring url, title;
    ComPtr<ICoreWebView2Controller> ctrl;
    ComPtr<ICoreWebView2> view;
    std::string icon_png;              // favicon, PNG bytes (the bar decodes them)
    void *icon_bmp = nullptr;          // ID2D1Bitmap owned by the bar
    bool loading = false, audible = false, muted = false, suspended = false, creating = false;
    bool can_back = false, can_fwd = false, crashed = false;
    double hidden_since = 0;           // 0 while it is the one on screen
    std::wstring pending;              // a url to load as soon as the view exists
    std::vector<std::wstring> scripts_ids;   // the injected userscripts, to swap them
    double saved_x = 0, saved_y = 0, saved_media = 0;   // where the page was when it was last hidden
    bool restoring = false;            // unloaded and loaded again: put it back where it was
    float progress = 0;                // loading line, 0..1 (estimated: WebView2 does not give one)
    double load_started = 0, load_done = 0;
    void *video = nullptr;             // Pluma's own player for its YouTube video (video.cpp), waiting or on
    bool video_mode = false;           // ... on screen instead of the page, which sleeps behind it
    std::wstring video_declined;       // the video the person went back to the page for: it stays on the page
    bool incognito = false;            // InPrivate: nothing of it is kept (history, cookies, session); gone with the window
    // the bar's geometry, animated
    Anim x, w, hover, close_hover;
    bool placed = false;
};
extern std::vector<std::unique_ptr<Tab>> g_tabs;
void with_profile(std::function<void(ICoreWebView2Profile *)> f);   // the browser profile, once a view exists
extern int g_active;

Tab *active_tab();
Tab *tab_by_id(int id);
int tab_index(int id);
Tab *tab_new(const std::wstring &url, bool activate, int at = -1, bool incognito = false);   // url "" = the empty new tab
void tab_close(int index, bool from_mouse = false);
void tab_activate(int index);
void tab_navigate(Tab *t, const std::wstring &url);
void tabs_preload(const std::wstring &url);
void tab_warm(Tab *t);                    // a tab that was let go starts loading again (the mouse rests on it)   // starts loading what is about to be asked for ("" stops it)
void tab_reopen_closed();
void tabs_layout_views();                 // put the active view where the page goes
void tabs_tick();                         // every few seconds: freeze, unload
void tabs_discard_all();
void tabs_on_hide();                      // the window closed (kept in memory): freeze the pages, then give memory back
enum { TRIM_FROZEN, TRIM_IDLE, TRIM_ALL };
void tabs_trim(int mode);                 // processes with nothing on screen give their unused memory back to Windows
bool user_away(double ms);                // nobody has touched the PC for that long (tests: always)
void tabs_set_scripts();                  // userscripts changed: refresh them in every view
void tabs_apply_theme();
void tab_mute(Tab *t);
void tab_go_back(); void tab_go_forward(); void tab_reload(bool hard); void tab_stop();
void tab_zoom(int dir);                   // -1 out, 0 reset, +1 in
double tab_zoom_factor();
void tab_find();
void tab_print();
void tab_devtools();
void tab_downloads();
void session_save_soon();
void session_save();
void session_restore();
void env_create(std::function<void()> ready);
void spare_prepare();
size_t engine_memory_mb();               // private memory of all Pluma processes (as the Task Manager counts it)

// ---------------------------------------------------------------- the bar (ui.cpp)
void ui_init();
void ui_layout();                         // window resized, fullscreen, tabs added...
void ui_invalidate();
float ui_band_height();                   // px; 0 in fullscreen
void ui_tab_closing(Tab *t, bool from_mouse);   // keeps a fading copy while the others close the gap
void ui_tab_icon_changed(Tab *t);
void ui_release_icon(Tab *t);
void ui_focus_address(bool select_all = true);
bool ui_address_focused();
void ui_set_address_text(const std::wstring &s);
void ui_theme_changed();
void ui_font_changed();
void ui_dpi_changed();
void ui_notice(const std::wstring &text, const std::wstring &button, std::function<void()> action, int seconds = 0, int owner_tab = 0);
void ui_notice_drop(int tab_id);          // the tab a note was about went elsewhere: the note goes
void ui_toast(const std::wstring &text);
void ui_open_menu();
void ui_close_popups();
void ui_caption_hover(int code);          // HTMINBUTTON/HTMAXBUTTON/HTCLOSE under the mouse, or 0
void ui_caption_press(int code, bool down);
int ui_hit_caption(POINT client);         // HTCLIENT over a control; HTCAPTION or a caption button elsewhere in the bar
void ui_window_activated(bool active);
void ui_post(std::function<void()> f);    // runs f on the window's thread (from another thread)
float ui_scale();
bool handle_shortcut(UINT vk, bool ctrl, bool shift, bool alt);
void ui_toggle_pin();                      // the page on screen onto the new tab page, or off it (the star, Ctrl+D)

// colours (Mapea's tokens)
struct Palette { D2D1_COLOR_F bg, surface, text, sub, line, line_strong, accent, accent_soft, error, hover, press; };
const Palette &pal();
bool dark_theme();       // the bar (always dark in the incognito window)
bool user_dark_theme();  // what the settings say (ordinary pages follow this one)

// ---------------------------------------------------------------- YouTube in Pluma's own player (video.cpp)
void video_init();
bool video_available();                   // the engine (motor-video\libmpv-2.dll) is there
void video_page_changed(Tab *t);          // its address changed or it came on screen: a YouTube video starts the watch
enum { VIDEO_ASKED, VIDEO_ENDED, VIDEO_LEAVE, VIDEO_FAILED, VIDEO_WOKEN };
void video_back_to_page(Tab *t, int how); // the page again: ASKED (and stays there), ENDED, LEAVE (it navigates), FAILED,
                                          // WOKEN (the person went to the page: the player comes back once left alone)
void video_destroy(Tab *t);               // the tab or its page goes: the player too, the page untouched
void video_layout();                      // window moved or resized, tab switched: the player follows
bool video_on(Tab *t);
bool video_settled();                     // the player on screen already draws at its new size
double video_position(Tab *t);
bool video_focus();                       // the keys to the player, if one is on screen
void video_toggle_mute(Tab *t);
void video_settings_changed();
void video_debug_shot(const std::wstring &path);   // tests: the window with the controls' real transparency
void video_resized();                     // the player drew at a new size (the fullscreen zoom may end)
double video_aspect();                    // the playing video's width/height, or 0 without Pluma's player
RECT video_window_rect();                 // the player's window on the screen
void *video_client();                     // a handle of its own on the player on screen, for another thread
void video_client_done(void *client);
bool video_frame(void *client, int max_w, std::vector<BYTE> &px, int &w, int &h);   // the frame on screen (BGRX)
void video_zoom_done();                   // the picture is in place: the controls again

// ---------------------------------------------------------------- fullscreen (main.cpp, with the zoom of zoom.cpp)
void fullscreen_now(bool on);             // the change itself, at once
RECT fullscreen_rect();                   // the screen it fills (in tests, one off the user's screen)
void zoom_start(bool on);
bool zoom_busy();
void zoom_want(bool on);                  // asked during a zoom: where it ends
void zoom_stop();                         // the window goes: the zoom ends at once
void zoom_message(WPARAM wp, LPARAM lp);

// ---------------------------------------------------------------- userscripts (scripts.cpp)
struct UserScript { std::wstring file, name, version, description, run_at; bool enabled = true, noframes = false;
                    std::vector<std::wstring> match, include, exclude; std::string code; };
extern std::vector<UserScript> g_scripts;
void scripts_load();
std::vector<std::wstring> scripts_sources();   // the enabled userscripts, each ready to inject in every page
const std::wstring &base_script();        // Pluma's own few lines in every page: background tabs, prefetch on hover
bool script_install(const std::wstring &source, std::wstring &name_out);
void script_toggle(size_t i);
std::wstring scripts_folder();

// ---------------------------------------------------------------- extensions (extensions.cpp)
struct Extension { std::wstring id, name, folder, popup, options, icon_path; bool enabled = true, builtin = false; };
extern std::vector<Extension> g_exts;
void ext_sync(std::function<void()> done);   // loads the folders in extensiones\ and reads what is installed
void ext_toggle(size_t i);
void ext_remove(size_t i);
void ext_open_popup(size_t i, RECT anchor);
void ext_open_options(size_t i);
bool ext_bundled_removed();               // one of those that come with Pluma (uBlock Origin Lite) was taken off
void ext_restore_bundled();               // ... and back on
bool store_extension_id(const std::wstring &url, std::wstring &id, bool &edge);
void store_install(const std::wstring &id, bool edge);
std::wstring extensions_folder();
