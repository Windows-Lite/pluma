// Pluma: light pages. When a tab goes to one of the pages of a site Pluma knows (YouTube, YouTube Music, Reddit),
// Pluma answers that document itself with its own light page (inside the exe, as RCDATA) at the same address. Same
// origin: the person's cookies and session go on working. The page asks the site's own data (YouTube's API, as its
// page does; Reddit's own server-drawn pieces) and draws the answer in a few KB instead of megabytes of code.
// YouTube: a video goes straight into Pluma's player (video.cpp), without YouTube's watch page loading first.
// YouTube Music: the music plays in Pluma's player, sound only, and goes on while the page moves (it never reloads).
// Reddit: lists, posts and comments; its videos in Pluma's player.
// Whatever a light page does not do is the site's own page, and so is a page whose call or parsing failed: it goes to
// the same address with pluma_normal=1 (the site's page for that address, in this tab) or pluma_normal=2 (its
// "normal" button: all of that site in this tab, until the tab leaves it). Pluma takes the mark off before anything
// goes out, so the site and the address bar never see it.
// No page talks to Pluma: what the page needs (theme, language, its texts) goes into the HTML as it is served, and
// Pluma asks the page (video.cpp: what to play, where its player is).
#include "pluma.h"
#include "json.h"
#include <map>
#include <set>

// ---------------------------------------------------------------- the sites
static std::wstring no_fragment(const std::wstring &u) { return u.substr(0, u.find(L'#')); }
static void split_url(const std::wstring &url, size_t origin_len, std::wstring &path, std::wstring &query) {
    std::wstring rest = url.substr(origin_len);
    size_t q = rest.find_first_of(L"?#");
    path = rest.substr(0, q);
    query = q == std::wstring::npos ? L"" : rest.substr(q);
    if (path.size() > 1 && path.back() == L'/') path.pop_back();
}
static bool has_param(const std::wstring &query, const wchar_t *k) {
    // the query has k with a value
    std::wstring a = L"?" + std::wstring(k) + L"=", b = L"&" + std::wstring(k) + L"=";
    size_t at = query.find(a);
    if (at == std::wstring::npos) at = query.find(b);
    return at != std::wstring::npos && at + a.size() < query.size() && query[at + a.size()] != L'&' && query[at + a.size()] != L'#';
}
static std::vector<std::wstring> path_parts(const std::wstring &path) {
    std::vector<std::wstring> parts;
    for (size_t i = 1; i <= path.size();) {
        size_t e = path.find(L'/', i);
        if (e == std::wstring::npos) e = path.size();
        if (e > i) parts.push_back(path.substr(i, e - i));
        i = e + 1;
    }
    return parts;
}

// YouTube: /, /results, /feed/subscriptions, /feed/history, /playlist, /watch (with Pluma's player there), and a
// channel's home or videos (/@name, /channel/UC..., /c/, /user/)
static bool youtube_host(const std::wstring &host) {
    return host == L"www.youtube.com" || host == L"youtube.com" || host == L"m.youtube.com" || host == L"youtu.be";
}
static bool youtube_url(const std::wstring &url) {
    std::wstring path, query;
    split_url(url, wcslen(L"https://www.youtube.com"), path, query);
    if (path == L"/") return true;
    if (path == L"/results") return has_param(query, L"search_query");
    if (path == L"/feed/subscriptions" || path == L"/feed/history") return true;
    if (path == L"/playlist") return has_param(query, L"list");
    if (path == L"/watch") return has_param(query, L"v") && g_set.native_video && video_available();
    std::vector<std::wstring> parts = path_parts(path);
    size_t base = 0;
    if (!parts.empty() && parts[0].size() > 1 && parts[0][0] == L'@') base = 1;
    else if (parts.size() >= 2 && (parts[0] == L"channel" || parts[0] == L"c" || parts[0] == L"user")) base = 2;
    else return false;
    if (parts.size() == base) return true;
    return parts.size() == base + 1 && (parts[base] == L"videos" || parts[base] == L"featured");
}
// YouTube Music: home, search, an artist, an album or a playlist (/browse/..., /channel/..., /playlist), the library
// and a song (/watch, which plays it). Its music needs Pluma's player
static bool music_host(const std::wstring &host) { return host == L"music.youtube.com"; }
static bool music_url(const std::wstring &url) {
    if (!g_set.native_video || !video_available()) return false;
    std::wstring path, query;
    split_url(url, wcslen(L"https://music.youtube.com"), path, query);
    if (path == L"/" || path == L"/library") return true;
    if (path == L"/search") return has_param(query, L"q");
    if (path == L"/playlist") return has_param(query, L"list");
    if (path == L"/watch") return has_param(query, L"v");
    std::vector<std::wstring> parts = path_parts(path);
    return parts.size() == 2 && (parts[0] == L"browse" || parts[0] == L"channel");
}
// Reddit (www.reddit.com; reddit.com goes there by itself, and old.reddit.com is not touched): the front page, a
// subreddit and its orders, a post with its comments, a person's page, search
static bool reddit_host(const std::wstring &host) { return host == L"www.reddit.com" || host == L"reddit.com"; }
static bool reddit_url(const std::wstring &url) {
    std::wstring path, query;
    split_url(url, wcslen(L"https://www.reddit.com"), path, query);
    std::vector<std::wstring> p = path_parts(path);
    auto order = [](const std::wstring &s) { return s == L"hot" || s == L"new" || s == L"top" || s == L"rising" || s == L"best"; };
    if (p.empty()) return true;
    if (p.size() == 1) return order(p[0]) || (p[0] == L"search" && has_param(query, L"q"));
    if (p[0] == L"r" && p[1].find(L'+') == std::wstring::npos) {
        if (p.size() == 2) return true;
        if (p.size() == 3) return order(p[2]) || (p[2] == L"search" && has_param(query, L"q"));
        return p[2] == L"comments" && p.size() <= 6;   // /r/x/comments/id[/slug[/comment]]
    }
    if (p[0] == L"comments") return p.size() <= 3;
    if (p[0] == L"user" || p[0] == L"u") return p.size() == 2 || (p.size() == 3 && (p[2] == L"submitted" || p[2] == L"comments" || p[2] == L"overview"));
    return false;
}

struct Site {
    int id;
    const wchar_t *origin;   // what Pluma answers (one origin: the light page lives there)
    int resource;            // its page, inside the exe
    bool *on;                // its setting
    bool (*host)(const std::wstring &);   // the site (leaving it ends "the normal one in this tab")
    bool (*url)(const std::wstring &);    // the addresses its light page does
    const wchar_t *name;     // for the log
    const wchar_t *shows_normal, *back;   // the note on a tab that shows the site's own page, and its button
};
static const Site kSites[] = {
    { LITE_YOUTUBE, L"https://www.youtube.com", 203, &g_set.youtube_lite, youtube_host, youtube_url, L"YouTube",
      L"Esta pestaña muestra el YouTube normal.", L"Volver a YouTube ligero" },
    { LITE_MUSIC, L"https://music.youtube.com", 204, &g_set.music_lite, music_host, music_url, L"YouTube Music",
      L"Esta pestaña muestra el YouTube Music normal.", L"Volver a YouTube Music ligero" },
    { LITE_REDDIT, L"https://www.reddit.com", 205, &g_set.reddit_lite, reddit_host, reddit_url, L"Reddit",
      L"Esta pestaña muestra el Reddit normal.", L"Volver a Reddit ligero" },
};
static const Site *site_of(const std::wstring &url) {
    // the site whose light page answers this address (its origin exactly)
    for (auto &s : kSites) {
        size_t n = wcslen(s.origin);
        if (url.size() > n && url.compare(0, n, s.origin) == 0 && url[n] == L'/') return &s;
    }
    return nullptr;
}
static const Site *site_by_id(int id) { for (auto &s : kSites) if (s.id == id) return &s; return nullptr; }

// ---------------------------------------------------------------- each tab
static std::map<int, int> g_normal;                     // tabs on a site's own page (its button), until they leave it
static std::map<int, std::set<std::wstring>> g_pass;    // addresses that are the site's own page in a tab (after a failure)
static std::map<int, std::wstring> g_serve;             // the document Pluma answers in each tab's main frame
static std::map<int, int> g_tell;                       // tabs to tell, once loaded, how to come back to the light one

// an address the site's own page was asked for, as remembered: YouTube's page loads itself again with themeRefresh=1
// when its theme differs from the cookie's, and that is still the same page
static std::wstring pass_key(const std::wstring &u) {
    std::wstring k = no_fragment(u);
    for (const wchar_t *p : { L"&themeRefresh=1", L"?themeRefresh=1&", L"?themeRefresh=1" }) {
        size_t at = k.find(p);
        if (at != std::wstring::npos) { k.erase(at + (p[0] == L'?' ? 1 : 0), wcslen(p) - (p[0] == L'?' ? 1 : 0)); break; }
    }
    if (!k.empty() && k.back() == L'?') k.pop_back();
    return k;
}

// the address without Pluma's mark, and which mark it had (0 none, 1 this address, 2 this tab)
static std::wstring take_mark(const std::wstring &u, int &kind) {
    kind = 0;
    size_t q = u.find(L'?');
    if (q == std::wstring::npos) return u;
    size_t h = u.find(L'#', q);
    std::wstring frag = h == std::wstring::npos ? L"" : u.substr(h), query = u.substr(q + 1, h == std::wstring::npos ? std::wstring::npos : h - q - 1), out;
    for (size_t i = 0; i <= query.size();) {
        size_t e = query.find(L'&', i);
        if (e == std::wstring::npos) e = query.size();
        std::wstring p = query.substr(i, e - i);
        if (starts_with(p, L"pluma_normal=")) kind = std::max(1, _wtoi(p.c_str() + 13));
        else if (!p.empty()) out += (out.empty() ? L"" : L"&") + p;
        i = e + 1;
    }
    return u.substr(0, q) + (out.empty() ? L"" : L"?" + out) + frag;
}

// ---------------------------------------------------------------- the pages
static std::string resource(int id) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    return g ? std::string((const char *)LockResource(g), SizeofResource(g_inst, r)) : std::string();
}
// a page, with what it needs from Pluma written into it: the bits all light pages share (lite.html, RCDATA 206, in
// place of <!--PLUMA_COMUN-->), the theme, the language, and its texts in that language (written in Spanish in the
// page, as t('...'), and translated with T() as every text of Pluma)
static std::string page_html(int id) {
    struct Page { std::string raw; std::vector<std::wstring> words; };
    static std::map<int, Page> pages;
    Page &pg = pages[id];
    if (pg.raw.empty()) {
        pg.raw = resource(id);
        if (pg.raw.empty()) return "";
        const char *mark = "<!--PLUMA_COMUN-->";
        size_t m = pg.raw.find(mark);
        if (m != std::string::npos) pg.raw = pg.raw.substr(0, m) + resource(206) + pg.raw.substr(m + strlen(mark));
        std::set<std::string> seen;
        const std::string &raw = pg.raw;
        for (size_t i = raw.find("t('"); i != std::string::npos; i = raw.find("t('", i + 3)) {
            char before = i ? raw[i - 1] : ' ';
            if (isalnum((unsigned char)before) || before == '_' || before == '$' || before == '.') continue;
            size_t e = raw.find('\'', i + 3);
            if (e == std::string::npos || e - i > 300) continue;
            std::string w = raw.substr(i + 3, e - i - 3);
            if (seen.insert(w).second) pg.words.push_back(wide(w));
        }
    }
    std::wstring cfg = L"{\"dark\":" + std::wstring(dark_theme() ? L"true" : L"false") + L",\"hl\":" + json_escape(lang_code()) + L",\"t\":{";
    bool first = true;
    for (auto &w : pg.words) {
        const wchar_t *tr = T(w.c_str());
        if (w == tr) continue;
        cfg += (first ? L"" : L",") + json_escape(w) + L":" + json_escape(tr);
        first = false;
    }
    cfg += L"}}";
    const char *a = "/*PLUMA_CFG*/", *b = "/*PLUMA_CFG_FIN*/";
    size_t s = pg.raw.find(a), e = pg.raw.find(b);
    if (s == std::string::npos || e == std::string::npos || e < s) return pg.raw;
    return pg.raw.substr(0, s) + utf8(cfg) + pg.raw.substr(e + strlen(b));
}

static void serve(ICoreWebView2WebResourceRequestedEventArgs *a, int resource_id) {
    std::string body = page_html(resource_id);
    if (body.empty() || !g_env) return;   // (then the site's own)
    IStream *s = SHCreateMemStream((const BYTE *)body.data(), (UINT)body.size());
    ComPtr<ICoreWebView2WebResourceResponse> res;
    if (s && SUCCEEDED(g_env->CreateWebResourceResponse(s, 200, L"OK",
            L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff", &res)))
        a->put_Response(res.Get());
    if (s) s->Release();
}

void lite_attach(Tab *t) {
    int id = t->id;
    ICoreWebView2 *v = t->view.Get();
    if (!v) return;
    // documents only (the page's own calls go to the site untouched); and those a service worker of the site's asks
    // for in the page's name, if it has one (YouTube and YouTube Music have)
    ComPtr<ICoreWebView2_22> v22;
    t->view.As(&v22);
    for (auto &s : kSites) {
        std::wstring filter = std::wstring(s.origin) + L"/*";
        if (v22) v22->AddWebResourceRequestedFilterWithRequestSourceKinds(filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT,
                                                                          COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
        else v->AddWebResourceRequestedFilter(filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
    }
    EventRegistrationToken tok;
    v->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
            // (the main frame only: frames have an event of their own)
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            LPWSTR uri = NULL;
            a->get_Uri(&uri);
            std::wstring u = uri ? uri : L"";
            CoTaskMemFree(uri);
            if (t->video && (t->yt_lite || t->lite == LITE_REDDIT)) video_back_to_page(t, VIDEO_LEAVE);   // (the light page's player goes with it)
            if (t->lite == LITE_MUSIC) audio_stop(t);   // (a new document: the light Music page goes, and its music)
            int kind = 0;
            std::wstring clean = take_mark(u, kind);
            const Site *site = site_of(u);
            if (kind && site) {
                // the site's own page, asked for by the light one: without the mark, and remembered
                a->put_Cancel(TRUE);
                if (kind >= 2) { g_normal[id] = site->id; g_tell[id] = site->id; }
                else g_pass[id].insert(pass_key(clean));
                t->yt_lite = false;
                t->lite = LITE_NONE;
                g_serve.erase(id);
                log_line(std::wstring(site->name) + L" ligero: la página normal" + std::wstring(kind >= 2 ? L" en esta pestaña" : L" para esta dirección"));
                ui_post([id, clean] { Tab *t = tab_by_id(id); if (t && t->view) t->view->Navigate(clean.c_str()); });
                return S_OK;
            }
            // leaving a site ends "its normal page in this tab" (and what failed there is tried again next time)
            std::wstring host = url_host(u);
            auto n = g_normal.find(id);
            if (n != g_normal.end()) { const Site *ns = site_by_id(n->second); if (!ns || !ns->host(host)) { g_normal.erase(n); g_tell.erase(id); } }
            bool known = false;
            for (auto &s : kSites) if (s.host(host)) known = true;
            if (!known) g_pass.erase(id);
            std::wstring nf = no_fragment(u);
            auto p = g_pass.find(id);
            bool lite = site && *site->on && !(g_normal.count(id) && g_normal[id] == site->id) && !(p != g_pass.end() && p->second.count(pass_key(nf))) &&
                        site->url(nf);
            t->lite = lite ? site->id : LITE_NONE;
            t->yt_lite = lite && site->id == LITE_YOUTUBE;
            if (lite) g_serve[id] = nf; else g_serve.erase(id);
            return S_OK;
        }).Get(), &tok);
    v->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *a) -> HRESULT {
            auto it = g_serve.find(id);
            Tab *t = tab_by_id(id);
            if (it == g_serve.end() || !t) return S_OK;
            ComPtr<ICoreWebView2WebResourceRequest> req;
            LPWSTR uri = NULL, method = NULL;
            if (FAILED(a->get_Request(&req)) || FAILED(req->get_Uri(&uri))) return S_OK;
            std::wstring u = uri; CoTaskMemFree(uri);
            req->get_Method(&method);
            bool get = method && !wcscmp(method, L"GET");
            CoTaskMemFree(method);
            const Site *s = site_by_id(t->lite);
            if (get && s && no_fragment(u) == it->second) {
                serve(a, s->resource);
                static int told = 0;   // (a few lines: to know, next time, whether a page that stayed the site's passed through here)
                if (told++ < 20) log_line(std::wstring(s->name) + L" ligero: servida " + u.substr(0, u.find_first_of(L"?#")));
            }
            return S_OK;
        }).Get(), &tok);
    v->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
            BOOL ok = FALSE; a->get_IsSuccess(&ok);
            Tab *t = tab_by_id(id);
            if (ok && t && t->lite == LITE_MUSIC) audio_page_loaded(t);
            auto tell = g_tell.find(id);
            if (!ok || tell == g_tell.end()) return S_OK;
            const Site *s = site_by_id(tell->second);
            g_tell.erase(tell);
            if (!s) return S_OK;
            ui_notice(T(s->shows_normal), T(s->back), [id] {
                Tab *t = tab_by_id(id);
                g_normal.erase(id);
                g_pass.erase(id);
                if (t && t->view) t->view->Navigate(t->url.c_str());
            }, 12, id);
            return S_OK;
        }).Get(), &tok);
}

void lite_settings_changed() {
    // a site off: its light pages open become the site's own (they load again); on: from its next page
    for (auto &t : g_tabs) {
        const Site *s = site_by_id(t->lite);
        if (s && !*s->on && t->view) t->view->Reload();
    }
}
