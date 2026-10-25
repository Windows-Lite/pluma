// Pluma: tabs and their WebView2s. A tab has a view only while it is worth one: a new tab costs nothing until
// something is typed, a restored tab until it is looked at, and a hidden tab is frozen, then let go.
#include "pluma.h"
#include <psapi.h>

std::vector<std::unique_ptr<Tab>> g_tabs;
int g_active = -1;
static int g_next_id = 1;
struct Closed { std::wstring url, title; std::string icon; int index; };
static std::vector<Closed> g_closed;
static ComPtr<ICoreWebView2Controller> g_spare;   // made ahead of time, never navigated: the next page starts at once
static bool g_spare_making;
static std::vector<std::function<void(ComPtr<ICoreWebView2Controller>)>> g_view_waiters;
static ComPtr<ICoreWebView2Profile> g_profile;
static std::vector<std::function<void(ICoreWebView2Profile *)>> g_profile_waiters;
void with_profile(std::function<void(ICoreWebView2Profile *)> f) {
    if (g_profile) { f(g_profile.Get()); return; }
    g_profile_waiters.push_back(f);
    spare_prepare();   // a view brings the profile with it
}

Tab *active_tab() { return g_active >= 0 && g_active < (int)g_tabs.size() ? g_tabs[g_active].get() : nullptr; }
Tab *tab_by_id(int id) { for (auto &t : g_tabs) if (t->id == id) return t.get(); return nullptr; }
int tab_index(int id) { for (size_t i = 0; i < g_tabs.size(); i++) if (g_tabs[i]->id == id) return (int)i; return -1; }

// ---------------------------------------------------------------- what was typed -> where to go
static bool all_digits_dots(const std::wstring &h) {
    if (h.empty()) return false;
    for (wchar_t c : h) if (!iswdigit(c) && c != L'.') return false;
    return std::count(h.begin(), h.end(), L'.') == 3;
}
std::wstring fixup_input(const std::wstring &typed) {
    std::wstring s = trim(typed);
    if (s.empty()) return L"";
    std::wstring l = lower(s);
    static const wchar_t *kSchemes[] = { L"http://", L"https://", L"file:", L"about:", L"edge:", L"chrome-extension:",
                                         L"view-source:", L"data:", L"blob:", L"mailto:" };
    for (auto p : kSchemes) if (starts_with(l, p)) return s;
    if (s.size() > 2 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) {
        wchar_t buf[4096]; DWORD n = 4096;
        if (SUCCEEDED(UrlCreateFromPathW(s.c_str(), buf, &n, 0))) return buf;
    }
    if (s[0] != L'?' && s.find_first_of(L" \t") == std::wstring::npos) {
        size_t end = l.find_first_of(L"/?#");
        std::wstring host = l.substr(0, end);
        std::wstring name = host.substr(0, host.find(L':'));
        if (name == L"localhost" || all_digits_dots(name)) return L"http://" + s;
        size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0 && dot + 2 <= name.size()) {
            std::wstring tld = name.substr(dot + 1);
            bool ok = tld.size() >= 2;
            for (wchar_t c : tld) if (!iswalpha(c) && !(starts_with(tld, L"xn--") && (iswalnum(c) || c == L'-'))) ok = false;
            for (wchar_t c : name) if (!iswalnum(c) && c != L'.' && c != L'-' && c < 0x80) ok = false;
            if (ok) return L"https://" + s;   // https first; plain http only if asked for
        }
    }
    return search_url(s[0] == L'?' ? trim(s.substr(1)) : s);
}

// ---------------------------------------------------------------- the engine
static std::wstring browser_args() {
    // AppContainer: every page runs in a tighter sandbox than the default one (as in Edge's enhanced security)
    std::wstring a = L"--enable-features=msWebView2RendererAppContainer";
    // the graphics keep decoded images of every page seen, up to a budget made for big PCs: 64 MB is plenty to scroll
    // through photos without decoding them again, and saved about 17 MB with six tabs (measured 25/10)
    a += L" --force-gpu-mem-discardable-limit-mb=64";
    wchar_t port[16] = L"";
    if (GetEnvironmentVariableW(L"PLUMA_DEPURAR", port, 16) > 0) a += L" --remote-debugging-port=" + std::wstring(port);
    wchar_t extra[1024] = L"";
    GetPrivateProfileStringW(L"pluma", L"motor", L"", extra, 1024, (g_state + L"\\ajustes.ini").c_str());
    if (*extra) a += L" " + std::wstring(extra);
    return a;
}
static void on_browser_exited();
static void trim_after_load();
static void make_view(std::function<void(ComPtr<ICoreWebView2Controller>)> done);
void env_create(std::function<void()> ready) {
    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    opts->put_AreBrowserExtensionsEnabled(TRUE);
    opts->put_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE_FLUENT_OVERLAY);
    std::wstring args = browser_args();
    opts->put_AdditionalBrowserArguments(args.c_str());
    std::wstring data = g_state + L"\\webview2";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, data.c_str(), opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([ready](HRESULT res, ICoreWebView2Environment *env) -> HRESULT {
            if (FAILED(res) || !env) {
                MessageBoxW(g_win, L"No se pudo iniciar el motor de Edge (WebView2). Instala «Microsoft Edge WebView2 Runtime».", L"Pluma", MB_ICONERROR);
                PostQuitMessage(1);
                return S_OK;
            }
            g_env = env;
            // views asked for before the engine was up
            auto waiting = std::move(g_view_waiters);
            g_view_waiters.clear();
            for (auto &w : waiting) make_view(w);
            ComPtr<ICoreWebView2Environment5> e5;
            EventRegistrationToken tok;
            if (SUCCEEDED(g_env.As(&e5)))
                e5->add_BrowserProcessExited(Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                    [](ICoreWebView2Environment *, ICoreWebView2BrowserProcessExitedEventArgs *a) -> HRESULT {
                        COREWEBVIEW2_BROWSER_PROCESS_EXIT_KIND k; a->get_BrowserProcessExitKind(&k);
                        if (!g_quitting) on_browser_exited();
                        return S_OK;
                    }).Get(), &tok);
            ready();
            return S_OK;
        }).Get());
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"No se pudo iniciar el motor de Edge (WebView2). Instala «Microsoft Edge WebView2 Runtime».", L"Pluma", MB_ICONERROR);
        PostQuitMessage(1);
    }
}
static void on_browser_exited() {
    // the engine died (it happens with a broken update or a crash): start it again and bring the page back
    log_line(L"el motor se ha cerrado: se arranca de nuevo");
    for (auto &t : g_tabs) { t->ctrl.Reset(); t->view.Reset(); t->suspended = false; t->loading = false; t->scripts_ids.clear(); }
    g_spare.Reset(); g_spare_making = false; g_profile.Reset();
    g_env.Reset();
    env_create([] { if (active_tab()) tab_activate(g_active); spare_prepare(); });
}

static RECT page_rect() {
    RECT rc; GetClientRect(g_win, &rc);
    rc.top = (LONG)ui_band_height();
    if (rc.top > rc.bottom) rc.top = rc.bottom;
    return rc;
}
void tabs_apply_theme() {
    if (!g_profile) return;
    COREWEBVIEW2_PREFERRED_COLOR_SCHEME s = g_set.theme == 2 ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO
                                          : dark_theme() ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT;
    g_profile->put_PreferredColorScheme(s);
}
static void setup_view(ICoreWebView2Controller *c) {
    ComPtr<ICoreWebView2> v; c->get_CoreWebView2(&v);
    c->put_IsVisible(FALSE);
    RECT r = page_rect(); c->put_Bounds(r);
    ComPtr<ICoreWebView2Controller2> c2;
    const Palette &p = pal();
    // until the first page paints, the bar's colour (no white flash in dark mode); pages get white once they load
    if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))))
        c2->put_DefaultBackgroundColor({ 255, (BYTE)(p.bg.r * 255), (BYTE)(p.bg.g * 255), (BYTE)(p.bg.b * 255) });
    ComPtr<ICoreWebView2Settings> st;
    v->get_Settings(&st);
    // nothing on a web page can talk to Pluma itself: no messages, no host objects
    st->put_IsWebMessageEnabled(FALSE);
    st->put_AreHostObjectsAllowed(FALSE);
    st->put_IsStatusBarEnabled(TRUE);
    st->put_AreDevToolsEnabled(TRUE);
    st->put_AreDefaultScriptDialogsEnabled(TRUE);
    st->put_IsBuiltInErrorPageEnabled(TRUE);
    ComPtr<ICoreWebView2Settings4> st4;
    if (SUCCEEDED(st.As(&st4))) { st4->put_IsPasswordAutosaveEnabled(TRUE); st4->put_IsGeneralAutofillEnabled(TRUE); }
    ComPtr<ICoreWebView2Settings8> st8;
    if (SUCCEEDED(st.As(&st8))) st8->put_IsReputationCheckingRequired(TRUE);   // SmartScreen on pages and downloads
    ComPtr<ICoreWebView2_9> v9;
    if (SUCCEEDED(v.As(&v9))) {
        v9->put_DefaultDownloadDialogCornerAlignment(COREWEBVIEW2_DEFAULT_DOWNLOAD_DIALOG_CORNER_ALIGNMENT_TOP_RIGHT);
        v9->put_DefaultDownloadDialogMargin({ (LONG)(8 * ui_scale()), (LONG)(8 * ui_scale()) });
    }
    if (!g_profile) {
        ComPtr<ICoreWebView2_13> v13;
        if (SUCCEEDED(v.As(&v13))) v13->get_Profile(&g_profile);
        tabs_apply_theme();
        auto waiting = std::move(g_profile_waiters);
        g_profile_waiters.clear();
        if (g_profile) for (auto &f : waiting) f(g_profile.Get());
    }
}
static void make_view(std::function<void(ComPtr<ICoreWebView2Controller>)> done) {
    if (!g_env) { g_view_waiters.push_back(done); return; }
    double t0 = now_ms();
    g_env->CreateCoreWebView2Controller(g_win, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [done, t0](HRESULT hr, ICoreWebView2Controller *c) -> HRESULT {
            if (FAILED(hr) || !c) { log_line(L"no se pudo crear una vista"); done(nullptr); return S_OK; }
            if (g_dev) log_line(L"vista creada en " + std::to_wstring((int)(now_ms() - t0)) + L" ms");
            setup_view(c);
            done(c);
            return S_OK;
        }).Get());
}
static void CALLBACK spare_timer(HWND, UINT, UINT_PTR id, DWORD) { KillTimer(g_win, id); spare_prepare(); }
void spare_prepare() {
    if (g_spare || g_spare_making || !g_env || g_quitting) return;
    g_spare_making = true;
    make_view([](ComPtr<ICoreWebView2Controller> c) { g_spare_making = false; g_spare = c; });
}
static void take_view(std::function<void(ComPtr<ICoreWebView2Controller>)> done) {
    if (g_spare) {
        auto c = g_spare;
        g_spare.Reset();
        RECT r = page_rect(); c->put_Bounds(r);
        done(c);
        SetTimer(g_win, 0x51A3, 1500, spare_timer);   // the next spare, once this page is on its way
        return;
    }
    make_view(done);
}

// ---------------------------------------------------------------- one tab's view
static void set_progress(Tab *t, float p) { if (p > t->progress) t->progress = p; ui_invalidate(); }
static void on_icon(int id) {
    Tab *t = tab_by_id(id);
    if (!t || !t->view) return;
    ComPtr<ICoreWebView2_15> v15;
    if (FAILED(t->view.As(&v15))) return;
    v15->GetFavicon(COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG, Callback<ICoreWebView2GetFaviconCompletedHandler>(
        [id](HRESULT hr, IStream *s) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t || FAILED(hr) || !s) return S_OK;
            std::string png;
            char buf[16384]; ULONG got = 0;
            while (SUCCEEDED(s->Read(buf, sizeof(buf), &got)) && got > 0 && png.size() < (1 << 20)) png.append(buf, got);
            if (png.empty() || png == t->icon_png) return S_OK;
            t->icon_png = png;
            ui_tab_icon_changed(t);
            std::wstring host = url_host(t->url);
            if (!host.empty()) {
                CreateDirectoryW((g_state + L"\\iconos").c_str(), NULL);
                write_file(g_state + L"\\iconos\\" + hash_hex(host) + L".png", png);
            }
            return S_OK;
        }).Get());
}
static std::string cached_icon(const std::wstring &url) {
    std::string png;
    std::wstring host = url_host(url);
    if (!host.empty()) read_file(g_state + L"\\iconos\\" + hash_hex(host) + L".png", png);
    return png;
}
static void open_link_in_tab(int from_id, const std::wstring &url, bool activate) {
    int i = tab_index(from_id);
    tab_new(url, activate, i >= 0 ? i + 1 : -1);
}
static void attach(Tab *t, ComPtr<ICoreWebView2Controller> c) {
    int id = t->id;
    t->ctrl = c;
    t->view.Reset();
    c->get_CoreWebView2(&t->view);
    t->creating = false;
    t->suspended = false;
    EventRegistrationToken tok;
    ICoreWebView2 *v = t->view.Get();

    v->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            t->loading = true;
            t->progress = 0;
            t->load_started = now_ms();
            t->load_done = 0;
            set_progress(t, 0.08f);
            return S_OK;
        }).Get(), &tok);
    v->add_ContentLoading(Callback<ICoreWebView2ContentLoadingEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2ContentLoadingEventArgs *) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            set_progress(t, 0.5f);
            ComPtr<ICoreWebView2Controller2> c2;
            if (t->ctrl && SUCCEEDED(t->ctrl.As(&c2))) c2->put_DefaultBackgroundColor({ 255, 255, 255, 255 });   // as every browser
            return S_OK;
        }).Get(), &tok);
    v->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [id](ICoreWebView2 *v, ICoreWebView2SourceChangedEventArgs *) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            LPWSTR s = NULL; v->get_Source(&s);
            std::wstring u = s ? s : L"";
            CoTaskMemFree(s);
            if (u == L"about:blank" && !t->url.empty() && t->loading) return S_OK;
            if (url_host(u) != url_host(t->url)) { std::string png = cached_icon(u); if (!png.empty()) { t->icon_png = png; ui_tab_icon_changed(t); } }
            if (u != t->url) ui_notice_drop(t->id);
            t->url = u;
            if (t == active_tab() && !ui_address_focused()) ui_set_address_text(u);
            std::wstring ext_id; bool edge = false;
            if (t == active_tab() && store_extension_id(u, ext_id, edge)) {
                bool have = false;
                for (auto &e : g_exts) if (e.id == ext_id || (!e.folder.empty() && ends_with_i(e.folder, (L"\\" + ext_id).c_str()))) have = true;
                if (!have) ui_notice(L"Esta extensión se puede añadir a Pluma.", L"Añadir a Pluma", [ext_id, edge] { store_install(ext_id, edge); }, 0, t->id);
            }
            session_save_soon();
            ui_invalidate();
            return S_OK;
        }).Get(), &tok);
    v->add_HistoryChanged(Callback<ICoreWebView2HistoryChangedEventHandler>(
        [id](ICoreWebView2 *v, IUnknown *) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            BOOL b = FALSE, f = FALSE; v->get_CanGoBack(&b); v->get_CanGoForward(&f);
            t->can_back = b != FALSE; t->can_fwd = f != FALSE;
            ui_invalidate();
            return S_OK;
        }).Get(), &tok);
    v->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [id](ICoreWebView2 *v, IUnknown *) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            LPWSTR s = NULL; v->get_DocumentTitle(&s);
            t->title = s ? s : L"";
            CoTaskMemFree(s);
            if (t == active_tab()) SetWindowTextW(g_win, (t->title.empty() ? std::wstring(L"Pluma") : t->title + L" - Pluma").c_str());
            history_title(t->url, t->title);
            session_save_soon();
            ui_invalidate();
            return S_OK;
        }).Get(), &tok);
    v->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            BOOL ok = FALSE; a->get_IsSuccess(&ok);
            t->loading = false;
            t->progress = 1;
            t->load_done = now_ms();
            t->crashed = false;
            if (ok) history_add(t->url, t->title);
            if (ok && t == active_tab()) trim_after_load();
            if (g_open_asked >= 0 && t == active_tab()) {
                log_line(L"página lista " + std::to_wstring((int)(now_ms() - g_open_asked)) + L" ms después de pedirla" + (g_open_asked == 0 ? L" (arrancando)" : L" (desde memoria)"));
                g_open_asked = -1;
            }
            if (ok && t == active_tab()) {
                // a userscript or an extension from a store: offer to add it
                std::wstring path = t->url.substr(0, t->url.find_first_of(L"?#"));
                std::wstring ext_id; bool edge = false;
                if (ends_with_i(path, L".user.js")) {
                    t->view->ExecuteScript(L"(document.querySelector('pre')||document.body).innerText",
                        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([](HRESULT hr, LPCWSTR json) -> HRESULT {
                            if (FAILED(hr) || !json) return S_OK;
                            // the result is a JSON string: let the script module unquote and read it
                            std::wstring src = json;
                            Tab *at = active_tab();
                            ui_notice(L"Esta página es un script de usuario. ¿Lo instalas en Pluma?", L"Instalar", [src] {
                                std::wstring name;
                                if (script_install(src, name)) { tabs_set_scripts(); ui_toast(L"Script instalado: " + name); }
                                else ui_toast(L"No parece un script de usuario válido");
                            }, 0, at ? at->id : 0);
                            return S_OK;
                        }).Get());
                }
            }
            ui_invalidate();
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2_15> v15;
    if (SUCCEEDED(t->view.As(&v15)))
        v15->add_FaviconChanged(Callback<ICoreWebView2FaviconChangedEventHandler>(
            [id](ICoreWebView2 *, IUnknown *) -> HRESULT { on_icon(id); return S_OK; }).Get(), &tok);
    v->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT {
            // links that open a new window open a tab; Ctrl+click and the middle button (marked by the base script), one behind
            bool background = GetKeyState(VK_CONTROL) < 0;
            ComPtr<ICoreWebView2NewWindowRequestedEventArgs2> a2;
            if (SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&a2)))) {
                LPWSTR nm = NULL; a2->get_Name(&nm);
                if (nm && !wcscmp(nm, L"__pluma_bg")) background = true;
                CoTaskMemFree(nm);
            }
            ComPtr<ICoreWebView2Deferral> def;
            a->GetDeferral(&def);
            ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args = a;
            int at = tab_index(id);
            auto nt = std::make_unique<Tab>();
            nt->id = g_next_id++;
            nt->creating = true;
            LPWSTR u = NULL; a->get_Uri(&u);
            nt->url = u ? u : L"";
            CoTaskMemFree(u);
            nt->icon_png = cached_icon(nt->url);
            int nid = nt->id;
            int pos = at >= 0 ? at + 1 : (int)g_tabs.size();
            // after the tabs this one already opened, as Chrome does
            while (pos < (int)g_tabs.size() && g_tabs[pos]->creating) pos++;
            g_tabs.insert(g_tabs.begin() + pos, std::move(nt));
            if (g_active >= pos) g_active++;
            if (!background) tab_activate(pos);
            else { tab_by_id(nid)->hidden_since = now_ms(); ui_layout(); }
            take_view([nid, args, def](ComPtr<ICoreWebView2Controller> c) {
                Tab *t = tab_by_id(nid);
                if (!t || !c) { if (c) c->Close(); def->Complete(); return; }
                attach(t, c);
                args->put_NewWindow(t->view.Get());
                args->put_Handled(TRUE);
                def->Complete();
                if (t == active_tab()) tab_activate(g_active);
                else t->hidden_since = now_ms();
            });
            return S_OK;
        }).Get(), &tok);
    v->add_WindowCloseRequested(Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
        [id](ICoreWebView2 *, IUnknown *) -> HRESULT {
            int i = tab_index(id);
            if (i >= 0) tab_close(i);
            return S_OK;
        }).Get(), &tok);
    v->add_ContainsFullScreenElementChanged(Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
        [id](ICoreWebView2 *v, IUnknown *) -> HRESULT {
            BOOL fs = FALSE; v->get_ContainsFullScreenElement(&fs);
            if (tab_by_id(id) == active_tab()) set_fullscreen(fs != FALSE);
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2_8> v8;
    if (SUCCEEDED(t->view.As(&v8))) {
        v8->add_IsDocumentPlayingAudioChanged(Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
            [id](ICoreWebView2 *v, IUnknown *) -> HRESULT {
                Tab *t = tab_by_id(id);
                ComPtr<ICoreWebView2_8> v8;
                if (!t || FAILED(v->QueryInterface(IID_PPV_ARGS(&v8)))) return S_OK;
                BOOL a = FALSE; v8->get_IsDocumentPlayingAudio(&a);
                t->audible = a != FALSE;
                ui_invalidate();
                return S_OK;
            }).Get(), &tok);
    }
    ComPtr<ICoreWebView2_18> v18;
    if (SUCCEEDED(t->view.As(&v18)))
        v18->add_LaunchingExternalUriScheme(Callback<ICoreWebView2LaunchingExternalUriSchemeEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2LaunchingExternalUriSchemeEventArgs *a) -> HRESULT {
                // apps the user already trusts (YT-MP3's ytmp3:) open without the "open this app?" question
                LPWSTR u = NULL; a->get_Uri(&u);
                std::wstring uri = u ? u : L"";
                CoTaskMemFree(u);
                std::wstring scheme = lower(uri.substr(0, uri.find(L':')));
                for (auto &s : g_set.schemes)
                    if (scheme == s) {
                        a->put_Cancel(TRUE);
                        ShellExecuteW(NULL, L"open", uri.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        break;
                    }
                return S_OK;
            }).Get(), &tok);
    v->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
        [id](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *a) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND k; a->get_ProcessFailedKind(&k);
            Tab *t = tab_by_id(id);
            if (!t) return S_OK;
            if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED || k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                log_line(L"la página de una pestaña se ha cerrado (" + std::to_wstring((int)k) + L"): " + t->url);
                t->loading = false;
                if (t == active_tab()) t->view->Reload(); else t->crashed = true;
            }
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2_11> v11;
    if (SUCCEEDED(t->view.As(&v11)))
        v11->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
            [id](ICoreWebView2 *, ICoreWebView2ContextMenuRequestedEventArgs *a) -> HRESULT {
                // "open link in a new window" becomes "in a new tab": there is one window
                ComPtr<ICoreWebView2ContextMenuTarget> target;
                ComPtr<ICoreWebView2ContextMenuItemCollection> items;
                ComPtr<ICoreWebView2Environment9> e9;
                if (FAILED(a->get_ContextMenuTarget(&target)) || FAILED(a->get_MenuItems(&items)) || FAILED(g_env.As(&e9))) return S_OK;
                BOOL has = FALSE; target->get_HasLinkUri(&has);
                if (!has) return S_OK;
                LPWSTR lu = NULL; target->get_LinkUri(&lu);
                std::wstring link = lu ? lu : L"";
                CoTaskMemFree(lu);
                UINT32 n = 0; items->get_Count(&n);
                for (UINT32 i = 0; i < n; i++) {
                    ComPtr<ICoreWebView2ContextMenuItem> it;
                    if (FAILED(items->GetValueAtIndex(i, &it))) continue;
                    LPWSTR name = NULL; it->get_Name(&name);
                    bool is_new_window = name && !wcscmp(name, L"openLinkInNewWindow");
                    CoTaskMemFree(name);
                    if (!is_new_window) continue;
                    ComPtr<ICoreWebView2ContextMenuItem> mine;
                    if (FAILED(e9->CreateContextMenuItem(L"Abrir en una pestaña nueva", NULL, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &mine))) break;
                    EventRegistrationToken tk;
                    mine->add_CustomItemSelected(Callback<ICoreWebView2CustomItemSelectedEventHandler>(
                        [id, link](ICoreWebView2ContextMenuItem *, IUnknown *) -> HRESULT { open_link_in_tab(id, link, false); return S_OK; }).Get(), &tk);
                    items->RemoveValueAtIndex(i);
                    items->InsertValueAtIndex(i, mine.Get());
                    break;
                }
                return S_OK;
            }).Get(), &tok);
    c->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
        [](ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventArgs *a) -> HRESULT {
            COREWEBVIEW2_KEY_EVENT_KIND k; a->get_KeyEventKind(&k);
            if (k != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && k != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
            UINT vk = 0; a->get_VirtualKey(&vk);
            if (handle_shortcut(vk, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0)) a->put_Handled(TRUE);
            return S_OK;
        }).Get(), &tok);
    c->add_GotFocus(Callback<ICoreWebView2FocusChangedEventHandler>(
        [](ICoreWebView2Controller *, IUnknown *) -> HRESULT { ui_close_popups(); return S_OK; }).Get(), &tok);
    c->add_ZoomFactorChanged(Callback<ICoreWebView2ZoomFactorChangedEventHandler>(
        [](ICoreWebView2Controller *, IUnknown *) -> HRESULT { ui_invalidate(); return S_OK; }).Get(), &tok);

    v->AddScriptToExecuteOnDocumentCreated(base_script().c_str(), NULL);
    for (auto &src : scripts_sources())
        v->AddScriptToExecuteOnDocumentCreated(src.c_str(), Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [id](HRESULT hr, LPCWSTR sid) -> HRESULT { Tab *t = tab_by_id(id); if (t && SUCCEEDED(hr) && sid) t->scripts_ids.push_back(sid); return S_OK; }).Get());
}

// a few seconds after a page has loaded, the engine gives back what loading it left behind (at most every half
// minute; never the graphics nor the page on screen)
static void trim_after_load() {
    static double last = -1e9;
    if (now_ms() - last < 30000) return;
    last = now_ms();
    SetTimer(g_win, 0x51A6, 4000, [](HWND, UINT, UINT_PTR id, DWORD) { KillTimer(g_win, id); tabs_trim(TRIM_IDLE); });
}

static void ensure_view(Tab *t) {
    // gives a tab its view and loads its page (restored or unloaded tabs get here when looked at)
    if (t->view || t->creating || t->url.empty()) return;
    t->creating = true;
    int id = t->id;
    take_view([id](ComPtr<ICoreWebView2Controller> c) {
        Tab *t = tab_by_id(id);
        if (!t || !c) { if (c) c->Close(); if (t) t->creating = false; return; }
        attach(t, c);
        std::wstring url = t->pending.empty() ? t->url : t->pending;
        t->pending.clear();
        t->view->Navigate(url.c_str());
        if (t == active_tab()) tab_activate(g_active);
        else t->hidden_since = now_ms();
    });
}

void tabs_layout_views() {
    Tab *t = active_tab();
    RECT r = page_rect();
    if (t && t->ctrl) t->ctrl->put_Bounds(r);
}

// ---------------------------------------------------------------- tab operations
Tab *tab_new(const std::wstring &url, bool activate, int at) {
    auto t = std::make_unique<Tab>();
    t->id = g_next_id++;
    t->url = url;
    if (!url.empty()) t->icon_png = cached_icon(url);
    Tab *raw = t.get();
    if (at < 0 || at > (int)g_tabs.size()) at = (int)g_tabs.size();
    g_tabs.insert(g_tabs.begin() + at, std::move(t));
    if (g_active >= at && g_active >= 0) g_active++;
    if (activate || g_active < 0) tab_activate(at);
    else if (!url.empty()) { raw->hidden_since = now_ms(); ensure_view(raw); }
    ui_layout();
    session_save_soon();
    return raw;
}
static void release_view(Tab *t) {
    if (t->ctrl) { t->ctrl->put_IsVisible(FALSE); t->ctrl->Close(); }
    t->ctrl.Reset(); t->view.Reset();
    t->scripts_ids.clear();
    t->suspended = false; t->loading = false; t->audible = false; t->creating = false;
}
void tab_close(int i, bool from_mouse) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    Tab *t = g_tabs[i].get();
    if (!t->url.empty()) {
        g_closed.push_back({ t->url, t->title, t->icon_png, i });
        if (g_closed.size() > 25) g_closed.erase(g_closed.begin());
    }
    ui_tab_closing(t, from_mouse);
    ui_release_icon(t);
    release_view(t);
    bool was_active = i == g_active;
    g_tabs.erase(g_tabs.begin() + i);
    if (g_tabs.empty()) {
        g_active = -1;
        tab_new(L"", true);
        app_close_window();   // closing the last tab closes the window, as other browsers do
        return;
    }
    if (was_active) { g_active = -1; tab_activate(std::min(i, (int)g_tabs.size() - 1)); }
    else if (g_active > i) g_active--;
    ui_layout();
    session_save_soon();
}
void tab_reopen_closed() {
    if (g_closed.empty()) return;
    Closed c = g_closed.back();
    g_closed.pop_back();
    Tab *t = tab_new(c.url, true, std::min(c.index, (int)g_tabs.size()));
    t->title = c.title;
    t->icon_png = c.icon;
    ui_tab_icon_changed(t);
}
static void hide_view(Tab *t) {
    if (!t) return;
    t->hidden_since = now_ms();
    if (t->ctrl) t->ctrl->put_IsVisible(FALSE);
}
void tab_activate(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    Tab *old = active_tab();
    Tab *t = g_tabs[i].get();
    if (old && old != t) { hide_view(old); ui_notice_drop(old->id); }
    g_active = i;
    t->hidden_since = 0;
    SetWindowTextW(g_win, (t->title.empty() ? std::wstring(L"Pluma") : t->title + L" - Pluma").c_str());
    if (!ui_address_focused() || old != t) ui_set_address_text(t->url);
    if (t->view) {
        if (t->suspended) {
            ComPtr<ICoreWebView2_3> v3;
            if (SUCCEEDED(t->view.As(&v3))) v3->Resume();
            t->suspended = false;
        }
        ComPtr<ICoreWebView2_19> v19;
        if (SUCCEEDED(t->view.As(&v19))) v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        ui_layout();
        tabs_layout_views();
        t->ctrl->put_IsVisible(IsWindowVisible(g_win) || g_test ? TRUE : FALSE);
        if (t->crashed) { t->crashed = false; t->view->Reload(); }
        if (old != t && GetForegroundWindow() == g_win && !t->url.empty()) t->ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    } else {
        ui_layout();
        ensure_view(t);
    }
    if (t->url.empty() && old != t) ui_focus_address(false);
    ui_invalidate();
    session_save_soon();
}
void tab_navigate(Tab *t, const std::wstring &url) {
    if (!t || url.empty()) return;
    t->url = url;
    if (url_host(url) != url_host(t->url) || t->icon_png.empty()) { std::string png = cached_icon(url); t->icon_png = png; ui_tab_icon_changed(t); }
    if (t->view) {
        t->view->Navigate(url.c_str());
        t->ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    } else {
        t->pending = url;
        ensure_view(t);
    }
    t->loading = true;
    t->progress = 0.05f;
    ui_layout();
    ui_invalidate();
}
void tab_go_back() { Tab *t = active_tab(); if (t && t->view && t->can_back) t->view->GoBack(); }
void tab_go_forward() { Tab *t = active_tab(); if (t && t->view && t->can_fwd) t->view->GoForward(); }
void tab_reload(bool hard) {
    Tab *t = active_tab();
    if (!t) return;
    if (!t->view) { ensure_view(t); return; }
    if (hard) t->view->CallDevToolsProtocolMethod(L"Page.reload", L"{\"ignoreCache\":true}", NULL);
    else t->view->Reload();
}
void tab_stop() { Tab *t = active_tab(); if (t && t->view) t->view->Stop(); }
void tab_mute(Tab *t) {
    ComPtr<ICoreWebView2_8> v8;
    if (!t || !t->view || FAILED(t->view.As(&v8))) return;
    BOOL m = FALSE; v8->get_IsMuted(&m);
    v8->put_IsMuted(!m);
    t->muted = !m;
    ui_invalidate();
}
double tab_zoom_factor() {
    Tab *t = active_tab();
    double z = 1;
    if (t && t->ctrl) t->ctrl->get_ZoomFactor(&z);
    return z;
}
void tab_zoom(int dir) {
    static const double kSteps[] = { 0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0 };
    Tab *t = active_tab();
    if (!t || !t->ctrl) return;
    double z = tab_zoom_factor(), nz = 1.0;
    if (dir > 0) { nz = 5.0; for (double s : kSteps) if (s > z + 0.001) { nz = s; break; } }
    else if (dir < 0) { nz = 0.25; for (double s : kSteps) if (s < z - 0.001) nz = s; }
    t->ctrl->put_ZoomFactor(nz);
}
void tab_find() {
    Tab *t = active_tab();
    ComPtr<ICoreWebView2_28> v28;
    ComPtr<ICoreWebView2Environment15> e15;
    ComPtr<ICoreWebView2Find> find;
    ComPtr<ICoreWebView2FindOptions> o;
    if (!t || !t->view || FAILED(t->view.As(&v28)) || FAILED(g_env.As(&e15)) || FAILED(v28->get_Find(&find)) || FAILED(e15->CreateFindOptions(&o))) return;
    o->put_SuppressDefaultFindDialog(FALSE);
    t->ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    find->Start(o.Get(), Callback<ICoreWebView2FindStartCompletedHandler>([](HRESULT) -> HRESULT { return S_OK; }).Get());
}
void tab_print() {
    Tab *t = active_tab();
    ComPtr<ICoreWebView2_16> v16;
    if (t && t->view && SUCCEEDED(t->view.As(&v16))) v16->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_BROWSER);
}
void tab_devtools() { Tab *t = active_tab(); if (t && t->view) t->view->OpenDevToolsWindow(); }
void tab_downloads() {
    Tab *t = active_tab();
    ComPtr<ICoreWebView2_9> v9;
    if (t && t->view && SUCCEEDED(t->view.As(&v9))) v9->OpenDefaultDownloadDialog();
}
void tabs_set_scripts() {
    auto sources = scripts_sources();
    for (auto &t : g_tabs) {
        if (!t->view) continue;
        for (auto &sid : t->scripts_ids) t->view->RemoveScriptToExecuteOnDocumentCreated(sid.c_str());
        t->scripts_ids.clear();
        int id = t->id;
        for (auto &src : sources)
            t->view->AddScriptToExecuteOnDocumentCreated(src.c_str(), Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [id](HRESULT hr, LPCWSTR sid) -> HRESULT { Tab *t = tab_by_id(id); if (t && SUCCEEDED(hr) && sid) t->scripts_ids.push_back(sid); return S_OK; }).Get());
    }
}

// ---------------------------------------------------------------- memory: freeze, then let go
void tabs_tick() {
    double now = now_ms();
    // minimized or closed for a while: everything gives back what it does not use (once every half minute)
    static double last_full_trim = 0, last_idle_trim = 0;
    if ((!IsWindowVisible(g_win) || IsIconic(g_win)) && now - last_full_trim > 30000) { last_full_trim = now; tabs_trim(TRIM_ALL); }
    // on screen but nobody has touched the PC for a minute: the engine's own processes give back what they do not
    // use too (not the graphics, nor the tab on screen: nothing to notice when the person comes back)
    LASTINPUTINFO li = { sizeof(li) };
    static DWORD idle_ms = [] { wchar_t v[16] = L""; return GetEnvironmentVariableW(L"PLUMA_REPOSO_S", v, 16) ? (DWORD)_wtoi(v) * 1000 : 60000; }();
    bool idle = GetLastInputInfo(&li) && GetTickCount() - li.dwTime > idle_ms;
    if (idle_ms < 60000) idle = now > idle_ms;   // tests: as if nobody touched the PC
    if (IsWindowVisible(g_win) && !IsIconic(g_win) && idle && (last_idle_trim == 0 || now - last_idle_trim > 120000)) {
        last_idle_trim = now;
        tabs_trim(TRIM_IDLE);
    }
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    bool pressure = ms.dwMemoryLoad >= 85;   // the PC is short of memory: let go of the oldest hidden tab now
    Tab *oldest = nullptr;
    for (auto &up : g_tabs) {
        Tab *t = up.get();
        if (!t->view || t->hidden_since == 0 || t->audible || t->creating) continue;
        double hidden = now - t->hidden_since;
        if (!t->suspended && hidden > g_set.sleep_s * 1000.0) {
            ComPtr<ICoreWebView2_19> v19;
            if (SUCCEEDED(t->view.As(&v19))) v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
            ComPtr<ICoreWebView2_3> v3;
            int id = t->id;
            t->suspended = true;
            if (SUCCEEDED(t->view.As(&v3)))
                v3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>([id](HRESULT hr, BOOL ok) -> HRESULT {
                    Tab *t = tab_by_id(id);
                    if (t && (FAILED(hr) || !ok)) t->suspended = false;
                    else if (t) tabs_trim(TRIM_FROZEN);   // frozen: its process can give its memory back now
                    return S_OK;
                }).Get());
        }
        if (g_set.discard_min > 0 && hidden > g_set.discard_min * 60000.0) { release_view(t); continue; }
        if (!oldest || t->hidden_since < oldest->hidden_since) oldest = t;
    }
    if (pressure && oldest && now - oldest->hidden_since > 10000) {
        log_line(L"poca memoria en el PC (" + std::to_wstring(ms.dwMemoryLoad) + L" %): se descarga una pestaña escondida");
        release_view(oldest);
    }
    ui_invalidate();
}
void tabs_discard_all() {
    for (auto &t : g_tabs) release_view(t.get());
}

// Memory a process holds but is not using goes back to Windows (EmptyWorkingSet): it stays the process's, and comes
// back from RAM in microseconds if it is needed again. While Pluma is on screen this is done only to processes that
// draw nothing visible or audible (frozen tabs, the spare view, the extensions' workers); with the window closed or
// minimized, to all of them.
static UINT32 main_frame(Tab *t) {
    ComPtr<ICoreWebView2_20> v20;
    UINT32 id = 0;
    if (t && t->view && SUCCEEDED(t->view.As(&v20))) v20->get_FrameId(&id);
    return id;
}
static UINT32 root_frame(ICoreWebView2FrameInfo *f) {
    ComPtr<ICoreWebView2FrameInfo> cur = f;
    UINT32 id = 0;
    for (int depth = 0; cur && depth < 32; depth++) {
        ComPtr<ICoreWebView2FrameInfo2> f2;
        if (FAILED(cur.As(&f2))) break;
        f2->get_FrameId(&id);
        ComPtr<ICoreWebView2FrameInfo> parent;
        if (FAILED(f2->get_ParentFrameInfo(&parent)) || !parent) break;
        cur = parent;
    }
    return id;
}
static void empty_ws(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) { EmptyWorkingSet(h); CloseHandle(h); }
}
void tabs_trim(int mode) {
    bool everything = mode == TRIM_ALL;
    ComPtr<ICoreWebView2Environment13> e13;
    if (!g_env || FAILED(g_env.As(&e13))) return;
    // the frames that must keep their memory: the tab on screen, tabs playing sound, tabs not frozen yet
    std::vector<UINT32> keep;
    for (auto &t : g_tabs) {
        if (!t->view) continue;
        bool on_screen = !everything && t.get() == active_tab();
        bool sounding = t->audible && mode != TRIM_IDLE;
        if (on_screen || sounding || (!everything && !t->suspended && !t->audible)) keep.push_back(main_frame(t.get()));
    }
    e13->GetProcessExtendedInfos(Callback<ICoreWebView2GetProcessExtendedInfosCompletedHandler>(
        [mode, everything, keep](HRESULT hr, ICoreWebView2ProcessExtendedInfoCollection *list) -> HRESULT {
            UINT n = 0;
            if (FAILED(hr) || !list) return S_OK;
            list->get_Count(&n);
            for (UINT i = 0; i < n; i++) {
                ComPtr<ICoreWebView2ProcessExtendedInfo> x;
                ComPtr<ICoreWebView2ProcessInfo> pi;
                if (FAILED(list->GetValueAtIndex(i, &x)) || FAILED(x->get_ProcessInfo(&pi))) continue;
                INT32 pid = 0; COREWEBVIEW2_PROCESS_KIND kind;
                pi->get_ProcessId(&pid); pi->get_Kind(&kind);
                // while on screen the graphics keep everything (no stutter); the engine too, unless the PC is idle
                if (mode == TRIM_FROZEN && kind != COREWEBVIEW2_PROCESS_KIND_RENDERER) continue;
                if (mode == TRIM_IDLE && kind == COREWEBVIEW2_PROCESS_KIND_GPU) continue;
                bool needed = false;
                ComPtr<ICoreWebView2FrameInfoCollection> frames;
                if (SUCCEEDED(x->get_AssociatedFrameInfos(&frames)) && frames) {
                    ComPtr<ICoreWebView2FrameInfoCollectionIterator> it;
                    BOOL has = FALSE;
                    if (SUCCEEDED(frames->GetIterator(&it)))
                        for (it->get_HasCurrent(&has); has && !needed; it->MoveNext(&has)) {
                            ComPtr<ICoreWebView2FrameInfo> f;
                            if (FAILED(it->GetCurrent(&f))) break;
                            UINT32 r = root_frame(f.Get());
                            needed = std::find(keep.begin(), keep.end(), r) != keep.end();
                        }
                }
                if (!needed) empty_ws((DWORD)pid);
            }
            if (everything) EmptyWorkingSet(GetCurrentProcess());
            return S_OK;
        }).Get());
}
void tabs_on_hide() {
    // closed but kept in memory: the pages freeze where they are (to come back as they were), except those playing
    // sound, which are let go so the music stops; then every process gives its memory back
    for (auto &up : g_tabs) {
        Tab *t = up.get();
        if (!t->view) continue;
        if (t->audible) { release_view(t); continue; }
        t->hidden_since = now_ms();
        t->ctrl->put_IsVisible(FALSE);
        ComPtr<ICoreWebView2_19> v19;
        if (SUCCEEDED(t->view.As(&v19))) v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
        ComPtr<ICoreWebView2_3> v3;
        int id = t->id;
        t->suspended = true;
        if (SUCCEEDED(t->view.As(&v3)))
            v3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>([id](HRESULT hr, BOOL ok) -> HRESULT {
                Tab *t = tab_by_id(id);
                if (t && (FAILED(hr) || !ok)) t->suspended = false;
                return S_OK;
            }).Get());
    }
    SetTimer(g_win, 0x51A4, 2500, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(g_win, id);
        if (IsWindowVisible(g_win) && !IsIconic(g_win)) return;
        tabs_trim(TRIM_ALL);
        SetTimer(g_win, 0x51A5, 1500, [](HWND, UINT, UINT_PTR id2, DWORD) {
            KillTimer(g_win, id2);
            log_line(L"cerrado y en memoria: " + std::to_wstring(engine_memory_mb()) + L" MB");
        });
    });
}

// ---------------------------------------------------------------- session (sesion.tsv)
void session_save() {
    std::wstring out = L"activa\t" + std::to_wstring(std::max(0, g_active)) + L"\n";
    for (auto &t : g_tabs) {
        std::wstring title = t->title;
        for (auto &c : title) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        out += t->url + L"\t" + title + L"\n";
    }
    write_file(g_state + L"\\sesion.tsv", utf8(out));
}
void session_restore() {
    std::string data;
    if (!read_file(g_state + L"\\sesion.tsv", data)) return;
    std::wstring w = wide(data);
    int active = 0;
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        std::wstring a = line.substr(0, tab), b = line.substr(tab + 1);
        if (a == L"activa") { active = _wtoi(b.c_str()); continue; }
        auto t = std::make_unique<Tab>();
        t->id = g_next_id++;
        t->url = a; t->title = b;
        t->icon_png = cached_icon(a);
        t->hidden_since = now_ms();
        g_tabs.push_back(std::move(t));
    }
    if (g_tabs.empty()) return;
    g_active = std::min(std::max(active, 0), (int)g_tabs.size() - 1);
    g_tabs[g_active]->hidden_since = 0;
    for (auto &t : g_tabs) ui_tab_icon_changed(t.get());
    SetWindowTextW(g_win, (g_tabs[g_active]->title.empty() ? std::wstring(L"Pluma") : g_tabs[g_active]->title + L" - Pluma").c_str());
    ui_set_address_text(g_tabs[g_active]->url);
    ui_layout();
}

// ---------------------------------------------------------------- memory in use (private working set, as the Task Manager)
struct PmcEx2 {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
           QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage, PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
static size_t private_ws(HANDLE p) {
    PmcEx2 m = {}; m.cb = sizeof(m);
    if (!GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m))) return 0;
    return m.PrivateWorkingSetSize;
}
size_t engine_memory_mb() {
    size_t total = private_ws(GetCurrentProcess());
    ComPtr<ICoreWebView2Environment8> e8;
    ComPtr<ICoreWebView2ProcessInfoCollection> list;
    if (g_env && SUCCEEDED(g_env.As(&e8)) && SUCCEEDED(e8->GetProcessInfos(&list)) && list) {
        UINT n = 0; list->get_Count(&n);
        for (UINT i = 0; i < n; i++) {
            ComPtr<ICoreWebView2ProcessInfo> pi;
            INT32 pid = 0;
            if (FAILED(list->GetValueAtIndex(i, &pi)) || FAILED(pi->get_ProcessId(&pid))) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
            if (h) { total += private_ws(h); CloseHandle(h); }
        }
    }
    return total >> 20;
}
