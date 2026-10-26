// Pluma: page translation, as Chrome's and Edge's translator (WebView2 brings none).
// - The page's language is found locally: its lang attribute and its own words (translate_core.h); nothing of the
//   page goes anywhere until the person asks. A page in another language than the target (Pluma's language unless
//   chosen) lights the translate icon in the address bar and, once per site and language, offers it in a small
//   popup (ui.cpp): Translate / Not now / Never this language / Never this site / Always this language.
// - Translating: translate.js (RCDATA 221) goes into the page and hands out its text in pieces (a block's text
//   nodes, the visible ones first); Pluma sends them to Google Translate's web endpoint (translate.googleapis.com,
//   client=gtx, as Chrome's own; no account), from a worker thread with WinHTTP (the page's CSP never gets in the way),
//   and gives the page back plain strings for the same text nodes. New text keeps being translated while the page is
//   on screen: Pluma asks it every 700 ms (nothing is asked of a hidden, frozen or asleep page). "Show original" puts
//   every node back. A page translated in a tab carries on into the next pages of that language in the same tab.
// - Selected text: "Traducir «...»" in the page's context menu, the result in a small bubble near it, with a copy
//   button (ui.cpp).
// - Fails gracefully: offline or rate-limited, a toast, and what was translated stays.
// - Settings (Configuración > Idioma): offers on/off, the target, the languages always or never translated
//   (ajustes.ini: traducir_ofrecer, traducir_a, traducir_siempre, traducir_nunca) and the sites never translated
//   (traducir-nunca.tsv, one host a line, each profile its own; an incognito window keeps them only in memory).
#include "pluma.h"
#include "json.h"
#include "translate_core.h"
#include <winhttp.h>
#include <map>
#include <set>
#include <thread>
#include <mutex>

// the languages offered as targets (Google's codes); the settings page offers the same
static const wchar_t *kTargets[] = { L"es", L"ca", L"en", L"fr", L"de", L"it", L"pt", L"nl", L"pl", L"ru", L"uk", L"tr", L"ar", L"ja", L"ko", L"zh-CN" };

static std::string resource(int id) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    return g ? std::string((const char *)LockResource(g), SizeofResource(g_inst, r)) : std::string();
}
static const std::wstring &translator_js() { static std::wstring js = wide(resource(221)); return js; }
static std::wstring site_of(const std::wstring &url) {
    std::wstring h = lower(url_host(url));
    return starts_with(h, L"www.") ? h.substr(4) : h;
}
static bool has(const std::vector<std::wstring> &v, const std::wstring &x) { return std::find(v.begin(), v.end(), x) != v.end(); }
static std::wstring fmt1(const wchar_t *tpl, const std::wstring &a) {
    std::wstring s = tpl;
    size_t at = s.find(L"%1");
    if (at != std::wstring::npos) s.replace(at, 2, a);
    return s;
}

// ---------------------------------------------------------------- language names, in Pluma's language
// Windows's own ICU (icu.dll, Windows 10 1903 and later) names any language in any other; without it, the
// language's own name
typedef int (__cdecl *DisplayLanguageFn)(const char *locale, const char *display, wchar_t *out, int cap, int *err);
std::wstring translate_lang_name(const std::wstring &code, bool capital) {
    std::wstring p = tr_primary(code);
    if (p.empty()) return code;
    static std::map<std::wstring, std::wstring> cache;
    std::wstring key = std::wstring(lang_code()) + L"|" + p;
    auto it = cache.find(key);
    std::wstring name;
    if (it != cache.end()) name = it->second;
    else {
        static DisplayLanguageFn fn = [] {
            HMODULE m = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return m ? (DisplayLanguageFn)GetProcAddress(m, "uloc_getDisplayLanguage") : (DisplayLanguageFn)nullptr;
        }();
        wchar_t buf[96] = L"";
        int err = 0, n = fn ? fn(utf8(p).c_str(), utf8(lang_code()).c_str(), buf, 96, &err) : 0;
        if (fn && err <= 0 && n > 0 && n < 96 && p != buf) name.assign(buf, n);
        else if (GetLocaleInfoEx(p.c_str(), LOCALE_SNATIVELANGUAGENAME, buf, 96) > 0) name = buf;
        else name = p;
        cache[key] = name;
    }
    if (capital && !name.empty()) CharUpperBuffW(&name[0], 1);
    return name;
}
std::vector<std::wstring> translate_targets() { return std::vector<std::wstring>(std::begin(kTargets), std::end(kTargets)); }
std::wstring translate_target() {
    if (!g_set.tr_target.empty() && has(translate_targets(), g_set.tr_target)) return g_set.tr_target;
    return lang_code();
}
void translate_set_target(const std::wstring &code) {
    g_set.tr_target = code == lang_code() ? L"" : code;
    settings_save();
}

// ---------------------------------------------------------------- the person's choices
static std::set<std::wstring> g_never_sites;
static bool g_never_read = false;
static std::set<std::wstring> g_offered;   // site|language already offered in this window (the icon stays; the popup, once)
static void never_read() {
    if (g_never_read) return;
    g_never_read = true;
    for (auto &f : tsv_read(L"traducir-nunca.tsv")) if (!f[0].empty()) g_never_sites.insert(lower(f[0]));
}
std::vector<std::wstring> translate_never_sites() { never_read(); return std::vector<std::wstring>(g_never_sites.begin(), g_never_sites.end()); }
bool translate_never_site(const std::wstring &site) { never_read(); return g_never_sites.count(site) > 0; }
void translate_set_never_site(const std::wstring &site, bool on) {
    never_read();
    if (site.empty()) return;
    if (on) g_never_sites.insert(site); else g_never_sites.erase(site);
    if (!g_incognito) {   // (incognito keeps nothing of where it went: only for this window)
        std::wstring out;
        for (auto &s : g_never_sites) out += s + L"\n";
        write_file(g_data + L"\\traducir-nunca.tsv", utf8(out));
    }
    settings_page_refresh_soon();
}
bool translate_always(const std::wstring &lang) { return has(g_set.tr_always, lang); }
bool translate_never(const std::wstring &lang) { return has(g_set.tr_never, lang); }
static void set_in(std::vector<std::wstring> &v, const std::wstring &x, bool on) {
    auto it = std::find(v.begin(), v.end(), x);
    if (on && it == v.end()) v.push_back(x);
    if (!on && it != v.end()) v.erase(it);
}
void translate_set_always(const std::wstring &lang, bool on) {
    if (lang.empty()) return;
    set_in(g_set.tr_always, lang, on);
    if (on) set_in(g_set.tr_never, lang, false);
    settings_save();
}
void translate_set_never(const std::wstring &lang, bool on) {
    if (lang.empty()) return;
    set_in(g_set.tr_never, lang, on);
    if (on) set_in(g_set.tr_always, lang, false);
    settings_save();
}
std::wstring translate_site(Tab *t) { return t ? site_of(t->url) : L""; }

// ---------------------------------------------------------------- Google Translate (worker threads)
static HINTERNET http_session() {
    static std::once_flag once;
    static HINTERNET s = NULL;
    std::call_once(once, [] {
        s = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (s) WinHttpSetTimeouts(s, 4000, 5000, 8000, 15000);   // (short: offline says so in seconds, not a minute)
    });
    return s;
}
// 0: answered (out); else the HTTP status, or -1 without a connection
static int tr_post(const std::wstring &tl, bool html, const std::vector<std::wstring> &qs, std::string &out) {
    HINTERNET s = http_session();
    if (!s) return -1;
    std::wstring path = L"/translate_a/t?client=gtx&sl=auto&tl=" + url_encode(tl) + (html ? L"&format=html" : L"&format=text");
    std::string body;
    for (auto &q : qs) body += (body.empty() ? "q=" : "&q=") + utf8(url_encode(q));
    int result = -1;
    HINTERNET c = WinHttpConnect(s, L"translate.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    const wchar_t *hdr = L"Content-Type: application/x-www-form-urlencoded;charset=UTF-8";
    if (r && WinHttpSendRequest(r, hdr, (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        char buf[16384];
        bool read_ok = true;
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(r, buf, sizeof(buf), &got)) { read_ok = false; break; }
            if (!got) break;
            out.append(buf, got);
            if (out.size() > (8 << 20)) { read_ok = false; break; }
        }
        result = code == 200 && read_ok ? 0 : code ? (int)code : -1;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    return result;
}
// [["texto","fr"], ...] with sl=auto (or ["texto", ...]): the n translations, and the language of the first
static bool parse_answer(const std::string &body, size_t n, std::vector<std::wstring> &texts, std::wstring *src) {
    Json j = json_parse(wide(body));
    if (j.kind != Json::ARR) return false;
    texts.clear();
    if (n == 1 && j.arr.size() == 2 && j.arr[0].is_str() && j.arr[1].is_str()) {
        texts.push_back(j.arr[0].s);
        if (src) *src = j.arr[1].s;
        return true;
    }
    if (j.arr.size() != n) return false;
    for (auto &e : j.arr) {
        if (e.is_str()) texts.push_back(e.s);
        else if (e.kind == Json::ARR && !e.arr.empty() && e.arr[0].is_str()) {
            texts.push_back(e.arr[0].s);
            if (src && src->empty() && e.arr.size() > 1 && e.arr[1].is_str()) *src = e.arr[1].s;
        } else return false;
    }
    return true;
}
static std::wstring failure_text(int status) {
    if (status == -1) return T(L"No se pudo traducir: no hay conexión.");
    if (status == 429 || status == 403 || status == 503) return T(L"El traductor no responde ahora. Inténtalo dentro de un rato.");
    return T(L"No se pudo traducir la página.");
}

// ---------------------------------------------------------------- translating a page
struct Piece { int id; std::wstring html; size_t nodes; };
struct Job {
    bool on = false, busy = false;
    int gen = 0;                 // a new start, "show original" or a new document: answers of before are dropped
    std::wstring tl;
    int total = 0, done = 0, left = 0;
    int pending = 0, fail = 0;   // requests of this round still out; the first failure's status
    std::wstring put;            // the answers of this round, for the page: [id,[...]],...
};
static std::map<int, Job> g_jobs;
static UINT_PTR g_poll = 0;

static bool can_translate(Tab *t) {
    return t && t->view && !settings_page_url(t->url) && (starts_with(t->url, L"https://") || starts_with(t->url, L"http://"));
}
static void stats(Job &j, const Json &r) {
    if (r[L"total"].kind == Json::NUM) j.total = (int)r[L"total"].n;
    if (r[L"done"].kind == Json::NUM) j.done = (int)r[L"done"].n;
    if (r[L"left"].kind == Json::NUM) j.left = (int)r[L"left"].n;
}
int translate_progress(Tab *t) {
    auto it = t ? g_jobs.find(t->id) : g_jobs.end();
    if (it == g_jobs.end() || it->second.total <= 0) return 0;
    return std::min(100, it->second.done * 100 / it->second.total);
}
static void stop(int id, int status) {
    auto it = g_jobs.find(id);
    Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
    if (it == g_jobs.end() || !t) return;
    Job &j = it->second;
    j.on = false;
    j.busy = false;
    log_line(L"traducción: parada (" + std::to_wstring(status) + L"), " + std::to_wstring(j.done) + L" de " + std::to_wstring(j.total) + L" trozos");
    if (t->view) t->view->ExecuteScript(L"window.__plumaTr && __plumaTr.requeue()", nullptr);
    t->tr_state = j.done > 0 ? 2 : 0;
    ui_translate_changed();
    ui_toast(failure_text(status));
}
static void pump(int id);
static void round_done(int id, int gen) {
    // every request of the round answered: the page gets them all at once, then the next round
    auto it = g_jobs.find(id);
    Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
    if (it == g_jobs.end() || it->second.gen != gen || !t || !t->view) return;
    Job &j = it->second;
    std::wstring js = L"window.__plumaTr ? __plumaTr.put([" + j.put + L"]) : null";
    j.put.clear();
    int fail = j.fail;
    t->view->ExecuteScript(js.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [id, gen, fail](HRESULT hr, LPCWSTR json) -> HRESULT {
            auto it = g_jobs.find(id);
            if (it == g_jobs.end() || it->second.gen != gen) return S_OK;
            Job &j = it->second;
            j.busy = false;
            if (SUCCEEDED(hr) && json) stats(j, json_parse(json));
            if (fail) { stop(id, fail); return S_OK; }
            ui_translate_changed();
            pump(id);
            return S_OK;
        }).Get());
}
static void got(int id, int gen, const std::vector<Piece> &ps, int status, const std::vector<std::wstring> &answers) {
    auto it = g_jobs.find(id);
    if (it == g_jobs.end() || it->second.gen != gen) return;
    Job &j = it->second;
    for (size_t i = 0; i < ps.size(); i++) {
        if (!j.put.empty()) j.put += L",";
        j.put += L"[" + std::to_wstring(ps[i].id) + L",";
        if (status) { j.put += L"null]"; continue; }
        j.put += L"[";
        auto segs = tr_split_markers(answers[i], ps[i].nodes);
        for (size_t k = 0; k < segs.size(); k++) j.put += (k ? L"," : L"") + json_escape(segs[k]);
        j.put += L"]]";
    }
    if (status && !j.fail) j.fail = status;
    if (--j.pending <= 0) round_done(id, gen);
}
static void pump(int id) {
    // the next pieces of the page (up to ~12,000 characters), in up to three requests at once
    auto it = g_jobs.find(id);
    Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
    if (it == g_jobs.end() || !t || !t->view) return;
    Job &j = it->second;
    if (!j.on || j.busy) return;
    j.busy = true;
    int gen = j.gen;
    t->view->ExecuteScript(L"window.__plumaTr ? __plumaTr.take(12000) : null", Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [id, gen](HRESULT hr, LPCWSTR json) -> HRESULT {
            auto it = g_jobs.find(id);
            Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
            if (it == g_jobs.end() || it->second.gen != gen || !t) return S_OK;
            Job &j = it->second;
            j.busy = false;
            Json r = SUCCEEDED(hr) && json ? json_parse(json) : Json();
            if (r.kind != Json::OBJ) return S_OK;   // (no translator in the page: a new document, the job goes with it)
            stats(j, r);
            const Json &q = r[L"q"];
            if (q.kind != Json::ARR || q.arr.empty()) {
                if (t->tr_state == 1 && j.left == 0) {
                    t->tr_state = 2;
                    log_line(L"traducción: hecha, " + std::to_wstring(j.total) + L" trozos");
                    ui_translate_changed();
                }
                return S_OK;
            }
            std::vector<std::vector<Piece>> groups(1);
            size_t chars = 0;
            for (auto &e : q.arr) {
                if (e.kind != Json::ARR || e.arr.size() < 3 || !e.arr[1].is_str()) continue;
                Piece p{ (int)e.arr[0].n, e.arr[1].s.substr(0, 6000), (size_t)std::max(1.0, std::min(200.0, e.arr[2].n)) };
                if (!groups.back().empty() && (chars + p.html.size() > 5000 || groups.back().size() >= 100)) { groups.emplace_back(); chars = 0; }
                chars += p.html.size();
                groups.back().push_back(p);
            }
            if (groups.back().empty()) return S_OK;
            j.busy = true;
            j.pending = (int)groups.size();
            j.fail = 0;
            j.put.clear();
            std::wstring tl = j.tl;
            for (auto &g : groups)
                std::thread([id, gen, tl, g] {
                    std::vector<std::wstring> qs, answers;
                    for (auto &p : g) qs.push_back(p.html);
                    std::string body;
                    int st = tr_post(tl, true, qs, body);
                    if (st == 0 && !parse_answer(body, g.size(), answers, nullptr)) st = 502;
                    ui_post([id, gen, g, st, answers] { got(id, gen, g, st, answers); });
                }).detach();
            return S_OK;
        }).Get());
}
static void CALLBACK poll(HWND, UINT, UINT_PTR, DWORD) {
    // the page on screen, while translated: its new text (dynamic pages) gets translated too
    bool any = false;
    for (auto it = g_jobs.begin(); it != g_jobs.end();) {
        if (!tab_by_id(it->first)) { it = g_jobs.erase(it); continue; }
        any = any || it->second.on;
        ++it;
    }
    if (!any) { KillTimer(NULL, g_poll); g_poll = 0; return; }
    Tab *t = active_tab();
    if (!t || !t->view || t->suspended || t->video_mode || !IsWindowVisible(g_win) || IsIconic(g_win)) return;
    auto it = g_jobs.find(t->id);
    if (it != g_jobs.end() && it->second.on && !it->second.busy) pump(t->id);
}

void translate_page(Tab *t) {
    if (!can_translate(t)) { ui_toast(T(L"Esta página no se puede traducir.")); return; }
    if (translator_js().empty()) return;
    Job &j = g_jobs[t->id];
    j.gen++;
    j.on = true; j.busy = true;
    j.tl = translate_target();
    j.total = j.done = j.left = j.pending = j.fail = 0;
    j.put.clear();
    t->tr_state = 1;
    if (!t->page_lang.empty()) t->tr_sticky = t->page_lang;   // (the next pages of this language in this tab, too)
    log_line(L"traducción: página" + (t->page_lang.empty() ? std::wstring() : L" en " + t->page_lang) + L" a " + j.tl);
    ui_translate_changed();
    int id = t->id, gen = j.gen;
    std::wstring js = translator_js() + L"\n;__plumaTr.start(" + json_escape(j.tl) + L")";
    t->view->ExecuteScript(js.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [id, gen](HRESULT hr, LPCWSTR json) -> HRESULT {
            auto it = g_jobs.find(id);
            Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
            if (it == g_jobs.end() || it->second.gen != gen || !t) return S_OK;
            Job &j = it->second;
            j.busy = false;
            Json r = SUCCEEDED(hr) && json ? json_parse(json) : Json();
            if (r.kind != Json::OBJ) {
                j.on = false;
                t->tr_state = 0;
                ui_translate_changed();
                ui_toast(T(L"Esta página no se puede traducir."));
                return S_OK;
            }
            stats(j, r);
            pump(id);
            return S_OK;
        }).Get());
    if (!g_poll) g_poll = SetTimer(NULL, 0, 700, poll);
}
void translate_original(Tab *t) {
    if (!t) return;
    auto it = g_jobs.find(t->id);
    if (it != g_jobs.end()) { it->second.gen++; it->second.on = it->second.busy = false; }
    if (t->view) t->view->ExecuteScript(L"window.__plumaTr && __plumaTr.restore()", nullptr);
    t->tr_state = 0;
    t->tr_sticky.clear();
    ui_translate_changed();
}

// ---------------------------------------------------------------- the page's language
static const wchar_t *kDetectJs =
    L"(() => { const d = document.documentElement; let lang = (d && (d.getAttribute('lang') || d.getAttribute('xml:lang')) || '').trim();"
    L" if (!lang) { const m = document.querySelector('meta[http-equiv=\"content-language\" i]'); if (m) lang = (m.content || '').split(',')[0].trim(); }"
    L" let s = (document.title || '') + ' ', n = 0, x;"
    L" if (document.body) { const w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, { acceptNode: t => { const p = t.parentElement;"
    L"   return p && !p.closest('script,style,noscript,template,code,pre,textarea,svg') ? 1 : 3; } });"
    L"   while (s.length < 2500 && n++ < 6000 && (x = w.nextNode())) { const v = x.nodeValue.trim(); if (v) s += v + ' '; } }"
    L" return { lang, text: s.slice(0, 2500), on: !!(window.__plumaTr && __plumaTr.stats().on) }; })()";

static void detected(Tab *t, const std::wstring &attr, const std::wstring &sample, bool running) {
    bool strong = false;
    std::wstring guess = tr_guess_lang(sample, &strong), own = tr_primary(attr), lang;
    // the page's own word, unless its text clearly says another language (a site that never set it right)
    if (own.empty()) lang = guess;
    else if (!guess.empty() && guess != own && strong) lang = guess;
    else lang = own;
    if (t->tr_state && running) { if (lang != t->page_lang && !lang.empty()) { t->page_lang = lang; ui_invalidate(); } return; }
    bool changed = lang != t->page_lang || !t->tr_offer;
    t->page_lang = lang;
    std::wstring tl = tr_primary(translate_target());
    bool foreign = !lang.empty() && lang != tl;
    t->tr_offer = foreign;
    if (changed) ui_invalidate();
    if (!foreign || t->tr_state) return;
    std::wstring site = site_of(t->url);
    if (translate_never_site(site)) return;
    if (translate_always(lang) || t->tr_sticky == lang) { translate_page(t); return; }
    t->tr_sticky.clear();
    if (!g_set.tr_offer || translate_never(lang)) return;
    // (offered once; if the person was busy elsewhere, the next look at the page tries again)
    std::wstring key = site + L"|" + lang;
    if (!g_offered.count(key) && ui_translate_offer(t)) g_offered.insert(key);
}
static void detect(Tab *t) {
    if (!can_translate(t)) {
        if (t && (t->tr_offer || !t->page_lang.empty())) { t->tr_offer = false; t->page_lang.clear(); ui_invalidate(); }
        return;
    }
    int id = t->id;
    std::wstring url = t->url;
    t->view->ExecuteScript(kDetectJs, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [id, url](HRESULT hr, LPCWSTR json) -> HRESULT {
            Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
            if (!t || t->url != url || FAILED(hr) || !json) return S_OK;
            Json j = json_parse(json);
            if (j.kind != Json::OBJ) return S_OK;
            detected(t, j[L"lang"].s, j[L"text"].s, j[L"on"].b);
            return S_OK;
        }).Get());
}
static std::map<UINT_PTR, int> g_recheck;   // timers: an address the page changed itself, looked at again

void translate_attach(Tab *t) {
    int id = t->id;
    ICoreWebView2 *v = t->view.Get();
    if (!v) return;
    // a new view: a new document, nothing translated in it
    auto it = g_jobs.find(id);
    if (it != g_jobs.end()) { it->second.gen++; it->second.on = it->second.busy = false; }
    t->tr_state = 0; t->tr_offer = false; t->page_lang.clear();
    EventRegistrationToken tok;
    v->add_ContentLoading(Callback<ICoreWebView2ContentLoadingEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2ContentLoadingEventArgs *) -> HRESULT {
            // a new document: whatever was translated went with the old one
            Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
            if (!t) return S_OK;
            auto it = g_jobs.find(id);
            if (it != g_jobs.end()) { it->second.gen++; it->second.on = it->second.busy = false; }
            if (t->tr_state || t->tr_offer || !t->page_lang.empty()) { t->tr_state = 0; t->tr_offer = false; t->page_lang.clear(); ui_translate_changed(); }
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2_2> v2;
    if (SUCCEEDED(t->view.As(&v2)))
        v2->add_DOMContentLoaded(Callback<ICoreWebView2DOMContentLoadedEventHandler>(
            [id](ICoreWebView2 *, ICoreWebView2DOMContentLoadedEventArgs *) -> HRESULT { { Tab *tt_ = tab_by_id(id); WinScope ws_(win_of_tab(tt_)); detect(tt_); } return S_OK; }).Get(), &tok);
    v->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
            // (what was still loading at DOMContentLoaded: its words may say more)
            BOOL ok = FALSE; a->get_IsSuccess(&ok);
            Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
            if (ok && t && !t->tr_state) detect(t);
            return S_OK;
        }).Get(), &tok);
    v->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2SourceChangedEventArgs *a) -> HRESULT {
            // the page changed its own address (YouTube from video to video): look again once it has drawn the new one
            BOOL fresh = TRUE; a->get_IsNewDocument(&fresh);
            if (fresh) return S_OK;
            UINT_PTR tm = SetTimer(NULL, 0, 1500, [](HWND, UINT, UINT_PTR tm, DWORD) {
                KillTimer(NULL, tm);
                auto it = g_recheck.find(tm);
                if (it == g_recheck.end()) return;
                int id = it->second;
                g_recheck.erase(it);
                { Tab *tt_ = tab_by_id(id); WinScope ws_(win_of_tab(tt_)); detect(tt_); }
            });
            if (tm) g_recheck[tm] = id;
            return S_OK;
        }).Get(), &tok);
}

// ---------------------------------------------------------------- selected text
static int g_bubble_gen = 0;
static const wchar_t *kSelectionJs =
    L"(() => { const s = getSelection(); let txt = s ? String(s) : '', r = null;"
    L" if (s && s.rangeCount) { const b = s.getRangeAt(0).getBoundingClientRect(); if (b.width || b.height) r = [b.left, b.top, b.right, b.bottom]; }"
    L" const a = document.activeElement;"
    L" if (!txt && a && (a.tagName === 'TEXTAREA' || a.tagName === 'INPUT') && a.selectionStart != null) {"
    L"   try { txt = a.value.substring(a.selectionStart, a.selectionEnd); const b = a.getBoundingClientRect(); r = [b.left, b.top, b.right, b.bottom]; } catch (e) {} }"
    L" return { txt, r, dpr: devicePixelRatio }; })()";

void translate_selection(Tab *t, const std::wstring &given, bool has_point, POINT view_point) {
    // the selection's words (those the context menu gave, or the page's), translated in a bubble under them
    if (!t || !t->view || !t->ctrl) return;
    int id = t->id;
    t->view->ExecuteScript(kSelectionJs, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [id, given, has_point, view_point](HRESULT hr, LPCWSTR json) -> HRESULT {
            Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t));
            if (!t || !t->ctrl) return S_OK;
            Json j = SUCCEEDED(hr) && json ? json_parse(json) : Json();
            std::wstring text = trim(given.empty() ? j[L"txt"].s : given);
            if (text.empty()) { ui_toast(T(L"Selecciona un texto para traducirlo.")); return S_OK; }
            if (text.size() > 5000) text = text.substr(0, 5000);
            // where: under the selection (in the page's CSS pixels: times devicePixelRatio, from the view's corner)
            RECT b{}; t->ctrl->get_Bounds(&b);
            RECT at{};
            const Json &r = j[L"r"];
            double dpr = j[L"dpr"].kind == Json::NUM && j[L"dpr"].n > 0 ? j[L"dpr"].n : 1;
            if (r.kind == Json::ARR && r.arr.size() == 4)
                at = { (LONG)(r.arr[0].n * dpr), (LONG)(r.arr[1].n * dpr), (LONG)(r.arr[2].n * dpr), (LONG)(r.arr[3].n * dpr) };
            else if (has_point) at = { view_point.x, view_point.y, view_point.x, view_point.y };
            else at = { (b.right - b.left) / 3, (b.bottom - b.top) / 3, (b.right - b.left) / 3, (b.bottom - b.top) / 3 };
            at.left = std::max(at.left, 0L); at.top = std::max(at.top, 0L);
            at.right = std::min(at.right, b.right - b.left); at.bottom = std::min(at.bottom, b.bottom - b.top);
            POINT a = { b.left + at.left, b.top + at.top }, z = { b.left + at.right, b.top + at.bottom };
            ClientToScreen(g_win, &a); ClientToScreen(g_win, &z);
            int gen = ++g_bubble_gen;
            std::wstring tl = translate_target();
            ui_bubble({ a.x, a.y, std::max(a.x, z.x), std::max(a.y, z.y) }, translate_lang_name(tl, true), T(L"Traduciendo…"), true);
            std::thread([gen, text, tl] {
                std::string body;
                std::vector<std::wstring> out;
                std::wstring src;
                int st = tr_post(tl, false, { text }, body);
                bool ok = st == 0 && parse_answer(body, 1, out, &src) && !out.empty();
                ui_post([gen, ok, st, out, src, tl] {
                    if (gen != g_bubble_gen) return;
                    if (ok) ui_bubble_text((src.empty() ? L"" : translate_lang_name(src, true) + L" \x2192 ") + translate_lang_name(tl, true), out[0], false);
                    else ui_bubble_text(T(L"Traducir"), failure_text(st == 0 ? 502 : st), false);
                });
            }).detach();
            return S_OK;
        }).Get());
}

static bool g_pick_menu;   // tests (pluma-prueba:clic-derecho)
void translate_test_pick_menu() { g_pick_menu = true; }
static void pick_menu(ICoreWebView2ContextMenuRequestedEventArgs *a, ICoreWebView2ContextMenuItemCollection *items, ICoreWebView2ContextMenuItem *mine) {
    // a test instance never shows the engine's menu (it would take the focus): its rows go to the log and Pluma's row
    // is chosen as a click would
    if (!g_test || !g_pick_menu) return;
    g_pick_menu = false;
    std::wstring rows;
    UINT32 n = 0; items->get_Count(&n);
    for (UINT32 i = 0; i < n; i++) {
        ComPtr<ICoreWebView2ContextMenuItem> it;
        LPWSTR label = NULL;
        if (FAILED(items->GetValueAtIndex(i, &it)) || FAILED(it->get_Label(&label))) continue;
        rows += (rows.empty() ? L"" : L" | ") + std::wstring(label ? label : L"");
        CoTaskMemFree(label);
    }
    log_line(L"traducción: menú de la página: " + rows);
    INT32 cmd = 0;
    if (!mine || FAILED(mine->get_CommandId(&cmd))) return;
    a->put_SelectedCommandId(cmd);
    a->put_Handled(TRUE);
}
void translate_context_menu(Tab *t, ICoreWebView2ContextMenuRequestedEventArgs *a) {
    // "Traducir «...»" for a selection (under "Copy"), and "Traducir página" / "Mostrar original" on the page itself
    ComPtr<ICoreWebView2ContextMenuTarget> target;
    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
    ComPtr<ICoreWebView2Environment9> e9;
    if (!t || FAILED(a->get_ContextMenuTarget(&target)) || FAILED(a->get_MenuItems(&items)) || FAILED(g_env.As(&e9))) return;
    BOOL sel = FALSE, link = FALSE, editable = FALSE;
    target->get_HasSelection(&sel);
    target->get_HasLinkUri(&link);
    target->get_IsEditable(&editable);
    COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND kind = COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_PAGE;
    target->get_Kind(&kind);
    UINT32 n = 0; items->get_Count(&n);
    auto index_of = [&](const wchar_t *name) -> int {
        for (UINT32 i = 0; i < n; i++) {
            ComPtr<ICoreWebView2ContextMenuItem> it;
            LPWSTR nm = NULL;
            if (FAILED(items->GetValueAtIndex(i, &it)) || FAILED(it->get_Name(&nm))) continue;
            bool same = nm && !_wcsicmp(nm, name);
            CoTaskMemFree(nm);
            if (same) return (int)i;
        }
        return -1;
    };
    int id = t->id;
    EventRegistrationToken tk;
    if (sel) {
        LPWSTR st = NULL; target->get_SelectionText(&st);
        std::wstring text = st ? st : L"";
        CoTaskMemFree(st);
        // the first words, on one line, as Chrome's "Search Google for"
        std::wstring shown;
        bool space = false;
        for (wchar_t c : text) {
            if (iswspace(c)) { space = !shown.empty(); continue; }
            if (space) shown += L' ';
            space = false;
            shown += c;
            if (shown.size() >= 26) break;
        }
        if (shown.empty()) return;
        if (shown.size() < trim(text).size()) shown += L"\x2026";
        std::wstring label;
        for (wchar_t c : fmt1(T(L"Traducir «%1»"), shown)) { label += c; if (c == L'&') label += L'&'; }   // (& is an access key there)
        POINT loc{}; a->get_Location(&loc);
        IStream *icon = ui_glyph_png(0xF2B7);
        ComPtr<ICoreWebView2ContextMenuItem> mine;
        HRESULT hr = e9->CreateContextMenuItem(label.c_str(), icon, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &mine);
        if (icon) icon->Release();
        if (FAILED(hr)) return;
        mine->add_CustomItemSelected(Callback<ICoreWebView2CustomItemSelectedEventHandler>(
            [id, text, loc](ICoreWebView2ContextMenuItem *, IUnknown *) -> HRESULT {
                ui_post([id, text, loc] { Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t)); translate_selection(t, text, true, loc); });
                return S_OK;
            }).Get(), &tk);
        int at = index_of(L"copy");
        items->InsertValueAtIndex(at >= 0 ? (UINT32)at + 1 : 0, mine.Get());
        pick_menu(a, items.Get(), mine.Get());
    } else if (!link && !editable && kind == COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_PAGE && can_translate(t)) {
        int at = index_of(L"reload");
        if (at < 0) return;
        bool on = t->tr_state != 0;
        std::wstring label = on ? T(L"Mostrar original") : fmt1(T(L"Traducir a %1"), translate_lang_name(translate_target(), false));
        IStream *icon = ui_glyph_png(0xF2B7);
        ComPtr<ICoreWebView2ContextMenuItem> mine;
        HRESULT hr = e9->CreateContextMenuItem(label.c_str(), icon, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &mine);
        if (icon) icon->Release();
        if (FAILED(hr)) return;
        mine->add_CustomItemSelected(Callback<ICoreWebView2CustomItemSelectedEventHandler>(
            [id](ICoreWebView2ContextMenuItem *, IUnknown *) -> HRESULT {
                ui_post([id] { Tab *t = tab_by_id(id); WinScope ws_t(win_of_tab(t)); if (!t) return; if (t->tr_state) translate_original(t); else translate_page(t); });
                return S_OK;
            }).Get(), &tk);
        items->InsertValueAtIndex((UINT32)at + 1, mine.Get());
        pick_menu(a, items.Get(), mine.Get());
    }
}

// ---------------------------------------------------------------- the settings page
std::wstring translate_state_json() {
    auto b = [](bool x) { return x ? L"true" : L"false"; };
    auto pair = [](const std::wstring &code, const std::wstring &name) { return L"[" + json_escape(code) + L"," + json_escape(name) + L"]"; };
    std::wstring o = L",\"trOffer\":" + std::wstring(b(g_set.tr_offer)) + L",\"trTarget\":" + json_escape(g_set.tr_target) + L",\"trTargets\":[";
    o += pair(L"", fmt1(T(L"El de Pluma (%1)"), translate_lang_name(lang_code(), false)));
    for (auto c : kTargets) o += L"," + pair(c, translate_lang_name(c, true));
    o += L"],\"trAlways\":[";
    for (size_t i = 0; i < g_set.tr_always.size(); i++) o += (i ? L"," : L"") + pair(g_set.tr_always[i], translate_lang_name(g_set.tr_always[i], true));
    o += L"],\"trNever\":[";
    for (size_t i = 0; i < g_set.tr_never.size(); i++) o += (i ? L"," : L"") + pair(g_set.tr_never[i], translate_lang_name(g_set.tr_never[i], true));
    o += L"],\"trSites\":[";
    bool first = true;
    for (auto &s : translate_never_sites()) { o += (first ? L"" : L",") + json_escape(s); first = false; }
    return o + L"]";
}
bool translate_apply(const std::wstring &key, const Json &val) {
    if (key == L"trOffer") { g_set.tr_offer = val.b; settings_save(); ui_invalidate(); }
    else if (key == L"trTarget") { g_set.tr_target = has(translate_targets(), val.s) ? val.s : L""; settings_save(); }
    else if (key == L"trAlwaysOff") translate_set_always(val.s, false);
    else if (key == L"trNeverOff") translate_set_never(val.s, false);
    else if (key == L"trSiteOff") translate_set_never_site(val.s, false);
    else return false;
    return true;
}
