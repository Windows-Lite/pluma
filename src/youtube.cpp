// Pluma: YouTube ligero. When a tab goes to one of YouTube's pages (home, search, a channel, a list, subscriptions,
// history, a video), Pluma answers that document itself with its own light page (youtube.html, inside the exe,
// RCDATA 203) at the same address. Same origin: the person's cookies and session go on working. The page asks
// YouTube's own API (/youtubei/v1, as YouTube's page does; signed with SAPISIDHASH when signed in) and draws the
// answer in a few KB instead of YouTube's ~3 MB of code; a video goes straight into Pluma's player (video.cpp),
// without YouTube's watch page loading first.
// Whatever it does not do (lives, Shorts, Studio, shopping, music.youtube.com...) is YouTube's own page, and so is
// a page whose call or parsing failed: it goes to the same address with pluma_normal=1 (YouTube's page for that
// address, in this tab) or pluma_normal=2 (its "YouTube normal" button: all of YouTube in this tab, until the tab
// leaves YouTube). Pluma takes the mark off before anything goes out, so YouTube and the address bar never see it.
// No page talks to Pluma: what the page needs (theme, language, its texts) goes into the HTML as it is served.
#include "pluma.h"
#include "json.h"
#include <map>
#include <set>

static std::map<int, bool> g_normal;                    // tabs on YouTube's own page (its button), until they leave YouTube
static std::map<int, std::set<std::wstring>> g_pass;    // addresses that are YouTube's own page in a tab (after a failure)
static std::map<int, std::wstring> g_serve;             // the document Pluma answers in each tab's main frame
static std::set<int> g_tell;                            // tabs to tell, once loaded, how to come back to the light one
static const wchar_t *kSite = L"https://www.youtube.com";

static std::wstring no_fragment(const std::wstring &u) { return u.substr(0, u.find(L'#')); }
// an address YouTube's own page was asked for, as remembered: YouTube's page loads itself again with themeRefresh=1
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
static bool youtube_host(const std::wstring &host) {
    return host == L"www.youtube.com" || host == L"youtube.com" || host == L"m.youtube.com" || host == L"youtu.be";
}

// the pages the light one does (anything else is YouTube's): /, /results, /feed/subscriptions, /feed/history,
// /playlist, /watch (with Pluma's player there), and a channel's home or videos (/@name, /channel/UC..., /c/, /user/)
static bool lite_url(const std::wstring &url) {
    if (!starts_with(url, L"https://www.youtube.com/")) return false;
    std::wstring rest = url.substr(wcslen(kSite));
    size_t q = rest.find_first_of(L"?#");
    std::wstring path = rest.substr(0, q), query = q == std::wstring::npos ? L"" : rest.substr(q);
    if (path.size() > 1 && path.back() == L'/') path.pop_back();
    auto has = [&](const wchar_t *k) {
        std::wstring a = L"?" + std::wstring(k) + L"=", b = L"&" + std::wstring(k) + L"=";
        size_t at = query.find(a);
        if (at == std::wstring::npos) at = query.find(b);
        return at != std::wstring::npos && at + a.size() < query.size() && query[at + a.size()] != L'&' && query[at + a.size()] != L'#';
    };
    if (path == L"/") return true;
    if (path == L"/results") return has(L"search_query");
    if (path == L"/feed/subscriptions" || path == L"/feed/history") return true;
    if (path == L"/playlist") return has(L"list");
    if (path == L"/watch") return has(L"v") && g_set.native_video && video_available();
    std::vector<std::wstring> parts;
    for (size_t i = 1; i <= path.size();) {
        size_t e = path.find(L'/', i);
        if (e == std::wstring::npos) e = path.size();
        if (e > i) parts.push_back(path.substr(i, e - i));
        i = e + 1;
    }
    size_t base = 0;
    if (!parts.empty() && parts[0].size() > 1 && parts[0][0] == L'@') base = 1;
    else if (parts.size() >= 2 && (parts[0] == L"channel" || parts[0] == L"c" || parts[0] == L"user")) base = 2;
    else return false;
    if (parts.size() == base) return true;
    return parts.size() == base + 1 && (parts[base] == L"videos" || parts[base] == L"featured");
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

// the page, with what it needs from Pluma written into it: the theme, the language, and its texts in that language
// (written in Spanish in the page, as t('...'), and translated with T() as every text of Pluma)
static std::string page_html() {
    static std::string raw;
    static std::vector<std::wstring> words;
    if (raw.empty()) {
        HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(203), RT_RCDATA);
        HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
        if (!g) return "";
        raw.assign((const char *)LockResource(g), SizeofResource(g_inst, r));
        std::set<std::string> seen;
        for (size_t i = raw.find("t('"); i != std::string::npos; i = raw.find("t('", i + 3)) {
            char before = i ? raw[i - 1] : ' ';
            if (isalnum((unsigned char)before) || before == '_' || before == '$' || before == '.') continue;
            size_t e = raw.find('\'', i + 3);
            if (e == std::string::npos || e - i > 300) continue;
            std::string w = raw.substr(i + 3, e - i - 3);
            if (seen.insert(w).second) words.push_back(wide(w));
        }
    }
    std::wstring cfg = L"{\"dark\":" + std::wstring(dark_theme() ? L"true" : L"false") + L",\"hl\":" + json_escape(lang_code()) + L",\"t\":{";
    bool first = true;
    for (auto &w : words) {
        const wchar_t *tr = T(w.c_str());
        if (w == tr) continue;
        cfg += (first ? L"" : L",") + json_escape(w) + L":" + json_escape(tr);
        first = false;
    }
    cfg += L"}}";
    const char *a = "/*PLUMA_CFG*/", *b = "/*PLUMA_CFG_FIN*/";
    size_t s = raw.find(a), e = raw.find(b);
    if (s == std::string::npos || e == std::string::npos || e < s) return raw;
    return raw.substr(0, s) + utf8(cfg) + raw.substr(e + strlen(b));
}

static void serve(ICoreWebView2WebResourceRequestedEventArgs *a) {
    std::string body = page_html();
    if (body.empty() || !g_env) return;   // (then YouTube's own)
    IStream *s = SHCreateMemStream((const BYTE *)body.data(), (UINT)body.size());
    ComPtr<ICoreWebView2WebResourceResponse> res;
    if (s && SUCCEEDED(g_env->CreateWebResourceResponse(s, 200, L"OK",
            L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff", &res)))
        a->put_Response(res.Get());
    if (s) s->Release();
}

void youtube_attach(Tab *t) {
    int id = t->id;
    ICoreWebView2 *v = t->view.Get();
    if (!v) return;
    // documents only (the page's own calls go to YouTube untouched); and those a service worker of YouTube's asks for
    // in the page's name, if it has one
    ComPtr<ICoreWebView2_22> v22;
    if (SUCCEEDED(t->view.As(&v22)))
        v22->AddWebResourceRequestedFilterWithRequestSourceKinds(L"https://www.youtube.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT,
                                                                 COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
    else v->AddWebResourceRequestedFilter(L"https://www.youtube.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
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
            if (t->video && t->yt_lite) video_back_to_page(t, VIDEO_LEAVE);   // (the light page's player goes with it)
            int kind = 0;
            std::wstring clean = take_mark(u, kind);
            if (kind && starts_with(u, L"https://www.youtube.com/")) {
                // YouTube's own page, asked for by the light one: without the mark, and remembered
                a->put_Cancel(TRUE);
                if (kind >= 2) { g_normal[id] = true; g_tell.insert(id); }
                else g_pass[id].insert(pass_key(clean));
                t->yt_lite = false;
                g_serve.erase(id);
                log_line(L"YouTube ligero: la página normal" + std::wstring(kind >= 2 ? L" en esta pestaña" : L" para esta dirección"));
                ui_post([id, clean] { Tab *t = tab_by_id(id); if (t && t->view) t->view->Navigate(clean.c_str()); });
                return S_OK;
            }
            if (!youtube_host(url_host(u))) { g_normal.erase(id); g_pass.erase(id); g_tell.erase(id); }
            std::wstring nf = no_fragment(u);
            auto p = g_pass.find(id);
            bool lite = g_set.youtube_lite && !g_normal.count(id) && !(p != g_pass.end() && p->second.count(pass_key(nf))) && lite_url(nf);
            t->yt_lite = lite;
            if (lite) g_serve[id] = nf; else g_serve.erase(id);
            return S_OK;
        }).Get(), &tok);
    v->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *a) -> HRESULT {
            auto it = g_serve.find(id);
            if (it == g_serve.end()) return S_OK;
            ComPtr<ICoreWebView2WebResourceRequest> req;
            LPWSTR uri = NULL, method = NULL;
            if (FAILED(a->get_Request(&req)) || FAILED(req->get_Uri(&uri))) return S_OK;
            std::wstring u = uri; CoTaskMemFree(uri);
            req->get_Method(&method);
            bool get = method && !wcscmp(method, L"GET");
            CoTaskMemFree(method);
            if (get && no_fragment(u) == it->second) {
                serve(a);
                static int told = 0;   // (a few lines: to know, next time, whether a page that stayed YouTube's passed through here)
                if (told++ < 20) log_line(L"YouTube ligero: servida " + u.substr(0, u.find_first_of(L"?#")));
            }
            return S_OK;
        }).Get(), &tok);
    v->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
            BOOL ok = FALSE; a->get_IsSuccess(&ok);
            if (!ok || !g_tell.count(id)) return S_OK;
            g_tell.erase(id);
            ui_notice(T(L"Esta pestaña muestra el YouTube normal."), T(L"Volver a YouTube ligero"), [id] {
                Tab *t = tab_by_id(id);
                g_normal.erase(id);
                g_pass.erase(id);
                if (t && t->view) t->view->Navigate(t->url.c_str());
            }, 12, id);
            return S_OK;
        }).Get(), &tok);
}

void youtube_settings_changed() {
    // off: the light pages open become YouTube's own (they load again); on: from the next YouTube page
    if (g_set.youtube_lite) return;
    for (auto &t : g_tabs) if (t->yt_lite && t->view) t->view->Reload();
}
