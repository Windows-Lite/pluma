// Pluma: Chrome extensions, through WebView2's own support. They come from two folders (the one beside Pluma.exe,
// which brings uBlock Origin Lite, and %LOCALAPPDATA%\Pluma\extensiones), or from the Chrome Web Store / Edge
// Add-ons with one click. A toolbar button opens an extension's popup in a small window of its own.
#include "pluma.h"
#include "json.h"
#include <thread>
#include <urlmon.h>

std::vector<Extension> g_exts;
std::wstring extensions_folder() {
    std::wstring f = g_data + L"\\extensiones";
    CreateDirectoryW(f.c_str(), NULL);
    return f;
}
// which folder became which extension id (WebView2 does not say), and the bundled ones the user removed
struct Known { std::wstring folder, id; bool removed = false; };
static std::vector<Known> g_known;
static std::wstring known_file() { return g_data + L"\\extensiones.tsv"; }
static void known_load() {
    g_known.clear();
    std::string d;
    if (!read_file(known_file(), d)) return;
    std::wstring w = wide(d);
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        size_t a = line.find(L'\t'), b = a == std::wstring::npos ? a : line.find(L'\t', a + 1);
        if (b == std::wstring::npos) continue;
        g_known.push_back({ line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1, 7) == L"quitada" });
    }
}
static void known_save() {
    std::wstring o;
    for (auto &k : g_known) o += k.folder + L"\t" + k.id + L"\t" + (k.removed ? L"quitada" : L"si") + L"\n";
    write_file(known_file(), utf8(o));
}
static Known *known_by_folder(const std::wstring &f) { for (auto &k : g_known) if (!_wcsicmp(k.folder.c_str(), f.c_str())) return &k; return nullptr; }
static Known *known_by_id(const std::wstring &id) { for (auto &k : g_known) if (k.id == id) return &k; return nullptr; }

static std::vector<std::wstring> extension_folders() {
    std::vector<std::wstring> out;
    for (const std::wstring &base : { g_root + L"\\extensiones", extensions_folder() }) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring f = base + L"\\" + fd.cFileName;
            if (PathFileExistsW((f + L"\\manifest.json").c_str())) out.push_back(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}
static Json read_manifest(const std::wstring &folder) {
    std::string d;
    if (!read_file(folder + L"\\manifest.json", d)) return Json();
    if (d.size() >= 3 && (unsigned char)d[0] == 0xEF) d = d.substr(3);
    return json_parse(wide(d));
}
static std::wstring pick_icon(const std::wstring &folder, const Json &icons) {
    // the size closest to what the toolbar draws (about 32 px on a scaled screen)
    if (icons.is_str()) return folder + L"\\" + icons.s;
    if (!icons.is_obj()) return L"";
    std::wstring best; int best_d = 1 << 30;
    int want = (int)(18 * ui_scale() + 0.5f) * 2 > 40 ? 48 : 32;
    for (auto &kv : icons.obj) {
        if (!kv.second.is_str()) continue;
        int d = abs(_wtoi(kv.first.c_str()) - want);
        if (d < best_d) { best_d = d; best = kv.second.s; }
    }
    std::wstring path = best.empty() ? L"" : folder + L"\\" + best;
    for (auto &c : path) if (c == L'/') c = L'\\';
    return path;
}
static void fill_from_manifest(Extension &e) {
    if (e.folder.empty()) return;
    Json m = read_manifest(e.folder);
    const Json &act = m[L"action"].is_obj() ? m[L"action"] : m[L"browser_action"];
    if (act[L"default_popup"].is_str()) e.popup = act[L"default_popup"].s;
    if (m[L"options_ui"][L"page"].is_str()) e.options = m[L"options_ui"][L"page"].s;
    else if (m[L"options_page"].is_str()) e.options = m[L"options_page"].s;
    std::wstring icon = pick_icon(e.folder, act[L"default_icon"]);
    if (icon.empty() || !PathFileExistsW(icon.c_str())) icon = pick_icon(e.folder, m[L"icons"]);
    // WIC reads PNG and the like, not SVG
    if (!icon.empty() && ends_with_i(icon, L".svg")) icon.clear();
    e.icon_path = icon;
    while (!e.popup.empty() && e.popup[0] == L'/') e.popup.erase(0, 1);
    while (!e.options.empty() && e.options[0] == L'/') e.options.erase(0, 1);
}

static void refresh_list(ICoreWebView2Profile *pr, std::function<void()> done) {
    ComPtr<ICoreWebView2Profile7> p7;
    if (FAILED(pr->QueryInterface(IID_PPV_ARGS(&p7)))) { if (done) done(); return; }
    p7->GetBrowserExtensions(Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
        [done](HRESULT hr, ICoreWebView2BrowserExtensionList *list) -> HRESULT {
            g_exts.clear();
            UINT n = 0;
            if (SUCCEEDED(hr) && list) list->get_Count(&n);
            for (UINT i = 0; i < n; i++) {
                ComPtr<ICoreWebView2BrowserExtension> x;
                if (FAILED(list->GetValueAtIndex(i, &x))) continue;
                LPWSTR id = NULL, name = NULL; BOOL on = FALSE;
                x->get_Id(&id); x->get_Name(&name); x->get_IsEnabled(&on);
                Extension e;
                e.id = id ? id : L""; e.name = name ? name : L""; e.enabled = on != FALSE;
                CoTaskMemFree(id); CoTaskMemFree(name);
                Known *k = known_by_id(e.id);
                e.builtin = !k;   // Edge's own (PDF viewer, clipboard): not shown
                if (k) e.folder = k->folder;
                fill_from_manifest(e);
                g_exts.push_back(e);
            }
            std::sort(g_exts.begin(), g_exts.end(), [](const Extension &a, const Extension &b) { return lower(a.name) < lower(b.name); });
            if (done) done();
            return S_OK;
        }).Get());
}
static void add_folders(ICoreWebView2Profile *pr, std::vector<std::wstring> todo, std::function<void()> done) {
    if (todo.empty()) { done(); return; }
    ComPtr<ICoreWebView2Profile7> p7;
    if (FAILED(pr->QueryInterface(IID_PPV_ARGS(&p7)))) { done(); return; }
    std::wstring folder = todo.back();
    todo.pop_back();
    ComPtr<ICoreWebView2Profile> keep = pr;
    p7->AddBrowserExtension(folder.c_str(), Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
        [folder, todo, done, keep](HRESULT hr, ICoreWebView2BrowserExtension *x) -> HRESULT {
            if (SUCCEEDED(hr) && x) {
                LPWSTR id = NULL; x->get_Id(&id);
                Known *k = known_by_folder(folder);
                if (k) { k->id = id ? id : L""; k->removed = false; }
                else g_known.push_back({ folder, id ? id : L"", false });
                CoTaskMemFree(id);
                known_save();
                log_line(L"extensión añadida: " + folder);
            } else {
                wchar_t b[16]; swprintf_s(b, L"%08X", (unsigned)hr);
                log_line(L"no se pudo añadir la extensión " + folder + L" (" + b + L")");
            }
            add_folders(keep.Get(), todo, done);
            return S_OK;
        }).Get());
}
// what waits for the list of extensions (the incognito profile gets them once it is read)
static bool g_ext_ready;
static std::vector<std::function<void()>> g_ext_waiters;
void ext_when_ready(std::function<void()> f) { if (g_ext_ready) f(); else g_ext_waiters.push_back(f); }
static void ext_ready() {
    g_ext_ready = true;
    auto w = std::move(g_ext_waiters);
    g_ext_waiters.clear();
    for (auto &f : w) f();
}
void ext_sync(std::function<void()> done_) {
    auto done = [done_] { ext_ready(); if (done_) done_(); };
    with_profile([done](ICoreWebView2Profile *pr) {
        known_load();
        ComPtr<ICoreWebView2Profile> keep = pr;
        refresh_list(pr, [keep, done] {
            // folders that are not installed yet (or whose extension went missing) get added
            std::vector<std::wstring> todo;
            for (auto &f : extension_folders()) {
                Known *k = known_by_folder(f);
                if (k && k->removed) continue;
                bool installed = false;
                if (k) for (auto &e : g_exts) if (e.id == k->id) installed = true;
                if (!installed) todo.push_back(f);
            }
            if (todo.empty()) { done(); return; }
            add_folders(keep.Get(), todo, [keep, done] { refresh_list(keep.Get(), done); });
        });
    });
}
void ext_add_all_to(ICoreWebView2Profile *pr) {
    // the incognito profile gets the same extensions as the ordinary one (those on: uBlock above all)
    ComPtr<ICoreWebView2Profile7> p7;
    if (!pr || FAILED(pr->QueryInterface(IID_PPV_ARGS(&p7)))) return;
    for (auto &e : g_exts) {
        if (e.builtin || !e.enabled || e.folder.empty()) continue;
        std::wstring folder = e.folder;
        p7->AddBrowserExtension(folder.c_str(), Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [folder](HRESULT hr, ICoreWebView2BrowserExtension *) -> HRESULT {
                if (FAILED(hr)) { wchar_t b[16]; swprintf_s(b, L"%08X", (unsigned)hr); log_line(L"incógnito: no se pudo añadir " + folder + L" (" + b + L")"); }
                return S_OK;
            }).Get());
    }
}
static void with_extension(const std::wstring &id, std::function<void(ICoreWebView2BrowserExtension *)> f) {
    with_profile([id, f](ICoreWebView2Profile *pr) {
        ComPtr<ICoreWebView2Profile7> p7;
        if (FAILED(pr->QueryInterface(IID_PPV_ARGS(&p7)))) return;
        p7->GetBrowserExtensions(Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [id, f](HRESULT hr, ICoreWebView2BrowserExtensionList *list) -> HRESULT {
                UINT n = 0;
                if (SUCCEEDED(hr) && list) list->get_Count(&n);
                for (UINT i = 0; i < n; i++) {
                    ComPtr<ICoreWebView2BrowserExtension> x;
                    LPWSTR xid = NULL;
                    if (FAILED(list->GetValueAtIndex(i, &x)) || FAILED(x->get_Id(&xid))) continue;
                    bool same = xid && id == xid;
                    CoTaskMemFree(xid);
                    if (same) { f(x.Get()); break; }
                }
                return S_OK;
            }).Get());
    });
}
void ext_toggle(size_t i) {
    if (i >= g_exts.size()) return;
    std::wstring id = g_exts[i].id;
    bool on = !g_exts[i].enabled;
    g_exts[i].enabled = on;
    with_extension(id, [on](ICoreWebView2BrowserExtension *x) {
        x->Enable(on, Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>([](HRESULT) -> HRESULT { ui_layout(); return S_OK; }).Get());
    });
}
static void delete_tree(const std::wstring &dir) {
    std::wstring from = dir + L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}
void ext_remove(size_t i) {
    if (i >= g_exts.size()) return;
    Extension e = g_exts[i];
    g_exts.erase(g_exts.begin() + i);
    with_extension(e.id, [e](ICoreWebView2BrowserExtension *x) {
        x->Remove(Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>([e](HRESULT hr) -> HRESULT {
            Known *k = known_by_id(e.id);
            bool mine = !e.folder.empty() && !_wcsnicmp(e.folder.c_str(), extensions_folder().c_str(), extensions_folder().size());
            if (mine) {
                // installed from a store: its files go too
                delete_tree(e.folder);
                g_known.erase(std::remove_if(g_known.begin(), g_known.end(), [&](const Known &q) { return q.id == e.id; }), g_known.end());
            } else if (k) k->removed = true;   // bundled with Pluma: remember not to add it back
            known_save();
            ui_toast(T(L"Extensión quitada: ") + e.name);
            ui_layout();
            return S_OK;
        }).Get());
    });
    ui_layout();
}
bool ext_bundled_removed() {
    // one of those that come with Pluma (uBlock Origin Lite) was taken off
    std::wstring mine = g_root + L"\\extensiones\\";
    for (auto &k : g_known) if (k.removed && !_wcsnicmp(k.folder.c_str(), mine.c_str(), mine.size())) return true;
    return false;
}
void ext_restore_bundled() {
    std::wstring mine = g_root + L"\\extensiones\\";
    for (auto &k : g_known) if (k.removed && !_wcsnicmp(k.folder.c_str(), mine.c_str(), mine.size())) k.removed = false;
    known_save();
    ext_sync([] { ui_layout(); ui_toast(T(L"Extensión añadida de nuevo")); });
}
void ext_open_options(size_t i) {
    if (i >= g_exts.size() || g_exts[i].options.empty()) return;
    tab_new(L"chrome-extension://" + g_exts[i].id + L"/" + g_exts[i].options, true);
}

// ---------------------------------------------------------------- an extension's popup
static HWND g_pop;
static ComPtr<ICoreWebView2Controller> g_pop_ctrl;
static RECT g_pop_anchor;
static void pop_close() {
    if (g_pop_ctrl) { g_pop_ctrl->Close(); g_pop_ctrl.Reset(); }
    if (g_pop) { HWND h = g_pop; g_pop = NULL; DestroyWindow(h); }
}
static void pop_size(int w, int h) {
    float s = ui_scale();
    w = std::max((int)(160 * s), std::min(w, (int)(800 * s)));
    h = std::max((int)(60 * s), std::min(h, (int)(600 * s)));
    int x = g_pop_anchor.right - w, y = g_pop_anchor.bottom + (int)(6 * s);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&g_pop_anchor, MONITOR_DEFAULTTONEAREST), &mi);
    x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - w));
    SetWindowPos(g_pop, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
    if (g_pop_ctrl) { RECT r = { 0, 0, w, h }; g_pop_ctrl->put_Bounds(r); }
}
static void pop_measure(bool first) {
    // as big as its page wants, within limits; measured again during its first second (uBlock Origin Lite fills its
    // popup in as it goes: measured once, it came out cut in half)
    ComPtr<ICoreWebView2> v;
    if (!g_pop_ctrl || FAILED(g_pop_ctrl->get_CoreWebView2(&v)) || !v) return;
    v->ExecuteScript(L"(() => { const d = document.documentElement, b = document.body;"
                     L" return [Math.ceil(Math.max(d.scrollWidth, b ? b.scrollWidth : 0)), Math.ceil(Math.max(d.scrollHeight, b ? b.scrollHeight : 0))]; })()",
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([first](HRESULT hr, LPCWSTR r) -> HRESULT {
            if (!g_pop || FAILED(hr) || !r) return S_OK;
            int w = 0, h = 0;
            if (swscanf_s(r, L"[%d,%d]", &w, &h) == 2) {
                float s = ui_scale();
                RECT now; GetClientRect(g_pop, &now);
                int nw = (int)(w * s), nh = (int)(h * s);
                if (first || abs(nw - now.right) > 2 || abs(nh - now.bottom) > 2) pop_size(nw, nh);
            }
            if (first) { SetForegroundWindow(g_pop); if (g_pop_ctrl) g_pop_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
            return S_OK;
        }).Get());
}
static LRESULT CALLBACK pop_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (h == g_pop) {
            int n = (int)(INT_PTR)GetPropW(h, L"medidas") + 1;
            SetPropW(h, L"medidas", (HANDLE)(INT_PTR)n);
            pop_measure(false);
            if (n >= 6) KillTimer(h, wp);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && h == g_pop) {
            // clicking anywhere else closes it, as in Chrome (after this message: not from inside it)
            PostMessageW(h, WM_CLOSE, 0, 0);
        } else if (g_pop_ctrl) g_pop_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_CLOSE: if (h == g_pop) pop_close(); else DestroyWindow(h); return 0;
    case WM_SIZE: if (g_pop_ctrl && h == g_pop) { RECT r; GetClientRect(h, &r); g_pop_ctrl->put_Bounds(r); } return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void ext_open_popup(size_t i, RECT anchor) {
    if (i >= g_exts.size() || !g_env) return;
    if (g_pop) { pop_close(); return; }
    Extension e = g_exts[i];
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = pop_proc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"PlumaExtension";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
        reg = true;
    }
    g_pop_anchor = anchor;
    g_pop = CreateWindowExW(WS_EX_TOOLWINDOW, L"PlumaExtension", e.name.c_str(), WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10, 10, g_win, NULL, g_inst, NULL);
    DWORD round = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(g_pop, 33, &round, sizeof(round));
    const Palette &p = pal();
    COLORREF border = RGB((int)(p.line_strong.r * 255), (int)(p.line_strong.g * 255), (int)(p.line_strong.b * 255));
    DwmSetWindowAttribute(g_pop, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    // on the screen before its page goes in: a page made in a hidden window stayed "hidden" and drew nothing (the
    // popup came out empty, 25/10)
    pop_size((int)(340 * ui_scale()), (int)(120 * ui_scale()));
    HWND me = g_pop;
    Tab *t = active_tab();
    std::wstring page_url = t ? t->url : L"";
    create_controller(g_pop, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [me, e, page_url](HRESULT hr, ICoreWebView2Controller *c) -> HRESULT {
            if (FAILED(hr) || !c || me != g_pop) { if (c) c->Close(); return S_OK; }
            g_pop_ctrl = c;
            ComPtr<ICoreWebView2> v;
            c->get_CoreWebView2(&v);
            EventRegistrationToken tok;
            // the popup asks for "the active tab": that is the page the person is on, not the popup itself
            std::wstring shim = L"(() => { const want = " + json_escape(page_url) + L"; const api = (globalThis.chrome && chrome.tabs) ? chrome.tabs : null;"
                L" if (!api || !api.query) return; const orig = api.query.bind(api);"
                L" const pick = (all) => { const own = location.origin; const real = all.filter(t => !(t.url || '').startsWith(own));"
                L" const exact = real.filter(t => t.url === want); return exact.length ? [exact[0]] : real.filter(t => t.active).slice(0, 1); };"
                L" api.query = function (q, cb) { if (q && q.active && (q.currentWindow || q.lastFocusedWindow)) {"
                L" const p = orig({}).then(pick); if (cb) { p.then(cb); return; } return p; } return orig(q, cb); }; })();";
            v->AddScriptToExecuteOnDocumentCreated(shim.c_str(), NULL);
            v->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT {
                    pop_measure(true);
                    if (g_pop) SetTimer(g_pop, 1, 180, NULL);
                    return S_OK;
                }).Get(), &tok);
            v->add_WindowCloseRequested(Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
                [](ICoreWebView2 *, IUnknown *) -> HRESULT { if (g_pop) PostMessageW(g_pop, WM_CLOSE, 0, 0); return S_OK; }).Get(), &tok);
            v->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT {
                    LPWSTR u = NULL; a->get_Uri(&u);
                    std::wstring url = u ? u : L"";
                    CoTaskMemFree(u);
                    a->put_Handled(TRUE);
                    if (g_pop) PostMessageW(g_pop, WM_CLOSE, 0, 0);
                    ui_post([url] { tab_new(url, true); });
                    return S_OK;
                }).Get(), &tok);
            c->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                [](ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventArgs *a) -> HRESULT {
                    UINT vk = 0; a->get_VirtualKey(&vk);
                    if (vk == VK_ESCAPE) { a->put_Handled(TRUE); if (g_pop) PostMessageW(g_pop, WM_CLOSE, 0, 0); }
                    return S_OK;
                }).Get(), &tok);
            RECT r; GetClientRect(g_pop, &r);
            c->put_Bounds(r);
            c->put_IsVisible(TRUE);
            std::wstring url = L"chrome-extension://" + e.id + L"/" + e.popup;
            v->Navigate(url.c_str());
            return S_OK;
        }).Get());
}

// ---------------------------------------------------------------- installing from a store
bool store_extension_id(const std::wstring &url, std::wstring &id, bool &edge) {
    std::wstring u = lower(url);
    edge = starts_with(u, L"https://microsoftedge.microsoft.com/addons/detail/");
    if (!edge && !starts_with(u, L"https://chromewebstore.google.com/detail/") && !starts_with(u, L"https://chrome.google.com/webstore/detail/")) return false;
    std::wstring path = u.substr(0, u.find_first_of(L"?#"));
    // the id is the 32 letters a-p at the end of the path
    while (!path.empty() && path.back() == L'/') path.pop_back();
    std::wstring last = path.substr(path.rfind(L'/') + 1);
    if (last.size() != 32) return false;
    for (wchar_t c : last) if (c < L'a' || c > L'p') return false;
    id = last;
    return true;
}
static bool unpack_crx(const std::wstring &crx, const std::wstring &dest) {
    std::string d;
    if (!read_file(crx, d) || d.size() < 16 || d.compare(0, 4, "Cr24") != 0) return false;
    unsigned ver = *(const unsigned *)(d.data() + 4);
    size_t zip_at;
    if (ver == 3) zip_at = 12 + *(const unsigned *)(d.data() + 8);
    else if (ver == 2) zip_at = 16 + *(const unsigned *)(d.data() + 8) + *(const unsigned *)(d.data() + 12);
    else return false;
    if (zip_at >= d.size()) return false;
    std::wstring zip = crx + L".zip";
    if (!write_file(zip, d.substr(zip_at))) return false;
    delete_tree(dest);
    SHCreateDirectoryExW(NULL, dest.c_str(), NULL);
    // Windows' own tar reads zip files (and refuses paths that climb out of the folder)
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dest + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    bool ok = CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi) != 0;
    DWORD code = 1;
    if (ok) { WaitForSingleObject(pi.hProcess, 60000); GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    DeleteFileW(zip.c_str());
    delete_tree(dest + L"\\_metadata");   // the store's signatures: a loose extension with it does not load
    return ok && code == 0 && PathFileExistsW((dest + L"\\manifest.json").c_str());
}
void store_install(const std::wstring &id, bool edge) {
    ui_toast(T(L"Descargando la extensión..."));
    std::wstring dest = extensions_folder() + L"\\" + id;
    std::wstring tmp = g_data + L"\\" + id + L".crx";
    std::wstring url = edge
        ? L"https://edge.microsoft.com/extensionwebstorebase/v1/crx?response=redirect&x=id%3D" + id + L"%26installsource%3Dondemand%26uc"
        : L"https://clients2.google.com/service/update2/crx?response=redirect&prodversion=155.0.0.0&acceptformat=crx2,crx3&x=id%3D" + id + L"%26uc";
    std::thread([id, dest, tmp, url] {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        bool ok = SUCCEEDED(URLDownloadToFileW(NULL, url.c_str(), tmp.c_str(), 0, NULL)) && unpack_crx(tmp, dest);
        DeleteFileW(tmp.c_str());
        CoUninitialize();
        ui_post([ok, dest] {
            if (!ok) { ui_toast(T(L"No se pudo descargar la extensión")); return; }
            Json m = read_manifest(dest);
            bool old = m[L"manifest_version"].kind == Json::NUM && m[L"manifest_version"].n < 3;
            with_profile([dest, old](ICoreWebView2Profile *pr) {
                ComPtr<ICoreWebView2Profile> keep = pr;
                known_load();
                add_folders(pr, { dest }, [keep, old] {
                    refresh_list(keep.Get(), [old] {
                        ui_layout();
                        ui_toast(old ? T(L"Añadida, pero es de las antiguas (MV2): puede que no funcione") : T(L"Extensión añadida"));
                    });
                });
            });
        });
    }).detach();
}
