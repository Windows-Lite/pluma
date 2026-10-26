// Pluma: the reader view, as Firefox's Reader View or Edge's Immersive Reader. Mozilla's Readability (readability/,
// Apache-2.0, unchanged; RCDATA 211 and 212) does the work:
// - once a page has its document (DOMContentLoaded; again when it finishes loading, and after an address change made
//   by the page itself), isProbablyReaderable runs in it (1-3 ms). An article (not a site's home) shows the book in the
//   address bar (ui.cpp).
// - the book or F9: Readability parses a copy of the document in the page (15-70 ms) and Pluma loads the same address
//   again, answering that one document itself (WebResourceRequested, as youtube.cpp) with reader.html (RCDATA 210)
//   and the article written into it. Same address and origin: the address bar, the star, history, the tab's icon and
//   the article's images work as before, and none of the page's own scripts run. The page's CSP allows only its own
//   script (a nonce per page) and it keeps only an allowlist of the article's HTML (clean() in reader.html).
// - the book, F9 or "Salir del modo lectura": the page loads again, its own (the answer is served once).
// - the reader page speaks to Pluma through requests to https://lector.pluma.local/<token>?... (no page may message
//   Pluma): the font size (lectura_letra in ajustes.ini), "Abrir siempre en modo lectura" for its site (lectura.tsv,
//   one host a line; then an article of that site opens straight in the reader view) and leaving.
#include "pluma.h"
#include "json.h"
#include <bcrypt.h>
#include <map>
#include <set>

struct Pending { std::wstring nf, article; };        // a reader page about to be served (the address, the article's JSON)
static std::map<int, Pending> g_pending;              // ... in each tab
static std::map<int, std::wstring> g_token;           // the last reader page served in each tab
static std::map<UINT_PTR, int> g_recheck;             // timers: an address the page changed itself, checked again
static std::set<std::wstring> g_always;               // sites opened straight in the reader view
static bool g_always_read = false;
static const wchar_t *kBeacon = L"https://lector.pluma.local/";

static std::wstring no_fragment(const std::wstring &u) { return u.substr(0, u.find(L'#')); }
static std::wstring site_of(const std::wstring &url) {
    std::wstring h = lower(url_host(url));
    return starts_with(h, L"www.") ? h.substr(4) : h;
}
static std::string resource(int id) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    return g ? std::string((const char *)LockResource(g), SizeofResource(g_inst, r)) : std::string();
}
static std::wstring random_hex() {
    BYTE b[16] = {};
    BCryptGenRandom(NULL, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    wchar_t s[40] = L"";
    for (int i = 0; i < 16; i++) swprintf_s(s + i * 2, 3, L"%02x", b[i]);
    return s;
}

// ---------------------------------------------------------------- the sites opened straight in the reader view
static void always_read() {
    if (g_always_read) return;
    g_always_read = true;
    for (auto &f : tsv_read(L"lectura.tsv")) if (!f[0].empty()) g_always.insert(lower(f[0]));
}
std::vector<std::wstring> reader_sites() { always_read(); return std::vector<std::wstring>(g_always.begin(), g_always.end()); }
void reader_site_set(const std::wstring &site, bool on) {
    always_read();
    if (g_incognito || site.empty()) return;   // (incognito keeps nothing of where it went)
    if (on) g_always.insert(site); else g_always.erase(site);
    std::wstring out;
    for (auto &s : g_always) out += s + L"\n";
    write_file(g_state + L"\\lectura.tsv", utf8(out));
    settings_page_refresh_soon();
}

// ---------------------------------------------------------------- the scripts run in the page
static const std::wstring &check_js() {
    // is this an article? (and is it Pluma's own reader page, by its token); "art": the page says it is one, for the
    // automatic reader view (a site's home or a section can look like an article to Readability)
    static std::wstring js;
    if (js.empty())
        js = L"(() => { var module;\n" + wide(resource(212)) + L"\n"
             L"const m = document.querySelector('meta[name=\"pluma-reader\"]');"
             L"const og = document.querySelector('meta[property=\"og:type\"]');"
             L"let art = !!(og && /article/i.test(og.content || ''));"
             L"if (!art) for (const s of document.querySelectorAll('script[type=\"application/ld+json\"]'))"
             L"  if (/\"@type\"\\s*:\\s*\\[?\\s*\"(News|Blog|Tech|Scholarly|Report|Opinion|Analysis)?(Article|Posting)\"/.test(s.textContent)) { art = true; break; }"
             L"let ok = false; try { ok = location.pathname.length > 1 && isProbablyReaderable(document); } catch (e) {}"
             L"return { r: m ? m.content : '', ok, art }; })()";
    return js;
}
static const std::wstring &parse_js() {
    static std::wstring js;
    if (js.empty())
        js = L"(() => { var module;\n" + wide(resource(211)) + L"\n"
             L"try { const a = new Readability(document.cloneNode(true)).parse();"
             L" if (!a || !a.content) return null;"
             L" return { title: a.title, byline: a.byline, siteName: a.siteName, publishedTime: a.publishedTime, content: a.content,"
             L"          length: a.length, lang: a.lang || document.documentElement.lang || '', dir: a.dir || '' };"
             L"} catch (e) { return null; } })()";
    return js;
}

static bool can_read(Tab *t) {
    return t && t->view && !t->yt_lite && !settings_page_url(t->url) && (starts_with(t->url, L"https://") || starts_with(t->url, L"http://"));
}

static void filter(Tab *t, const std::wstring &nf, bool add) {
    // documents only, also those a service worker of the site asks for in the page's name (as youtube.cpp)
    ComPtr<ICoreWebView2_22> v22;
    if (SUCCEEDED(t->view.As(&v22))) {
        if (add) v22->AddWebResourceRequestedFilterWithRequestSourceKinds(nf.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
        else v22->RemoveWebResourceRequestedFilterWithRequestSourceKinds(nf.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
    } else if (add) t->view->AddWebResourceRequestedFilter(nf.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
    else t->view->RemoveWebResourceRequestedFilter(nf.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
}
static void drop_pending(Tab *t) {
    auto p = g_pending.find(t->id);
    if (p == g_pending.end()) return;
    if (t->view) filter(t, p->second.nf, false);
    g_pending.erase(p);
}

static void open_reader(Tab *t, bool automatic) {
    if (!can_read(t) || g_pending.count(t->id)) return;
    int id = t->id;
    std::wstring url = t->url;
    double t0 = now_ms();
    t->view->ExecuteScript(parse_js().c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [id, url, automatic, t0](HRESULT hr, LPCWSTR json) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t || !t->view || t->url != url) return S_OK;
            std::wstring a = SUCCEEDED(hr) && json ? json : L"null";
            if (a == L"null" || a.size() < 10) {
                if (!automatic) ui_toast(T(L"Esta página no se puede mostrar en modo lectura."));
                return S_OK;
            }
            log_line(L"modo lectura: artículo leído en " + std::to_wstring((int)(now_ms() - t0)) + L" ms (" + std::to_wstring(a.size() / 1024) + L" KB)" +
                     (automatic ? L", automático" : L""));
            std::wstring nf = no_fragment(url);
            g_pending[id] = { nf, a };
            filter(t, nf, true);
            t->view->Navigate(nf.c_str());
            return S_OK;
        }).Get());
}

// the words of reader.html, in the person's language
static const wchar_t *kWords[] = { L"Letra más pequeña", L"Letra más grande", L"Abrir siempre en modo lectura", L"Salir del modo lectura",
                                   L"min de lectura", L"Esta página no se puede mostrar en modo lectura." };

static std::string page_for(Tab *t, const Pending &p, const std::wstring &token, std::wstring &nonce) {
    std::string html = resource(210);
    if (html.empty()) return html;
    nonce = random_hex();
    auto put = [&](const char *what, const std::string &with) {
        size_t at = html.find(what);
        if (at != std::string::npos) html.replace(at, strlen(what), with);
    };
    put("PLUMA_TOKEN", utf8(token));
    put("PLUMA_NONCE", utf8(nonce));
    always_read();
    auto b = [](bool x) { return x ? L"true" : L"false"; };
    std::wstring site = site_of(p.nf);
    std::wstring cfg = L"{\"token\":" + json_escape(token) + L",\"dark\":" + b(dark_theme()) + L",\"hl\":" + json_escape(lang_code()) +
                       L",\"size\":" + std::to_wstring(g_set.reader_size) + L",\"always\":" + b(g_always.count(site) > 0) +
                       L",\"incognito\":" + b(t->incognito) + L",\"host\":" + json_escape(site) + L",\"base\":" + json_escape(p.nf) + L",\"t\":{";
    bool first = true;
    for (auto w : kWords) { cfg += (first ? L"" : L",") + json_escape(w) + L":" + json_escape(T(w)); first = false; }
    cfg += L"}}";
    // inside a <script> block: no "<" at all ("</script>" in an article would end it); in JSON a "<" can only be in a
    // string, where < is the same
    std::string data = utf8(L"{\"a\":" + p.article + L",\"cfg\":" + cfg + L"}"), safe;
    safe.reserve(data.size() + 256);
    for (char c : data) { if (c == '<') safe += "\\u003c"; else safe += c; }
    put("/*PLUMA_DATA*/", safe);
    return html;
}
static void respond(ICoreWebView2WebResourceRequestedEventArgs *a, int status, const wchar_t *reason, const std::string &body, const std::wstring &headers) {
    if (!g_env) return;
    IStream *s = SHCreateMemStream((const BYTE *)body.data(), (UINT)body.size());
    ComPtr<ICoreWebView2WebResourceResponse> res;
    if (s && SUCCEEDED(g_env->CreateWebResourceResponse(s, status, reason, headers.c_str(), &res))) a->put_Response(res.Get());
    if (s) s->Release();
}

// what the reader page says (lector.pluma.local/<token>?size=18 | always=1 | exit=1); only the page that tab was given
static void heard(int id, const std::wstring &u) {
    std::wstring rest = u.substr(wcslen(kBeacon));
    size_t q = rest.find(L'?');
    if (q == std::wstring::npos) return;
    auto tok = g_token.find(id);
    Tab *t = tab_by_id(id);
    if (!t || tok == g_token.end() || rest.substr(0, q) != tok->second || !t->reader) return;
    std::wstring query = rest.substr(q + 1);
    size_t eq = query.find(L'=');
    std::wstring key = query.substr(0, eq), val = eq == std::wstring::npos ? L"" : query.substr(eq + 1);
    if (key == L"size") {
        int s = _wtoi(val.c_str());
        if (s >= 12 && s <= 40 && s != g_set.reader_size) { g_set.reader_size = s; settings_save(); }
    } else if (key == L"always") reader_site_set(site_of(t->url), val == L"1");
    else if (key == L"exit") ui_post([id] { reader_toggle(tab_by_id(id)); });
}

static void check(Tab *t) {
    if (!can_read(t)) {
        if (t && (t->readerable || t->reader)) { t->readerable = t->reader = false; ui_invalidate(); }
        return;
    }
    int id = t->id;
    std::wstring url = t->url;
    t->view->ExecuteScript(check_js().c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [id, url](HRESULT hr, LPCWSTR json) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t || t->url != url || FAILED(hr) || !json) return S_OK;
            Json j = json_parse(json);
            auto tok = g_token.find(id);
            bool reader = !j[L"r"].s.empty() && tok != g_token.end() && j[L"r"].s == tok->second;
            bool ok = reader || j[L"ok"].b;
            if (reader != t->reader || ok != t->readerable) { t->reader = reader; t->readerable = ok; ui_invalidate(); }
            always_read();
            if (!reader && ok && j[L"art"].b && g_always.count(site_of(url)) && no_fragment(t->reader_declined) != no_fragment(url))
                open_reader(t, true);
            return S_OK;
        }).Get());
}

void reader_toggle(Tab *t) {
    // the book or F9: into the reader view, or back to the page
    if (!t || !t->view) return;
    if (t->reader) {
        t->reader_declined = t->url;   // (its site may open in the reader view by itself: not this time)
        t->reader = false;
        g_token.erase(t->id);
        drop_pending(t);
        t->view->Reload();
        ui_invalidate();
    } else if (t->readerable) open_reader(t, false);
}

void reader_attach(Tab *t) {
    int id = t->id;
    ICoreWebView2 *v = t->view.Get();
    if (!v) return;
    EventRegistrationToken tok;
    v->AddWebResourceRequestedFilter((std::wstring(kBeacon) + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    v->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            LPWSTR uri = NULL;
            a->get_Uri(&uri);
            std::wstring u = uri ? uri : L"";
            CoTaskMemFree(uri);
            std::wstring nf = no_fragment(u);
            // (a link to a part of the article keeps the reader view)
            bool part = t->reader && u.find(L'#') != std::wstring::npos && nf == no_fragment(t->url);
            if (!part && (t->reader || t->readerable)) { t->reader = t->readerable = false; ui_invalidate(); }
            auto p = g_pending.find(id);
            if (p != g_pending.end() && p->second.nf != nf) drop_pending(t);
            if (!t->reader_declined.empty() && no_fragment(t->reader_declined) != nf) t->reader_declined.clear();
            return S_OK;
        }).Get(), &tok);
    v->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *a) -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> req;
            LPWSTR uri = NULL, method = NULL;
            if (FAILED(a->get_Request(&req)) || FAILED(req->get_Uri(&uri))) return S_OK;
            std::wstring u = uri; CoTaskMemFree(uri);
            if (starts_with(u, kBeacon)) {
                // never goes out (.local is not on the internet anyway)
                heard(id, u);
                respond(a, 204, L"No Content", "", L"Cache-Control: no-store");
                return S_OK;
            }
            auto p = g_pending.find(id);
            if (p == g_pending.end() || no_fragment(u) != p->second.nf) return S_OK;
            req->get_Method(&method);
            bool get = method && !wcscmp(method, L"GET");
            CoTaskMemFree(method);
            Tab *t = tab_by_id(id);
            if (!get || !t) return S_OK;
            std::wstring token = random_hex(), nonce;
            std::string body = page_for(t, p->second, token, nonce);
            drop_pending(t);   // (served once: reloading gives the page its own again)
            if (body.empty()) return S_OK;
            respond(a, 200, L"OK", body,
                    L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                    L"Content-Security-Policy: default-src 'none'; script-src 'nonce-" + nonce + L"'; style-src 'unsafe-inline'; "
                    L"img-src https: http: data:; connect-src 'none'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'");
            g_token[id] = token;
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2_2> v2;
    if (SUCCEEDED(t->view.As(&v2)))
        v2->add_DOMContentLoaded(Callback<ICoreWebView2DOMContentLoadedEventHandler>(
            [id](ICoreWebView2 *, ICoreWebView2DOMContentLoadedEventArgs *) -> HRESULT { check(tab_by_id(id)); return S_OK; }).Get(), &tok);
    v->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
            // what was still loading at DOMContentLoaded may make it an article
            BOOL ok = FALSE; a->get_IsSuccess(&ok);
            Tab *t = tab_by_id(id);
            if (ok && t && !t->readerable) check(t);
            return S_OK;
        }).Get(), &tok);
    v->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2SourceChangedEventArgs *a) -> HRESULT {
            // the page changed its own address (sites that load articles without loading a page): look again once it
            // has drawn the new one
            BOOL fresh = TRUE; a->get_IsNewDocument(&fresh);
            if (fresh) return S_OK;
            UINT_PTR tm = SetTimer(NULL, 0, 1500, [](HWND, UINT, UINT_PTR tm, DWORD) {
                KillTimer(NULL, tm);
                auto it = g_recheck.find(tm);
                if (it == g_recheck.end()) return;
                int id = it->second;
                g_recheck.erase(it);
                check(tab_by_id(id));
            });
            if (tm) g_recheck[tm] = id;
            return S_OK;
        }).Get(), &tok);
}
