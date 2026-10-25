// Pluma: the settings page (pluma://settings), as Edge's edge://settings.
// The page is settings.html, inside the exe (RCDATA 200), served to the views at https://pluma.local/ (a host no
// one can register: .local never reaches the internet). It talks to Pluma with chrome.webview.postMessage:
//   {op:"get", texts:[...]}          -> Pluma answers with the state and the texts in the chosen language (T())
//   {op:"set", key, value}           -> a setting changed: saved to ajustes.ini and applied at once
//   {op:"do", what}                  -> an action (clear data, check for updates, open a folder...)
// Only messages from that page count: other pages cannot reach Pluma's settings.
#include "pluma.h"
#include "json.h"

static const wchar_t *kHost = L"https://pluma.local/";
static const wchar_t *kPage = L"https://pluma.local/settings";

bool settings_page_url(const std::wstring &u) { return starts_with(u, kHost); }
std::wstring settings_page_display(const std::wstring &u) {
    // what the address bar shows for Pluma's own pages
    return settings_page_url(u) ? L"pluma://" + u.substr(wcslen(kHost)) : u;
}
std::wstring settings_page_from_typed(const std::wstring &typed) {
    std::wstring t = lower(typed);
    // Pluma's own pages: pluma://settings, pluma://bookmarks, pluma://importar (edge:// and chrome:// settings too, by habit)
    if (starts_with(t, L"pluma://bookmarks") || starts_with(t, L"pluma://marcadores")) return std::wstring(kHost) + L"bookmarks";
    if (starts_with(t, L"pluma://importar") || starts_with(t, L"pluma://import")) return std::wstring(kHost) + L"importar";
    for (const wchar_t *p : { L"pluma://", L"edge://settings", L"chrome://settings" })
        if (starts_with(t, p)) return kPage;
    return L"";
}
// which page a pluma.local url is, and the resource that holds its HTML (200 settings, 201 bookmarks, 202 import)
static int page_resource(const std::wstring &u) {
    std::wstring path = u.substr(wcslen(kHost));
    size_t cut = path.find_first_of(L"?#");
    if (cut != std::wstring::npos) path = path.substr(0, cut);
    if (path == L"bookmarks") return 201;
    if (path == L"importar") return 202;
    if (path.empty() || path == L"settings") return 200;
    return 0;
}
void settings_open() {
    for (size_t i = 0; i < g_tabs.size(); i++)
        if (settings_page_url(g_tabs[i]->url) && page_resource(g_tabs[i]->url) == 200) { tab_activate((int)i); return; }
    tab_new(kPage, true);
}

static std::string page_html(int id) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    if (!g) return "";
    return std::string((const char *)LockResource(g), SizeofResource(g_inst, r));
}

static bool startup_on() {
    wchar_t v[MAX_PATH * 2] = L""; DWORD sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Pluma", RRF_RT_REG_SZ, NULL, v, &sz) == ERROR_SUCCESS;
}
static void startup_set(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + g_root + L"\\Pluma.exe\" --oculto";
        RegSetValueExW(k, L"Pluma", 0, REG_SZ, (const BYTE *)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"Pluma");
    RegCloseKey(k);
}
static bool default_browser() {
    wchar_t v[128] = L""; DWORD sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice",
                 L"ProgId", RRF_RT_REG_SZ, NULL, v, &sz);
    return !wcscmp(v, L"PlumaHTML");
}

static std::wstring state_json(const Json &texts) {
    auto b = [](bool x) { return x ? L"true" : L"false"; };
    std::wstring o = L"{\"op\":\"state\"";
    o += L",\"dark\":" + std::wstring(b(dark_theme()));
    o += L",\"lang\":" + json_escape(lang_code());
    o += L",\"theme\":" + std::to_wstring(g_set.theme);
    o += L",\"font\":" + json_escape(g_set.font);
    o += L",\"engine\":" + json_escape(g_set.engine);
    o += L",\"idioma\":" + json_escape(g_set.lang);
    o += L",\"update\":" + std::to_wstring(g_set.update);
    o += L",\"version\":" + json_escape(update_version());
    o += L",\"suggest\":" + std::wstring(b(g_set.suggest_top));
    o += L",\"video\":" + std::wstring(b(g_set.native_video));
    o += L",\"videoAvailable\":" + std::wstring(b(video_available()));
    o += L",\"resident\":" + std::wstring(b(g_set.resident));
    o += L",\"startup\":" + std::wstring(b(startup_on()));
    o += L",\"spare\":" + std::wstring(b(g_set.spare));
    o += L",\"sleep\":" + std::to_wstring(g_set.sleep_s);
    o += L",\"defaultBrowser\":" + std::wstring(b(default_browser()));
    o += L",\"memory\":" + std::to_wstring(engine_memory_mb());
    size_t on = 0; for (auto &e : g_exts) if (!e.builtin && e.enabled) on++;
    size_t son = 0; for (auto &s : g_scripts) if (s.enabled) son++;
    o += L",\"extensions\":" + std::to_wstring(on) + L",\"scripts\":" + std::to_wstring(son);
    o += L",\"engines\":[";
    bool first = true;
    for (const wchar_t *id : { L"google", L"duckduckgo", L"bing", L"startpage", L"brave" }) {
        o += (first ? L"" : L",") + std::wstring(L"[") + json_escape(id) + L"," + json_escape(engine_name(id)) + L"]";
        first = false;
    }
    o += L"],\"langs\":[";
    first = true;
    for (const wchar_t *c : { L"", L"es", L"en", L"pt", L"fr", L"de", L"it" }) {
        o += (first ? L"" : L",") + std::wstring(L"[") + json_escape(c) + L"," + json_escape(lang_name(c)) + L"]";
        first = false;
    }
    o += L"]";
    // the page's words, in the chosen language (the page is written in Spanish, as the code)
    if (texts.kind == Json::ARR) {
        o += L",\"t\":{";
        first = true;
        for (auto &s : texts.arr) {
            if (!s.is_str()) continue;
            o += (first ? L"" : L",") + json_escape(s.s) + L":" + json_escape(T(s.s.c_str()));
            first = false;
        }
        o += L"}";
    }
    return o + L"}";
}

static void post_state(ICoreWebView2 *v, const Json &texts) { v->PostWebMessageAsJson(state_json(texts).c_str()); }

void settings_page_refresh_soon() {
    // (after the setting is written, from the window's thread)
    if (g_bar) ui_post([] { settings_page_refresh(); });
}
void settings_page_refresh() {
    // something changed elsewhere (the menu, another settings tab): the open pages show it
    for (auto &t : g_tabs) if (t->view && settings_page_url(t->url) && page_resource(t->url) == 200) post_state(t->view.Get(), Json());
}

static void apply(const std::wstring &key, const Json &val) {
    if (key == L"theme") { g_set.theme = (int)val.n; settings_save(); ui_theme_changed(); tabs_apply_theme(); }
    else if (key == L"font") { g_set.font = val.s; settings_save(); ui_font_changed(); }
    else if (key == L"engine") { g_set.engine = val.s; settings_save(); }
    else if (key == L"idioma") { g_set.lang = val.s; settings_save(); ui_invalidate(); }
    else if (key == L"update") { g_set.update = (int)val.n; settings_save(); }
    else if (key == L"suggest") { g_set.suggest_top = val.b; settings_save(); ui_ntp_changed(); }
    else if (key == L"video") { g_set.native_video = val.b; settings_save(); video_settings_changed(); }
    else if (key == L"resident") { g_set.resident = val.b; settings_save(); }
    else if (key == L"spare") { g_set.spare = val.b; settings_save(); }
    else if (key == L"sleep") { g_set.sleep_s = std::max(10, (int)val.n); settings_save(); }
    else if (key == L"startup") startup_set(val.b);
}
static void act(const std::wstring &what) {
    if (what == L"clearHistory") { history_clear(); ui_toast(T(L"Historial borrado")); }
    else if (what == L"clearAll") data_clear_all();
    else if (what == L"checkUpdates") update_check_now();
    else if (what == L"defaultBrowser") ShellExecuteW(NULL, L"open", L"ms-settings:defaultapps?registeredAppUser=Pluma", NULL, NULL, SW_SHOWNORMAL);
    else if (what == L"downloads") tab_downloads();
    else if (what == L"extensions") ui_open_menu_page(L"ext");
    else if (what == L"scripts") ui_open_menu_page(L"scripts");
    else if (what == L"store") tab_new(L"https://chromewebstore.google.com/", true);
    else if (what == L"bookmarks") bookmarks_open();
    else if (what == L"import") import_open();
    else if (what == L"repo") tab_new(L"https://github.com/Windows-Lite/pluma", true);
}

void settings_attach(ICoreWebView2 *v) {
    // every view: Pluma's own page is served from the exe, and only it may talk to Pluma
    v->AddWebResourceRequestedFilter(L"https://pluma.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken tok;
    v->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *a) -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> req;
            LPWSTR uri = NULL;
            if (FAILED(a->get_Request(&req)) || FAILED(req->get_Uri(&uri))) return S_OK;
            std::wstring u = uri; CoTaskMemFree(uri);
            int id = settings_page_url(u) ? page_resource(u) : 0;
            std::string body = id ? page_html(id) : "";
            bool page = id != 0;
            IStream *s = SHCreateMemStream((const BYTE *)body.data(), (UINT)body.size());
            ComPtr<ICoreWebView2WebResourceResponse> res;
            if (g_env && SUCCEEDED(g_env->CreateWebResourceResponse(s, page ? 200 : 404, page ? L"OK" : L"Not Found",
                    L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store", &res)))
                a->put_Response(res.Get());
            if (s) s->Release();
            return S_OK;
        }).Get(), &tok);
    // pages cannot message Pluma (tabs.cpp turns it off); only this one, while it is on screen
    v->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [](ICoreWebView2 *v, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
            LPWSTR uri = NULL;
            a->get_Uri(&uri);
            bool own = uri && settings_page_url(uri);
            CoTaskMemFree(uri);
            ComPtr<ICoreWebView2Settings> st;
            if (SUCCEEDED(v->get_Settings(&st))) st->put_IsWebMessageEnabled(own ? TRUE : FALSE);
            return S_OK;
        }).Get(), &tok);
    v->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2 *v, ICoreWebView2WebMessageReceivedEventArgs *a) -> HRESULT {
            LPWSTR src = NULL, msg = NULL;
            a->get_Source(&src);
            std::wstring from = src ? src : L"";
            CoTaskMemFree(src);
            if (!settings_page_url(from)) return S_OK;
            if (FAILED(a->get_WebMessageAsJson(&msg)) || !msg) return S_OK;
            Json j = json_parse(msg);
            CoTaskMemFree(msg);
            if (page_resource(from) != 200) { bm_page_do(v, from, j); return S_OK; }   // the bookmarks and import pages
            std::wstring op = j[L"op"].s;
            if (op == L"get") { post_state(v, j[L"texts"]); return S_OK; }
            if (op == L"set") { apply(j[L"key"].s, j[L"value"]); if (j[L"key"].s == L"startup") settings_page_refresh(); }
            else if (op == L"do") act(j[L"what"].s);
            return S_OK;
        }).Get(), &tok);
}
