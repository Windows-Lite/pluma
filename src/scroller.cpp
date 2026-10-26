// Pluma: the Scroller. The videos of the page on screen, whatever the site, one after another, tall, with the wheel or
// at the end of each, as TikTok (plan approved 26/10: one generic engine, no code for any one site).
// - Layer 1, here: the tab's network (WebResourceResponseReceived). Its HLS/DASH lists and video files (by their type
//   first, then their name; never the pieces of a list), up to 500 per page, in memory only. One is enough for the
//   bar's button; so is a quick count of the page's videos when it loads, or a site already used in the Scroller.
// - Layers 2 and 3 and the layer on screen: scroller.js (RCDATA 230), in a world of Pluma's own in the page
//   (Page.createIsolatedWorld). The page cannot see it nor reach it; it talks to Pluma through a binding added to
//   that world alone (Runtime.addBinding with its context: the main world has no such function), and only messages
//   from that context, of the expected shapes and sizes, are read. Pluma answers it with Runtime.evaluate in it.
// - Layer 4 and the player: each video goes to Pluma's player (video.cpp, one mpv for the whole feed) with the page's
//   Referer and agent, and its cookies only if the video is on the page's own host. Every address is checked again
//   there (scroller_core.h): http(s) only, no local files, none of mpv's own protocols.
// Incognito: nothing of what is watched goes to pluma.log or to disk (no sites remembered).
#include "pluma.h"
#include "json.h"
#include "scroller_core.h"
#include <bcrypt.h>
#include <map>
#include <set>

struct NetHit { std::wstring url, type; long long size = 0; };
struct Scroll {
    std::vector<NetHit> net;           // the page's videos its network brought (this document)
    std::set<std::wstring> net_keys;   // ... by address without its query
    size_t net_sent = 0;               // ... how many the open layer already has
    bool found = false;                // the bar's button: videos in this page
    bool open = false;
    int ctx = 0;                       // the engine's world (its execution context)
    std::wstring bind;                 // ... and the name of its binding (new each time)
    double wheel_at = 0;
    bool always_checked = false;       // the "always in the Scroller" site of this page was looked at
};
static std::map<int, Scroll> g_sc;
static std::map<std::wstring, bool> g_sites;   // scroller.tsv: site -> opens straight in the Scroller (else: used once)
static bool g_sites_read = false;

static std::string resource(int id) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    return g ? std::string((const char *)LockResource(g), SizeofResource(g_inst, r)) : std::string();
}
static std::wstring site_of(const std::wstring &url) {
    std::wstring h = lower(url_host(url));
    return starts_with(h, L"www.") ? h.substr(4) : h;
}
static bool web_page(Tab *t) {
    return t && t->view && !settings_page_url(t->url) && (starts_with(t->url, L"https://") || starts_with(t->url, L"http://"));
}

// ---------------------------------------------------------------- the sites (scroller.tsv; never from incognito)
static void sites_read() {
    if (g_sites_read) return;
    g_sites_read = true;
    for (auto &f : tsv_read(L"scroller.tsv")) if (!f[0].empty()) g_sites[lower(f[0])] = f.size() > 1 && f[1] == L"1";
}
static void sites_save() {
    if (g_incognito) return;
    std::wstring out;
    for (auto &s : g_sites) out += s.first + L"\t" + (s.second ? L"1" : L"0") + L"\n";
    write_file(g_data + L"\\scroller.tsv", utf8(out));
}
std::vector<std::wstring> scroller_sites() {
    sites_read();
    std::vector<std::wstring> v;
    for (auto &s : g_sites) if (s.second) v.push_back(s.first);
    return v;
}
void scroller_site_set(const std::wstring &site, bool always) {
    sites_read();
    if (g_incognito || site.empty()) return;
    g_sites[lower(site)] = always;
    sites_save();
    settings_page_refresh_soon();
}
static bool site_always(Tab *t) { sites_read(); auto it = g_sites.find(site_of(t->url)); return it != g_sites.end() && it->second; }

// ---------------------------------------------------------------- talking to the engine's world
static void ev(Tab *t, const std::wstring &js) {
    // Runtime.evaluate in the engine's world (never the page's: video.cpp's eval goes there)
    auto it = g_sc.find(t ? t->id : 0);
    if (!t || !t->view || it == g_sc.end() || !it->second.ctx) return;
    std::wstring p = L"{\"expression\":" + json_escape(js) + L",\"contextId\":" + std::to_wstring(it->second.ctx) + L",\"returnByValue\":true}";
    t->view->CallDevToolsProtocolMethod(L"Runtime.evaluate", p.c_str(), NULL);
}
static std::wstring net_json(Scroll &s, size_t from) {
    std::wstring j = L"[";
    for (size_t i = from; i < s.net.size(); i++) {
        if (j.size() > 1) j += L",";
        j += L"{\"url\":" + json_escape(s.net[i].url) + L",\"type\":" + json_escape(s.net[i].type) + L",\"size\":" + std::to_wstring(s.net[i].size) + L"}";
    }
    return j + L"]";
}
static void push_net(Tab *t) {
    // the network's new videos, to the open layer (a few at a time)
    auto &s = g_sc[t->id];
    if (!s.open || s.net_sent >= s.net.size()) return;
    ev(t, L"__plumaScroller.net(" + net_json(s, s.net_sent) + L")");
    s.net_sent = s.net.size();
}
static void tell_state(Tab *t) { ev(t, L"__plumaScroller.player(Object.assign({ev:'state'}," + video_scroller_state(t) + L"))"); }

// ---------------------------------------------------------------- layer 1: the network
static bool ends_any(const std::wstring &path, std::initializer_list<const wchar_t *> exts) {
    for (auto e : exts) if (ends_with_i(path, e)) return true;
    return false;
}
static void on_response(int id, ICoreWebView2WebResourceResponseReceivedEventArgs *a) {
    // every response of the tab: kept short (it runs for each one). Lists and files of video, by their type first;
    // never a list's pieces (.ts, .m4s) nor a list's first piece (a small .mp4)
    ComPtr<ICoreWebView2WebResourceRequest> rq;
    LPWSTR u = NULL;
    if (FAILED(a->get_Request(&rq)) || !rq || FAILED(rq->get_Uri(&u)) || !u) return;
    std::wstring url = u;
    CoTaskMemFree(u);
    if (!starts_with(url, L"https://") && !starts_with(url, L"http://")) return;
    std::wstring path = url.substr(0, url.find_first_of(L"?#"));
    if (ends_any(path, { L".js", L".css", L".png", L".jpg", L".jpeg", L".webp", L".avif", L".svg", L".ico", L".woff", L".woff2", L".ttf",
                         L".ts", L".m4s", L".m4a", L".aac", L".mp3", L".vtt", L".html", L".htm", L".php" })) return;
    ComPtr<ICoreWebView2WebResourceResponseView> rs;
    if (FAILED(a->get_Response(&rs)) || !rs) return;
    int code = 0;
    rs->get_StatusCode(&code);
    if (code != 200 && code != 206) return;
    ComPtr<ICoreWebView2HttpResponseHeaders> h;
    if (FAILED(rs->get_Headers(&h)) || !h) return;
    auto header = [&](const wchar_t *name) { LPWSTR v = NULL; std::wstring s; if (SUCCEEDED(h->GetHeader(name, &v)) && v) { s = v; CoTaskMemFree(v); } return lower(s); };
    std::wstring type = header(L"Content-Type");
    bool list = type.find(L"mpegurl") != std::wstring::npos || type.find(L"dash+xml") != std::wstring::npos ||
                ((ends_with_i(path, L".m3u8") || ends_with_i(path, L".mpd")) && type.find(L"html") == std::wstring::npos);
    bool file = !list && ((starts_with(type, L"video/") && type != L"video/mp2t" && type.find(L"segment") == std::wstring::npos) ||
                          (ends_any(path, { L".mp4", L".webm", L".mov", L".m4v" }) && (type.empty() || starts_with(type, L"application/octet"))));
    if (!list && !file) return;
    long long size = 0;
    std::wstring range = header(L"Content-Range");   // "bytes 0-1023/52341234": the whole file's size
    size_t slash = range.rfind(L'/');
    if (slash != std::wstring::npos) size = _wtoi64(range.c_str() + slash + 1);
    else if (code == 200) size = _wtoi64(header(L"Content-Length").c_str());
    if (file && size > 0 && size < 300 * 1024) return;   // (a list's first piece, an icon's loop)
    Tab *t = tab_by_id(id);
    if (!t) return;
    auto &s = g_sc[id];
    std::wstring key = path;
    if (s.net_keys.count(key) || s.net.size() >= 500) return;
    s.net_keys.insert(key);
    s.net.push_back({ url, list ? (type.find(L"dash") != std::wstring::npos || ends_with_i(path, L".mpd") ? L"application/dash+xml" : L"application/vnd.apple.mpegurl") : type, size });
    if (!s.found) { s.found = true; WinScope ws(win_of_tab(t)); if (t == active_tab()) ui_invalidate(); }
    if (s.open) {
        // (to the layer a moment later, with whatever else comes meanwhile)
        SetTimer(g_app, 0x7A000000 + id, 300, [](HWND w, UINT, UINT_PTR tid, DWORD) {
            KillTimer(w, tid);
            Tab *t = tab_by_id((int)(tid - 0x7A000000)); WinScope ws(win_of_tab(t));
            if (t) push_net(t);
        });
    }
}

// ---------------------------------------------------------------- what the engine says
static std::wstring short_text(const Json &j, size_t max) { std::wstring s = j.is_str() ? j.s : L""; return s.size() > max ? s.substr(0, max) : s; }
static void play(Tab *t, const Json &m) {
    // a video to Pluma's player: its address (checked again in video.cpp), the page's Referer and agent, and its
    // cookies only for the page's own host (a page could send them anywhere else through a redirect)
    ScrollMedia sm;
    sm.url = short_text(m[L"src"], 8192);
    sm.list = short_text(m[L"list"], 256 * 1024);
    sm.hls = m[L"hls"].b;
    sm.gif = m[L"gif"].b;
    sm.title = short_text(m[L"title"], 300);
    sm.author = short_text(m[L"author"], 120);
    sm.referer = t->url.substr(0, t->url.find(L'#'));
    const Json &b = m[L"box"];
    if (b.kind == Json::ARR && b.arr.size() == 5) { for (int i = 0; i < 4; i++) sm.box[i] = b.arr[i].n; sm.dpr = b.arr[4].n > 0 ? b.arr[4].n : 1; }
    ComPtr<ICoreWebView2Settings> st;
    ComPtr<ICoreWebView2Settings2> st2;
    LPWSTR ua = NULL;
    if (SUCCEEDED(t->view->get_Settings(&st)) && SUCCEEDED(st.As(&st2)) && SUCCEEDED(st2->get_UserAgent(&ua)) && ua) { sm.ua = ua; CoTaskMemFree(ua); }
    int id = t->id;
    auto go = [id](ScrollMedia sm) {
        Tab *t = tab_by_id(id); WinScope ws(win_of_tab(t));
        if (!t || !g_sc[id].open) return;
        if (!video_scroller_play(t, sm)) ev(t, L"__plumaScroller.player({ev:'failed'})");
    };
    ComPtr<ICoreWebView2_2> v2;
    ComPtr<ICoreWebView2CookieManager> cm;
    if (lower(url_host(sm.url)) != lower(url_host(t->url)) || FAILED(t->view.As(&v2)) || FAILED(v2->get_CookieManager(&cm)) || !cm) { go(sm); return; }
    cm->GetCookies(sm.url.c_str(), Callback<ICoreWebView2GetCookiesCompletedHandler>([go, sm](HRESULT hr, ICoreWebView2CookieList *list) mutable -> HRESULT {
        UINT n = 0;
        if (SUCCEEDED(hr) && list && SUCCEEDED(list->get_Count(&n)))
            for (UINT i = 0; i < n && i < 60; i++) {
                ComPtr<ICoreWebView2Cookie> c;
                LPWSTR name = NULL, value = NULL;
                if (FAILED(list->GetValueAtIndex(i, &c)) || !c) continue;
                if (SUCCEEDED(c->get_Name(&name)) && SUCCEEDED(c->get_Value(&value)) && name && value)
                    sm.cookie += (sm.cookie.empty() ? L"" : L"; ") + std::wstring(name) + L"=" + value;
                CoTaskMemFree(name); CoTaskMemFree(value);
            }
        go(sm);
        return S_OK;
    }).Get());
}
static void close_now(Tab *t, bool leaving);
static void on_message(int id, const std::wstring &json) {
    // Runtime.bindingCalled: only the engine's binding, from its own world, with a message of a known shape
    Tab *t = tab_by_id(id);
    if (!t) return;
    WinScope ws(win_of_tab(t));
    auto it = g_sc.find(id);
    if (it == g_sc.end() || !it->second.open || json.size() > 400 * 1024) return;
    Json e = json_parse(json);
    if (e[L"name"].s != it->second.bind || (int)e[L"executionContextId"].n != it->second.ctx || !e[L"payload"].is_str()) return;
    Json m = json_parse(e[L"payload"].s);
    std::wstring what = m[L"t"].s;
    if (what == L"box") {
        const Json &r = m[L"r"];
        if (r.kind == Json::ARR && r.arr.size() == 4) video_scroller_box(t, r.arr[0].n, r.arr[1].n, r.arr[2].n, r.arr[3].n, m[L"dpr"].n > 0 ? m[L"dpr"].n : 1);
    } else if (what == L"play") play(t, m);
    else if (what == L"hide") video_scroller_hide(t, m[L"on"].b);
    else if (what == L"cmd") {
        std::wstring c = m[L"c"].s;
        if (c == L"pause" || c == L"play" || c == L"mute" || c == L"loop") { video_scroller_cmd(t, c); tell_state(t); }
    } else if (what == L"close") close_now(t, false);
    else if (what == L"log" && (g_test || g_dev) && !t->incognito) {
        // tests: counts and states only (never an address: anything with "://" is cut)
        std::wstring s = short_text(m[L"s"], 180);
        size_t p = s.find(L"://");
        if (p != std::wstring::npos) s = s.substr(0, p);
        log_line(L"Scroller: " + s);
    }
}

// ---------------------------------------------------------------- in and out
static std::wstring random_name() {
    BYTE b[12] = {};
    BCryptGenRandom(NULL, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    wchar_t s[40] = L"__ps";
    for (int i = 0; i < 12; i++) swprintf_s(s + 4 + i * 2, 3, L"%02x", b[i]);
    return s;
}
static std::wstring engine_js(Tab *t, const std::wstring &bind) {
    static std::wstring src;
    if (src.empty()) src = wide(resource(230));
    const wchar_t *a = L"/*PLUMA_CFG*/", *b = L"/*PLUMA_CFG_FIN*/";
    size_t i = src.find(a), j = src.find(b);
    if (i == std::wstring::npos || j == std::wstring::npos || j < i) return L"";
    static const wchar_t *texts[] = { L"Anterior (↑)", L"Siguiente (↓)", L"Silenciar (M)", L"Activar sonido (M)", L"Bucle", L"Salir (Esc)",
        L"Rueda, ↑ ↓ o arrastra: otro vídeo · Espacio: pausa · M: sonido · Esc: salir", L"Cargando…", L"Buscando vídeos…", L"Buscando más vídeos…",
        L"No hay más vídeos", L"No hay vídeos en esta página", L"Este vídeo solo se ve en la página", L"Este vídeo no se pudo ver", L"Es el primero",
        L" vídeos no se pueden ver aquí", L"La web pide ir más despacio: se sigue en un minuto" };
    std::wstring tx = L"{";
    for (auto k : texts) tx += (tx.size() > 1 ? L"," : L"") + json_escape(k) + L":" + json_escape(T(k));
    tx += L"}";
    auto &s = g_sc[t->id];
    std::wstring cfg = L"{test:false,bind:" + json_escape(bind) + L",texts:" + tx + L",log:" + ((g_test || g_dev) && !t->incognito ? L"true" : L"false") +
                       L",mpv:" + (video_available() ? L"true" : L"false") + L",net:" + net_json(s, 0) + L"}";
    s.net_sent = s.net.size();
    return src.substr(0, i) + cfg + src.substr(j + wcslen(b));
}
static void cdp(Tab *t, const wchar_t *method, const std::wstring &params, std::function<void(const Json &)> done) {
    int id = t->id;
    t->view->CallDevToolsProtocolMethod(method, params.c_str(), Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
        [id, done](HRESULT hr, LPCWSTR json) -> HRESULT {
            Tab *t = tab_by_id(id); WinScope ws(win_of_tab(t));
            if (!t) return S_OK;
            done(SUCCEEDED(hr) && json ? json_parse(json) : Json());
            return S_OK;
        }).Get());
}
static void open_now(Tab *t) {
    // a world of Pluma's own in the page's main frame, a binding only it has, and the engine in it
    if (!web_page(t)) return;
    auto &s = g_sc[t->id];
    if (s.open) return;
    s.open = true;   // (now: a second F8 meanwhile does not open another)
    int id = t->id;
    auto fail = [id](const wchar_t *why) {
        log_line(std::wstring(L"Scroller: no se pudo abrir (") + why + L")");
        g_sc[id].open = false; g_sc[id].ctx = 0;
        if (Tab *t = tab_by_id(id)) { WinScope ws(win_of_tab(t)); ui_invalidate(); }
    };
    cdp(t, L"Page.getFrameTree", L"{}", [id, fail](const Json &r) {
        Tab *t = tab_by_id(id);
        std::wstring frame = r[L"frameTree"][L"frame"][L"id"].s;
        if (!t || frame.empty() || !g_sc[id].open) { fail(L"sin marco"); return; }
        cdp(t, L"Page.createIsolatedWorld", L"{\"frameId\":" + json_escape(frame) + L",\"worldName\":\"pluma-scroller\",\"grantUniveralAccess\":false}",
            [id, fail](const Json &r) {
            Tab *t = tab_by_id(id);
            int ctx = (int)r[L"executionContextId"].n;
            if (!t || !ctx || !g_sc[id].open) { fail(L"sin mundo"); return; }
            std::wstring bind = random_name();
            cdp(t, L"Runtime.addBinding", L"{\"name\":" + json_escape(bind) + L",\"executionContextId\":" + std::to_wstring(ctx) + L"}",
                [id, ctx, bind, fail](const Json &) {
                Tab *t = tab_by_id(id);
                if (!t || !g_sc[id].open) { fail(L"cerrado"); return; }
                auto &s = g_sc[id];
                s.ctx = ctx; s.bind = bind;
                std::wstring js = engine_js(t, bind);
                if (js.empty()) { fail(L"sin motor"); return; }
                cdp(t, L"Runtime.evaluate", L"{\"expression\":" + json_escape(js) + L",\"contextId\":" + std::to_wstring(ctx) + L",\"returnByValue\":true}",
                    [id, fail](const Json &r) {
                    if (r[L"result"][L"value"].n != 1) {
                        if (g_test && r[L"exceptionDetails"].kind == Json::OBJ) log_line(L"Scroller: " + r[L"exceptionDetails"][L"exception"][L"description"].s.substr(0, 300));
                        fail(r[L"exceptionDetails"].kind == Json::OBJ ? L"error del motor" : L"sin respuesta");
                        return;
                    }
                    Tab *t = tab_by_id(id);
                    if (!t) return;
                    if (!g_incognito && !t->incognito) { sites_read(); std::wstring site = site_of(t->url); if (!g_sites.count(site)) { g_sites[site] = false; sites_save(); } }
                    log_line(L"Scroller: abierto");
                    ui_invalidate();
                });
            });
        });
    });
}
static void close_now(Tab *t, bool leaving) {
    auto it = g_sc.find(t ? t->id : 0);
    if (!t || it == g_sc.end() || !it->second.open) return;
    if (!leaving) ev(t, L"__plumaScroller.close()");
    video_scroller_stop(t);
    if (t->view && !it->second.bind.empty())
        t->view->CallDevToolsProtocolMethod(L"Runtime.removeBinding", (L"{\"name\":" + json_escape(it->second.bind) + L"}").c_str(), NULL);
    it->second.open = false;
    it->second.ctx = 0;
    it->second.bind.clear();
    if (!leaving) log_line(L"Scroller: cerrado");
    ui_invalidate();
}
void scroller_toggle(Tab *t) {
    if (!t) return;
    if (scroller_is_open(t)) close_now(t, false);
    else open_now(t);
}
void scroller_close(Tab *t) { close_now(t, false); }
bool scroller_is_open(Tab *t) { auto it = g_sc.find(t ? t->id : 0); return it != g_sc.end() && it->second.open; }
bool scroller_offered(Tab *t) {
    if (!web_page(t)) return false;
    auto it = g_sc.find(t->id);
    if (it != g_sc.end() && (it->second.open || it->second.found)) return true;
    sites_read();
    return g_sites.count(site_of(t->url)) > 0;
}
void scroller_step(Tab *t, int dir) { if (scroller_is_open(t)) ev(t, L"__plumaScroller.step(" + std::to_wstring(dir > 0 ? 1 : -1) + L")"); }
void scroller_wheel(Tab *t, int delta) { if (scroller_is_open(t)) ev(t, L"__plumaScroller.wheel(" + std::to_wstring(-delta) + L")"); }
void scroller_player_event(Tab *t, int how) {
    if (!t) return;
    if (how == VIDEO_LEAVE) { close_now(t, true); return; }
    if (how == VIDEO_ASKED) { close_now(t, false); return; }
    if (how == VIDEO_ENDED) ev(t, L"__plumaScroller.player({ev:'ended'})");
    else if (how == VIDEO_FAILED) ev(t, L"__plumaScroller.player({ev:'failed'})");
}
void scroller_player_started(Tab *t, double aspect, double ms) {
    wchar_t js[120];
    swprintf_s(js, L"__plumaScroller.player({ev:'started',aspect:%.4f,ms:%d})", aspect > 0 ? aspect : 0, (int)ms);
    ev(t, js);
    tell_state(t);
    if (!t->incognito) log_line(L"Scroller: vídeo en " + std::to_wstring((int)ms) + L" ms");
}

// ---------------------------------------------------------------- every view
static const wchar_t *kQuickCount = LR"JS((() => {
  let n = document.querySelectorAll('video, meta[property^="og:video"], source[type^="video"]').length;
  for (const s of document.querySelectorAll('script[type="application/ld+json"]')) if (/VideoObject/.test(s.textContent)) { n++; break; }
  return n;
})())JS";
static void quick_count(Tab *t, bool late) {
    // a page with a video shows the button (and, a few seconds after it loads, a site set to "always" opens in the
    // Scroller: its videos are there by then)
    if (!web_page(t)) return;
    int id = t->id;
    t->view->ExecuteScript(kQuickCount, Callback<ICoreWebView2ExecuteScriptCompletedHandler>([id, late](HRESULT hr, LPCWSTR json) -> HRESULT {
        Tab *t = tab_by_id(id); WinScope ws(win_of_tab(t));
        if (!t || FAILED(hr) || !json) return S_OK;
        auto &s = g_sc[id];
        if (_wtoi(json) > 0 && !s.found) { s.found = true; if (t == active_tab()) ui_invalidate(); }
        if (late && !s.always_checked) {
            s.always_checked = true;
            if (site_always(t) && !s.open && video_available()) open_now(t);
        }
        return S_OK;
    }).Get());
}
void scroller_attach(Tab *t) {
    if (!t->view) return;
    int id = t->id;
    g_sc.erase(id);
    EventRegistrationToken tok;
    ICoreWebView2 *v = t->view.Get();
    ComPtr<ICoreWebView2_2> v2;
    if (SUCCEEDED(t->view.As(&v2))) {
        v2->add_WebResourceResponseReceived(Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
            [id](ICoreWebView2 *, ICoreWebView2WebResourceResponseReceivedEventArgs *a) -> HRESULT { on_response(id, a); return S_OK; }).Get(), &tok);
        v2->add_DOMContentLoaded(Callback<ICoreWebView2DOMContentLoadedEventHandler>(
            [id](ICoreWebView2 *, ICoreWebView2DOMContentLoadedEventArgs *) -> HRESULT { Tab *t = tab_by_id(id); WinScope ws(win_of_tab(t)); quick_count(t, false); return S_OK; }).Get(), &tok);
    }
    v->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *) -> HRESULT {
            // another document: the layer goes with this one (the player too), and what its network brought
            Tab *t = tab_by_id(id); WinScope ws(win_of_tab(t));
            if (!t) return S_OK;
            close_now(t, true);
            auto &s = g_sc[id];
            s.net.clear(); s.net_keys.clear(); s.net_sent = 0; s.always_checked = false;
            if (s.found) { s.found = false; if (t == active_tab()) ui_invalidate(); }
            return S_OK;
        }).Get(), &tok);
    v->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT {
            Tab *t = tab_by_id(id); WinScope ws(win_of_tab(t));
            quick_count(t, false);
            // pages that bring their videos later (X): once more a few seconds after
            SetTimer(g_app, 0x7B000000 + id, 3000, [](HWND w, UINT, UINT_PTR tid, DWORD) {
                KillTimer(w, tid);
                Tab *t = tab_by_id((int)(tid - 0x7B000000)); WinScope ws(win_of_tab(t));
                if (t) quick_count(t, true);
            });
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> rcv;
    if (SUCCEEDED(v->GetDevToolsProtocolEventReceiver(L"Runtime.bindingCalled", &rcv)) && rcv)
        rcv->add_DevToolsProtocolEventReceived(Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [id](ICoreWebView2 *, ICoreWebView2DevToolsProtocolEventReceivedEventArgs *a) -> HRESULT {
                LPWSTR j = NULL;
                if (SUCCEEDED(a->get_ParameterObjectAsJson(&j)) && j) { std::wstring s = j; CoTaskMemFree(j); on_message(id, s); }
                return S_OK;
            }).Get(), &tok);
}
