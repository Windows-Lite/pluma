// Pluma: "Use a strong password" on sign-up forms (Configuración > Privacidad y datos, `sugerir_contrasenas`).
// Edge's own needs Microsoft's sync, which the engine has not got, so Pluma brings it: passgen.js (RCDATA 220) goes
// into every page and frame of every view, with its words in the chosen language. The page draws the suggestion and
// fills the fields; the engine's own password manager saves it when the form is sent, as any typed password (in
// incognito nothing is kept, and the suggestion says so). The setting reaches the open pages through ExecuteScript
// with an event only Pluma knows the name of; new documents get the script with it written in.
//
// Keeping it: the engine should save on its own (IsPasswordAutosaveEnabled) but it asks instead («¿Guardar la
// contraseña?», a window of its own owned by Pluma's; WebView2Feedback #4857; nor does its credentials_enable_autosave
// preference change that, tried). That prompt stays the person's; so that a suggested password is not lost when it
// is closed (or goes with a click elsewhere), Pluma notices it going and, if the engine has not got the login by then
// and the site is not one the person said "never" to, writes the row itself with passwords.cpp's writer, which the
// engine only reads at its next start (the notice says so). With no prompt seen nothing is written, and an
// "Actualizar" prompt (an old login of that user) is left alone. For that Pluma makes the suggested passwords itself
// (the page asks for them) and the page says when one went with its form, both through requests to the page's own
// site at /.well-known/pluma-passgen/, which Pluma answers itself before anything goes out (same origin, so the
// site's CSP lets them through; web messages stay off for pages). Pluma only listens to the tab's own site (the
// request's Origin header, which no script sets: no frame of another site, nothing opaque) and only keeps a password
// it made for that tab, so a page cannot plant a login. A site whose CSP does not allow its own calls gets a
// suggestion made by the page and only the engine's prompt. Passwords stay in memory only, a few minutes at most, and
// nothing of them (nor the user or the site) goes to the log.
#include "pluma.h"
#include "json.h"
#include <bcrypt.h>
#include <algorithm>
#include <map>
#include <set>
#include <thread>

static std::wstring g_key;   // the name of the switch event: random, per run

static const std::string &source() {
    static const std::string s = [] {
        HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(220), RT_RCDATA);
        HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
        return g ? std::string((const char *)LockResource(g), SizeofResource(g_inst, r)) : std::string();
    }();
    return s;
}
static const std::wstring &key() {
    if (g_key.empty()) {
        BYTE b[16] = {};
        BCryptGenRandom(NULL, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        wchar_t s[40] = L"";
        for (int i = 0; i < 16; i++) swprintf_s(s + i * 2, 3, L"%02x", b[i]);
        g_key = s;
    }
    return g_key;
}
static std::wstring script(bool incognito) {
    std::wstring src = wide(source());
    const wchar_t *a = L"/*PLUMA_CFG*/", *b = L"/*PLUMA_CFG_FIN*/";
    size_t i = src.find(a), j = src.find(b);
    if (i == std::wstring::npos || j == std::wstring::npos || j < i) return L"";
    auto bl = [](bool x) { return x ? L"true" : L"false"; };
    std::wstring cfg = L"{on:" + std::wstring(bl(g_set.suggest_passwords)) + L",incognito:" + bl(incognito) + L",key:" + json_escape(key()) +
                       L",test:false,use:" + json_escape(T(L"Usar contraseña segura")) + L",save:" + json_escape(T(L"Pluma la guarda para este sitio")) +
                       L",nosave:" + json_escape(T(L"En incógnito no se guarda: apúntala")) + L"}";
    return src.substr(0, i) + cfg + src.substr(j + wcslen(b));
}
static void add(Tab *t) {
    int id = t->id;
    std::wstring s = script(t->incognito);
    if (s.empty()) return;
    t->view->AddScriptToExecuteOnDocumentCreated(s.c_str(), Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
        [id](HRESULT hr, LPCWSTR sid) -> HRESULT { Tab *t = tab_by_id(id); if (t && SUCCEEDED(hr) && sid) t->passgen_id = sid; return S_OK; }).Get());
}
static void watch_engine(ICoreWebView2 *v);
static void watch_requests(Tab *t);
void passgen_attach(Tab *t) {
    t->passgen_id.clear();
    if (!t->view) return;
    add(t);
    if (!t->incognito) { watch_engine(t->view.Get()); watch_requests(t); }
}
void passgen_settings_changed() {
    // new documents get the script as it is now (setting, language); the open ones are switched on or off at once
    // (their frames, at their next page)
    std::wstring flip = L"dispatchEvent(new CustomEvent(" + json_escape(L"pluma-passgen-" + key()) + L",{detail:" +
                        (g_set.suggest_passwords ? L"true" : L"false") + L"}))";
    for (auto &t : g_tabs) {
        if (!t->view) continue;
        if (!t->passgen_id.empty()) t->view->RemoveScriptToExecuteOnDocumentCreated(t->passgen_id.c_str());
        t->passgen_id.clear();
        add(t.get());
        t->view->ExecuteScript(flip.c_str(), NULL);
    }
}

// ---------------------------------------------------------------- keeping a suggested password that was sent
// Pluma makes the suggestions (so it knows which passwords are its own) and hears when one was sent; both through
// <the page's site>/.well-known/pluma-passgen/ (passgen.js), answered only for the tab's own site: the request's
// Origin, which no script can set, has to be the tab's (not a frame of another site, not an opaque one).
struct Made { std::vector<std::wstring> pws; double at = 0; std::vector<double> asked; };
struct Sent { std::wstring url, user, pass; double at = 0; bool busy = false; };
static std::map<int, Made> g_made;            // tab id -> the passwords Pluma made for it (memory only, a while)
static std::map<int, Sent> g_sent;            // tab id -> its sent suggestion (memory only)
static std::set<HWND> g_seen;                 // prompts being handled
static HWINEVENTHOOK g_hook;
static DWORD g_hook_pid;
static const double kKeepMs = 10 * 60 * 1000.0;   // what Pluma made for a tab, at most
static const double kSentMs = 30 * 1000.0;        // a report, until the engine's prompt shows
static const size_t kMadeMax = 4000;
static const size_t kAnswerMax = 20000;           // characters in one answer (a page cannot make Pluma work for long)

static void wipe(std::wstring &s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t)); s.clear(); }
static void forget_made(int id) {
    auto it = g_made.find(id);
    if (it == g_made.end()) return;
    for (auto &p : it->second.pws) wipe(p);
    g_made.erase(it);
}
static void forget(int id) {
    auto it = g_sent.find(id);
    if (it == g_sent.end()) return;
    wipe(it->second.pass); wipe(it->second.user);
    g_sent.erase(it);
}
static std::wstring host_of(const std::wstring &url) { return url_host(url); }
static std::wstring origin_of(const std::wstring &url) {
    // "https://Www.X.com:8443/a?b" -> "https://www.x.com:8443" (as an Origin header says it); "" if not http(s)
    std::wstring u = lower(url);
    size_t p = u.find(L"://");
    if (p == std::wstring::npos) return L"";
    std::wstring scheme = u.substr(0, p);
    if (scheme != L"http" && scheme != L"https") return L"";
    size_t e = u.find_first_of(L"/?#", p + 3);
    std::wstring auth = u.substr(p + 3, e == std::wstring::npos ? std::wstring::npos : e - p - 3);
    size_t at = auth.rfind(L'@');
    if (at != std::wstring::npos) auth = auth.substr(at + 1);
    if ((scheme == L"https" && auth.size() > 4 && auth.compare(auth.size() - 4, 4, L":443") == 0) ||
        (scheme == L"http" && auth.size() > 3 && auth.compare(auth.size() - 3, 3, L":80") == 0))
        auth = auth.substr(0, auth.rfind(L':'));
    return auth.empty() ? L"" : scheme + L"://" + auth;
}

// the generator, as passgen.js's (Chromium's): no look-alike characters, one of each class while it fits, no -- or __
static const wchar_t *kSets[][4] = {
    { L"abcdefghijkmnpqrstuvwxyz", L"ABCDEFGHJKLMNPQRSTUVWXYZ", L"23456789", L"-_.:!" },
    { L"abcdefghijkmnpqrstuvwxyz", L"ABCDEFGHJKLMNPQRSTUVWXYZ", L"23456789", NULL },
    { L"abcdefghijkmnpqrstuvwxyz", L"23456789", NULL, NULL },
    { L"ABCDEFGHJKLMNPQRSTUVWXYZ", L"23456789", NULL, NULL },
    { L"abcdefghijkmnpqrstuvwxyz", L"ABCDEFGHJKLMNPQRSTUVWXYZ", NULL, NULL },
    { L"abcdefghijkmnpqrstuvwxyz", NULL, NULL, NULL },
    { L"23456789", NULL, NULL, NULL },
};
static unsigned uniform(unsigned n) {
    unsigned lim = (unsigned)((0x100000000ull / n) * n), b = 0;
    for (;;) {
        BCryptGenRandom(NULL, (PUCHAR)&b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (lim == 0 || b < lim) return b % n;
    }
}
static std::wstring build(int len, int set) {
    std::vector<std::wstring> classes;
    for (auto c : kSets[set]) if (c) classes.push_back(c);
    std::wstring all;
    for (auto &c : classes) all += c;
    for (int tries = 0; tries < 20; tries++) {
        std::wstring out;
        for (size_t i = 0; i < classes.size() && (int)out.size() < len; i++) out += classes[i][uniform((unsigned)classes[i].size())];
        while ((int)out.size() < len) out += all[uniform((unsigned)all.size())];
        for (size_t i = out.size() - 1; i > 0; i--) std::swap(out[i], out[uniform((unsigned)i + 1)]);
        if (out.find(L"--") == std::wstring::npos && out.find(L"__") == std::wstring::npos) return out;
        wipe(out);
    }
    return L"";
}
static std::string make_for(int id, const Json &f) {
    // {lo, hi, len, pattern} -> {"<len>:<set>": [candidates]}: one for a field without a pattern; with one, as many as
    // passgen.js tries (30 at the usual length, 3 at the nearest others, every set), all remembered for this tab
    int lo = (int)f[L"lo"].n, hi = (int)f[L"hi"].n, len = (int)f[L"len"].n;
    bool pattern = f[L"pattern"].b;
    if (lo < 4 || hi > 200 || lo > hi || len < lo || len > hi) return "";
    Made &m = g_made[id];
    // at most 5 asks in 10 s per tab (one per field shown is what a page needs)
    double now = now_ms();
    m.asked.erase(std::remove_if(m.asked.begin(), m.asked.end(), [now](double a) { return now - a > 10000; }), m.asked.end());
    if (m.asked.size() >= 5) return "";
    m.asked.push_back(now);
    m.at = now;
    size_t chars = 0;
    if (m.pws.size() > kMadeMax) { for (auto &p : m.pws) wipe(p); m.pws.clear(); }
    std::wstring out = L"{";
    auto put = [&](int l, int set, int n) {
        out += (out.size() > 1 ? L",\"" : L"\"") + std::to_wstring(l) + L":" + std::to_wstring(set) + L"\":[";
        for (int i = 0; i < n && chars + l <= kAnswerMax; i++) {
            std::wstring p = build(l, set);
            chars += p.size();
            if (p.empty()) continue;
            out += (i ? L",\"" : L"\"") + p + L"\"";
            m.pws.push_back(p);
            wipe(p);
        }
        out += L"]";
    };
    if (!pattern) put(len, 0, 1);
    else
        for (int set = 0; set < 7; set++) {
            put(len, set, 30);
            for (int d = 1; d <= 16; d++) {
                if (len + d <= hi) put(len + d, set, 3);
                if (len - d >= lo) put(len - d, set, 3);
            }
        }
    out += L"}";
    std::string r = utf8(out);
    wipe(out);
    return r;
}

static void keep_it_ourselves(int id) {
    // the engine's prompt went: if the engine has not got it, Pluma's own writer, applied at the next start
    auto it = g_sent.find(id);
    if (it == g_sent.end()) return;
    Sent s = it->second;
    forget(id);
    int r = passwords_keep(s.url, s.user, s.pass);
    wipe(s.pass); wipe(s.user);
    if (r == 1) {
        log_line(L"contraseña sugerida: la guarda Pluma (entra al reiniciar)");
        ui_toast(T(L"Contraseña guardada, se rellenará al reiniciar Pluma: ") + host_of(s.url));
    } else if (r == 2) {
        log_line(L"contraseña sugerida: ya la tiene el motor");
    } else if (r == 3) {
        log_line(L"contraseña sugerida: sitio con «nunca», no se guarda");
    } else {
        log_line(L"contraseña sugerida: no se pudo guardar");
        ui_toast(T(L"No se pudo guardar la contraseña sugerida para ") + host_of(s.url) + L".");
    }
}

static void CALLBACK tick(HWND, UINT, UINT_PTR tid, DWORD) {
    // what Pluma holds goes after a while, or with its tab (no prompt seen: nothing is written)
    double now = now_ms();
    std::vector<int> old;
    for (auto &p : g_sent) if (!p.second.busy && (!tab_by_id(p.first) || now - p.second.at > kSentMs)) old.push_back(p.first);
    for (int id : old) forget(id);
    old.clear();
    for (auto &p : g_made) if (!tab_by_id(p.first) || now - p.second.at > kKeepMs) old.push_back(p.first);
    for (int id : old) forget_made(id);
    if (g_sent.empty() && g_made.empty()) KillTimer(g_win, tid);
}
static void heard_sent(Tab *t, const Json &j) {
    // {user, pass}: kept only if the password is one Pluma made for this tab
    std::wstring pass = j[L"pass"].s, user = j[L"user"].s;
    auto m = g_made.find(t->id);
    bool ours = false;
    if (m != g_made.end() && !pass.empty())
        for (auto &p : m->second.pws) if (p == pass) { ours = true; break; }
    std::wstring url = t->url.substr(0, t->url.find_first_of(L"?#"));
    if (ours && !settings_page_url(url)) {
        Sent &s = g_sent[t->id];
        wipe(s.pass); wipe(s.user);
        s.url = url; s.user = user; s.pass = pass; s.at = now_ms(); s.busy = false;
    }
    if (g_test) log_line(std::wstring(L"passgen: aviso de envío ") + (ours ? L"aceptado" : L"ignorado (no es una sugerida de Pluma)"));
    wipe(pass); wipe(user);
}

// Which prompt it is, by its title (the engine speaks Pluma's language): saving a new login, or updating an old one
// (that one is the person's call). Pluma does not press anything on it: UI Automation could (tried: it works, with no
// focus taken), but it switches the engine's accessibility on for the rest of the session, so every page builds its
// tree from then on; a posted Enter does nothing (the prompt is not active). The prompt is the person's; what Pluma
// does is notice it going, and if the engine has not got the login then (closed, or gone with a click elsewhere), and
// the site is not one the person said "never" to, keep it itself.
static bool has_any(const std::wstring &title, std::initializer_list<const wchar_t *> words) {
    std::wstring t = lower(title);
    for (auto w : words) if (t.find(lower(w)) != std::wstring::npos) return true;
    return false;
}
enum PromptEnd { NOT_PROMPT, UPDATE, GONE };
static PromptEnd watch_prompt(HWND w) {
    // (worker thread) until the prompt goes, and a moment for the engine to write what it saved
    std::wstring title;
    for (int i = 0; i < 15 && title.empty() && IsWindow(w); i++) {   // (its title can come a moment after the window)
        wchar_t buf[256] = L"";
        GetWindowTextW(w, buf, 256);
        title = buf;
        if (title.empty()) Sleep(100);
    }
    if (!has_any(title, { L"contraseña", L"password", L"senha", L"mot de passe", L"passwort" })) return NOT_PROMPT;
    if (has_any(title, { L"actualizar", L"update", L"atualizar", L"mettre à jour", L"aktualisieren", L"aggiorna" })) return UPDATE;
    for (int i = 0; i < (int)(kKeepMs / 200) && IsWindow(w) && IsWindowVisible(w); i++) Sleep(200);
    Sleep(2000);
    return GONE;
}
static void on_prompt(HWND w) {
    // a window of the engine's, owned by this window, just showed: its save prompt, or its autofill list...
    Tab *t = active_tab();
    if (g_test) log_line(L"passgen: ventana del motor " + std::to_wstring((long long)(INT_PTR)w) + (t && g_sent.count(t->id) ? L" (enviada)" : L""));
    if (!t || t->incognito || g_seen.count(w)) return;
    int id = t->id;
    auto it = g_sent.find(id);
    if (it == g_sent.end() || it->second.busy) return;
    // the prompt of that sending: right after it (a later one, of a login typed by hand, is not this one)
    if (now_ms() - it->second.at > kSentMs) { forget(id); return; }
    it->second.busy = true;
    g_seen.insert(w);
    std::thread([w, id] {
        PromptEnd r = watch_prompt(w);
        ui_post([w, id, r] {
            g_seen.erase(w);
            auto it = g_sent.find(id);
            if (it == g_sent.end()) return;
            if (r == NOT_PROMPT) { it->second.busy = false; return; }   // (its autofill list: the prompt may still come)
            if (r == UPDATE) { forget(id); return; }                      // (an old login of this user: the person's call)
            keep_it_ourselves(id);
        });
    }).detach();
}
static void CALLBACK on_show(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !w || GetAncestor(w, GA_ROOT) != w) return;
    if (GetWindow(w, GW_OWNER) != g_win) return;   // (other profiles' windows share the engine)
    wchar_t cls[64] = L"";
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"Chrome_WidgetWin_1")) return;
    on_prompt(w);
}
static void watch_engine(ICoreWebView2 *v) {
    UINT32 pid = 0;
    if (FAILED(v->get_BrowserProcessId(&pid)) || !pid || pid == g_hook_pid) return;
    if (g_hook) UnhookWinEvent(g_hook);
    g_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, on_show, pid, 0, WINEVENT_OUTOFCONTEXT);
    g_hook_pid = pid;
}
static void watch_requests(Tab *t) {
    // passgen.js's requests (any frame of the tab; they never go out): "new" (passwords for a field) and "sent" (one
    // of them went with its form). To or from another origin than the tab's, or with no Origin: an empty answer,
    // nothing kept.
    int id = t->id;
    const std::wstring path = L"/.well-known/pluma-passgen/";
    ComPtr<ICoreWebView2_22> v22;
    if (SUCCEEDED(t->view.As(&v22)))
        v22->AddWebResourceRequestedFilterWithRequestSourceKinds((L"*" + path + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
                                                                 COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
    else t->view->AddWebResourceRequestedFilter((L"*" + path + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken tok;
    t->view->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [id, path](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *a) -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> req;
            LPWSTR uri = NULL;
            if (FAILED(a->get_Request(&req)) || FAILED(req->get_Uri(&uri))) return S_OK;
            std::wstring u = uri; CoTaskMemFree(uri);
            // <origin>/.well-known/pluma-passgen/<what>, nothing else (never let through: answered here, always)
            if (u.find(path) == std::wstring::npos) return S_OK;
            std::wstring uo = origin_of(u);
            bool exact = !uo.empty() && u.size() > uo.size() + path.size() && u.compare(uo.size(), path.size(), path) == 0;
            std::wstring what = exact ? u.substr(uo.size() + path.size()) : L"";
            // who asks: the Origin header (the browser's, not the page's), which has to be the tab's own
            // (a page with "Referrer-Policy: no-referrer" sends "Origin: null" even to its own site; then
            // Sec-Fetch-Site, also the browser's, says whether the caller is of the url's origin)
            std::wstring origin, site;
            ComPtr<ICoreWebView2HttpRequestHeaders> hs;
            if (SUCCEEDED(req->get_Headers(&hs)) && hs) {
                LPWSTR o = NULL;
                if (SUCCEEDED(hs->GetHeader(L"Origin", &o)) && o) { origin = lower(o); CoTaskMemFree(o); o = NULL; }
                if (SUCCEEDED(hs->GetHeader(L"Sec-Fetch-Site", &o)) && o) { site = lower(o); CoTaskMemFree(o); }
            }
            Tab *t = tab_by_id(id);
            std::wstring mine = t ? origin_of(t->url) : L"";
            bool caller = origin == mine || ((origin.empty() || origin == L"null") && site == L"same-origin");
            bool trusted = exact && t && !t->incognito && !mine.empty() && uo == mine && caller;
            std::string answer;
            if (trusted) {
                ComPtr<IStream> body;
                std::string text;
                if (SUCCEEDED(req->get_Content(&body)) && body) {
                    char buf[4096]; ULONG got = 0;
                    while (text.size() < 16384 && SUCCEEDED(body->Read(buf, sizeof(buf), &got)) && got) text.append(buf, got);
                }
                std::wstring w = wide(text);
                if (!text.empty()) SecureZeroMemory(&text[0], text.size());
                Json j = json_parse(w);
                wipe(w);
                if (what == L"new") answer = make_for(id, j);
                else if (what == L"sent") heard_sent(t, j);
                for (auto &kv : j.obj) wipe(kv.second.s);
                SetTimer(g_win, 0x5A61, 1000, tick);
            } else if (g_test) {
                const wchar_t *o = origin.empty() ? L"-" : origin == mine ? L"el suyo" : origin == L"null" ? L"null" : L"otro";
                log_line(std::wstring(L"passgen: petición de otro origen, ignorada (Origin ") + o + L", Sec-Fetch-Site " +
                         (site.empty() ? L"-" : site) + L")");
            }
            std::wstring headers = L"Cache-Control: no-store";
            if (trusted) headers += L"\r\nContent-Type: application/json";
            IStream *s = answer.empty() ? NULL : SHCreateMemStream((const BYTE *)answer.data(), (UINT)answer.size());
            ComPtr<ICoreWebView2WebResourceResponse> res;
            if (g_env && SUCCEEDED(g_env->CreateWebResourceResponse(s, s ? 200 : 204, s ? L"OK" : L"No Content", headers.c_str(), &res))) a->put_Response(res.Get());
            if (s) s->Release();
            if (!answer.empty()) SecureZeroMemory(&answer[0], answer.size());
            return S_OK;
        }).Get(), &tok);
}
