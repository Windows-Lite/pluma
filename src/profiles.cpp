// Pluma: profiles, as Edge's. Each one has its own window (a Pluma.exe of its own, --perfil=<id>, started by the
// default one so the Task Manager groups them), its own WebView2 profile on the same engine (cookies, cache,
// extensions, passwords) and its own Pluma files (history, bookmarks, session, the new tab page) in g_data. The
// settings (theme, font, language...) are shared. The default profile is the Pluma there was before profiles:
// "Personal", with the same files and the engine's "Default" profile, so nothing of what was there moves.
//
// perfiles.tsv (in g_state, shared): id, name, colour (rrggbb) per line; "default" is the default one. Ids are never
// reused: a removed profile's engine folder only goes once the engine closes.
#include "pluma.h"
#include "json.h"
#include <ctime>
#include <thread>

static const std::vector<unsigned> kColors = { 0x2f6fed, 0x0f9d58, 0xe8710a, 0xd93025, 0x9334e6, 0x00897b, 0xc2185b, 0x5f6368 };
const std::vector<unsigned> &profile_colors() { return kColors; }
static std::wstring list_file() { return g_state + L"\\perfiles.tsv"; }
static std::wstring last_file() { return g_state + L"\\perfil-ultimo.txt"; }
static const UINT WM_APP_SHOW = WM_APP + 1, WM_APP_QUIT = WM_APP + 2, WM_APP_FRONT = WM_APP + 4;   // (FRONT: its window to the front; SHOW opens one more if one is open)
static const ULONG_PTR kCopyProfile = 0x504C56, kCopyOpenHere = 0x504C57;   // (main.cpp)

bool profile_valid_id(const std::wstring &id) {
    // it becomes a folder name and the engine's profile name: a few lowercase letters and digits, nothing else
    if (id.empty() || id.size() > 16 || id == L"default") return false;
    for (wchar_t c : id) if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) return false;
    return true;
}
std::wstring profile_data_dir(const std::wstring &id) { return id.empty() ? g_state : g_state + L"\\perfiles\\" + id; }
std::wstring wv_profile_name() { return g_profile_id.empty() ? std::wstring(L"Default") : L"pluma-" + g_profile_id; }
// (the engine keeps a named profile in "WV2Profile_<name>", the default one in "Default": seen on WebView2 155)
std::wstring wv_profile_dir_of(const std::wstring &id) { return g_state + L"\\webview2\\EBWebView\\" + (id.empty() ? std::wstring(L"Default") : L"WV2Profile_pluma-" + id); }
std::wstring wv_profile_dir() { return wv_profile_dir_of(g_profile_id); }

// ---------------------------------------------------------------- the list (cached; read again when it changes)
static std::vector<Profile> g_list;
static bool g_list_read;
static std::vector<Profile> read_list() {
    std::vector<Profile> out;
    Profile def; def.name = L"Personal"; def.color = kColors[0];
    out.push_back(def);
    std::string d;
    if (!read_file(list_file(), d)) return out;
    std::wstring w = wide(d);
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> f;
        size_t a = 0;
        while (true) { size_t t = line.find(L'\t', a); f.push_back(line.substr(a, t == std::wstring::npos ? std::wstring::npos : t - a)); if (t == std::wstring::npos) break; a = t + 1; }
        if (f.size() < 3) continue;
        Profile p; p.id = f[0]; p.name = trim(f[1]); p.color = wcstoul(f[2].c_str(), NULL, 16) & 0xffffff;
        if (p.id == L"default") { if (!p.name.empty()) out[0].name = p.name; out[0].color = p.color; continue; }
        if (!profile_valid_id(p.id) || p.name.empty()) continue;
        bool dup = false;
        for (auto &x : out) if (x.id == p.id) dup = true;
        if (!dup) out.push_back(p);
    }
    return out;
}
static void save_list(const std::vector<Profile> &v) {
    std::wstring o;
    for (auto &p : v) {
        std::wstring name = p.name;
        for (auto &c : name) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        wchar_t col[8]; swprintf_s(col, L"%06x", p.color & 0xffffff);
        o += (p.id.empty() ? std::wstring(L"default") : p.id) + L"\t" + name + L"\t" + col + L"\n";
    }
    write_file(list_file(), utf8(o));
    g_list = v; g_list_read = true;
}
std::vector<Profile> profiles_list() {
    if (!g_list_read) { g_list = read_list(); g_list_read = true; }
    return g_list;
}
Profile profile_current() {
    for (auto &p : profiles_list()) if (p.id == g_profile_id) return p;
    Profile p; p.id = g_profile_id; p.name = g_profile_id; p.color = kColors[0];
    return p;
}
std::wstring profile_shown_name(const Profile &p) {
    if (p.id.empty() && p.name == L"Personal") return T(L"Personal");
    return p.name;
}

// ---------------------------------------------------------------- the windows of the profiles
static std::wstring base_class() {
    wchar_t inst[64] = L"";
    std::wstring c = L"PlumaVentana";
    if (GetEnvironmentVariableW(L"PLUMA_INSTANCIA", inst, 64) > 0) c += std::wstring(L".") + inst;
    return c;
}
static std::wstring class_of(const std::wstring &id) { return base_class() + (id.empty() ? L"" : L".perfil." + id); }
HWND profile_window(const std::wstring &id) {
    HWND w = FindWindowW(class_of(id).c_str(), NULL);
    return w && !GetPropW(w, L"saliendo") ? w : NULL;
}
// every open window of another profile (of this instance)
static std::vector<HWND> other_windows() {
    struct Ctx { std::wstring prefix; std::vector<HWND> out; } ctx;
    ctx.prefix = base_class() + L".perfil.";
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto *c = (Ctx *)lp;
        wchar_t cls[160];
        if (GetClassNameW(h, cls, 160) && starts_with(cls, c->prefix.c_str()) && !win_of_hwnd(h)) c->out.push_back(h);   // (not this one's own windows)
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.out;
}
bool profiles_others_open() { return !other_windows().empty(); }
void profiles_quit_others() { for (HWND h : other_windows()) PostMessageW(h, WM_APP_QUIT, 0, 0); }
static void broadcast_changed() {
    // the others read the list again (names, colours): their bar, title and settings pages
    HWND def = profile_window(L"");
    if (def && !win_of_hwnd(def)) PostMessageW(def, WM_APP + 42, 0, 0);
    for (HWND h : other_windows()) PostMessageW(h, WM_APP + 42, 0, 0);
}
void profiles_changed() {
    g_list_read = false;
    for (auto &w : g_windows) {   // (every window's title says the profile, and its bar shows it)
        WinScope ws(w.get());
        Tab *t = active_tab();
        SetWindowTextW(g_win, (!t || t->title.empty() ? app_name() : t->title + L" - " + app_name()).c_str());
        ui_layout();
    }
    ui_taskbar_badge();
    settings_page_refresh();
}

// ---------------------------------------------------------------- starting a window
bool spawn_pluma(const std::wstring &args, DWORD *pid) {
    // the child is started with the engine options this one has (the engine refuses different ones: tabs.cpp)
    std::wstring lang, eargs;
    engine_options(lang, eargs);
    bool restart = args == L"--oculto" || args.empty();
    // (the default Pluma started again after an update starts the engine as the settings say now)
    SetEnvironmentVariableW(L"PLUMA_MOTOR_IDIOMA", restart || lang.empty() ? NULL : lang.c_str());
    SetEnvironmentVariableW(L"PLUMA_MOTOR_ARGS", restart || eargs.empty() ? NULL : eargs.c_str());
    std::wstring cmd = L"\"" + g_root + L"\\Pluma.exe\"" + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, 0, NULL, g_root.c_str(), &si, &pi)) return false;
    AllowSetForegroundWindow(pi.dwProcessId);
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}
static std::wstring quoted_urls(const std::vector<std::wstring> &urls) {
    std::wstring o;
    for (auto &u : urls) if (u.find(L'"') == std::wstring::npos) o += L" \"" + u + L"\"";
    return o;
}
void profile_launch_child(const std::wstring &id, const std::vector<std::wstring> &urls) {
    spawn_pluma(L"--perfil=" + (id.empty() ? std::wstring(L"default") : id) + quoted_urls(urls));
}
static void send_lines(HWND w, ULONG_PTR kind, const std::vector<std::wstring> &lines) {
    std::wstring all;
    for (auto &u : lines) all += u + L"\n";
    COPYDATASTRUCT cds = { kind, (DWORD)(all.size() * sizeof(wchar_t)), (PVOID)all.c_str() };
    DWORD_PTR res = 0;
    SendMessageTimeoutW(w, WM_COPYDATA, (WPARAM)g_win, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &res);
}
void profile_open(const std::wstring &id, const std::vector<std::wstring> &urls) {
    HWND w = id == g_profile_id && !g_incognito ? g_win : profile_window(id);
    if (w) {
        // open already: to the front (with the urls)
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        AllowSetForegroundWindow(pid);
        if (!urls.empty()) send_lines(w, kCopyOpenHere, urls);
        else PostMessageW(w, WM_APP_FRONT, 0, 0);
        return;
    }
    if (g_profile_id.empty() && !g_incognito) { profile_launch_child(id, urls); return; }
    // from another profile's window: the default one starts it, so every window hangs from it in the Task Manager
    HWND def = profile_window(L"");
    if (def) {
        DWORD pid = 0; GetWindowThreadProcessId(def, &pid);
        AllowSetForegroundWindow(pid);
        std::vector<std::wstring> lines = { id.empty() ? std::wstring(L"default") : id };
        lines.insert(lines.end(), urls.begin(), urls.end());
        send_lines(def, kCopyProfile, lines);
        return;
    }
    profile_launch_child(id, urls);
}

// ---------------------------------------------------------------- the one used last (links from other apps go there)
void profile_touch() {
    std::wstring me = g_profile_id.empty() ? std::wstring(L"default") : g_profile_id;
    std::string d;
    if (read_file(last_file(), d) && wide(d) == me) return;
    write_file(last_file(), utf8(me));
}
std::wstring profile_last_used() {
    std::string d;
    if (!read_file(last_file(), d)) return L"";
    std::wstring id = trim(wide(d));
    if (!profile_valid_id(id)) return L"";
    for (auto &p : profiles_list()) if (p.id == id) return id;
    return L"";
}

// ---------------------------------------------------------------- making, changing and removing
std::wstring profile_create(const std::wstring &name) {
    auto v = read_list();
    std::wstring id;
    for (int k = 0; k < 50 && id.empty(); k++) {
        wchar_t b[24]; swprintf_s(b, L"p%llx", ((unsigned long long)time(NULL) << 8) ^ (GetTickCount64() & 0xffff) ^ ((unsigned long long)k << 4));
        std::wstring cand = std::wstring(b).substr(0, 14);
        bool used = PathFileExistsW(profile_data_dir(cand).c_str());
        for (auto &p : v) if (p.id == cand) used = true;
        if (!used && profile_valid_id(cand)) id = cand;
        else Sleep(1);
    }
    if (id.empty()) return L"";
    // the first colour no one has yet
    unsigned color = kColors[v.size() % kColors.size()];
    for (unsigned c : kColors) { bool taken = false; for (auto &p : v) if (p.color == c) taken = true; if (!taken) { color = c; break; } }
    Profile p; p.id = id; p.name = name.substr(0, 60); p.color = color;
    v.push_back(p);
    save_list(v);
    SHCreateDirectoryExW(NULL, profile_data_dir(id).c_str(), NULL);
    log_line(L"perfil nuevo: " + id);
    broadcast_changed();
    profiles_changed();
    return id;
}
void profile_rename(const std::wstring &id, const std::wstring &name) {
    std::wstring n = trim(name).substr(0, 60);
    if (n.empty()) return;
    auto v = read_list();
    for (auto &p : v) if (p.id == id) p.name = n;
    save_list(v);
    broadcast_changed();
    profiles_changed();
}
void profile_set_color(const std::wstring &id, unsigned color) {
    auto v = read_list();
    for (auto &p : v) if (p.id == id) p.color = color & 0xffffff;
    save_list(v);
    broadcast_changed();
    profiles_changed();
}
static void remove_tree(const std::wstring &dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());   // (a link: never what it points to)
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
static void delete_engine_profile(const std::wstring &id) {
    // the engine's own data of that profile (cookies, cache, passwords, extensions): a view of it, never shown,
    // marks it for deletion (WebView2 removes the folder when the engine closes)
    if (!g_env) return;
    ComPtr<ICoreWebView2Environment10> e10;
    ComPtr<ICoreWebView2ControllerOptions> o;
    if (FAILED(g_env.As(&e10)) || FAILED(e10->CreateCoreWebView2ControllerOptions(&o))) return;
    std::wstring name = L"pluma-" + id;
    o->put_ProfileName(name.c_str());
    e10->CreateCoreWebView2ControllerWithOptions(g_win, o.Get(), Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [id](HRESULT hr, ICoreWebView2Controller *c) -> HRESULT {
            if (FAILED(hr) || !c) { log_line(L"perfil " + id + L": no se pudo abrir su perfil del motor para borrarlo"); return S_OK; }
            c->put_IsVisible(FALSE);
            RECT none = { 0, 0, 0, 0 }; c->put_Bounds(none);
            ComPtr<ICoreWebView2> v; ComPtr<ICoreWebView2_13> v13; ComPtr<ICoreWebView2Profile> pr; ComPtr<ICoreWebView2Profile8> p8;
            bool ok = SUCCEEDED(c->get_CoreWebView2(&v)) && SUCCEEDED(v.As(&v13)) && SUCCEEDED(v13->get_Profile(&pr)) &&
                      SUCCEEDED(pr.As(&p8)) && SUCCEEDED(p8->Delete());
            log_line(L"perfil " + id + (ok ? L": su perfil del motor, marcado para borrar" : L": este motor no sabe borrar perfiles"));
            if (!ok) c->Close();   // (Delete closes its views itself)
            return S_OK;
        }).Get());
}
void profile_remove(const std::wstring &id, std::function<void(bool)> done) {
    if (!profile_valid_id(id) || id == g_profile_id) { done(false); return; }
    // off the list first: nothing can open it again meanwhile
    auto v = read_list();
    size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Profile &p) { return p.id == id; }), v.end());
    if (v.size() == before) { done(false); return; }
    save_list(v);
    if (profile_last_used() == id || !PathFileExistsW(last_file().c_str())) write_file(last_file(), utf8(L"default"));
    HWND w = profile_window(id);
    if (w) PostMessageW(w, WM_APP_QUIT, 0, 0);
    // once its window is gone (up to 10 s): its engine profile and its files
    struct Wait { std::wstring id; std::function<void(bool)> done; int left = 100; };
    auto *wt = new Wait{ id, done };
    std::function<void()> *step = new std::function<void()>();
    *step = [wt, step] {
        HWND still = FindWindowW(class_of(wt->id).c_str(), NULL);
        if (still && --wt->left > 0) {
            std::thread([step] { Sleep(100); ui_post([step] { (*step)(); }); }).detach();
            return;
        }
        delete_engine_profile(wt->id);
        std::wstring dir = profile_data_dir(wt->id);
        if (profile_valid_id(wt->id) && starts_with(dir, (g_state + L"\\perfiles\\").c_str())) remove_tree(dir);
        log_line(L"perfil quitado: " + wt->id);
        broadcast_changed();
        profiles_changed();
        auto d = wt->done;
        delete wt; delete step;
        d(true);
    };
    (*step)();
}

// ---------------------------------------------------------------- the settings page's section
static std::wstring color_hex(unsigned c) { wchar_t b[8]; swprintf_s(b, L"#%06x", c & 0xffffff); return b; }
std::wstring profiles_state_json() {
    std::wstring o = L"{\"current\":" + json_escape(g_profile_id) + L",\"list\":[";
    bool first = true;
    for (auto &p : profiles_list()) {
        bool open = p.id == g_profile_id || profile_window(p.id) != NULL;
        o += (first ? L"" : L",") + std::wstring(L"{\"id\":") + json_escape(p.id) + L",\"name\":" + json_escape(profile_shown_name(p)) +
             L",\"color\":" + json_escape(color_hex(p.color)) + L",\"open\":" + (open ? L"true" : L"false") + L"}";
        first = false;
    }
    o += L"],\"colors\":[";
    first = true;
    for (unsigned c : kColors) { o += (first ? L"" : L",") + json_escape(color_hex(c)); first = false; }
    return o + L"]}";
}
void profiles_page_do(const Json &j) {
    std::wstring what = j[L"what"].s, id = j[L"id"].s;
    if (!id.empty() && !profile_valid_id(id)) return;
    if (what == L"new") {
        std::wstring nid = profile_create(T(L"Perfil") + std::wstring(L" ") + std::to_wstring(profiles_list().size() + 1));
        if (!nid.empty()) profile_open(nid);
    } else if (what == L"open") profile_open(id);
    else if (what == L"rename") profile_rename(id, j[L"name"].s);
    else if (what == L"color") {
        std::wstring c = j[L"color"].s;
        if (c.size() == 7 && c[0] == L'#') profile_set_color(id, wcstoul(c.c_str() + 1, NULL, 16));
    } else if (what == L"remove") {
        profile_remove(id, [](bool ok) { if (!ok) ui_toast(T(L"No se pudo quitar el perfil")); settings_page_refresh(); });
    }
    settings_page_refresh();
}
