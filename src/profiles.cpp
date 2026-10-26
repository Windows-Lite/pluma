// Pluma: profiles, as Edge's. Each one has its own window (a Pluma.exe of its own, --perfil=<id>, started by the
// default one so the Task Manager groups them), its own WebView2 profile on the same engine (cookies, cache,
// extensions, passwords) and its own Pluma files (history, bookmarks, session, the new tab page) in g_data. The
// settings (theme, font, language...) are shared. The default profile is the Pluma there was before profiles:
// "Personal", with the same files and the engine's "Default" profile, so nothing of what was there moves.
//
// perfiles.tsv (in g_state, shared): id, name, colour (rrggbb), avatar ("", "g<n>", "img") per line; "default" is the
// default one. Ids are never reused: a removed profile's engine folder only goes once the engine closes. A picture is
// avatar.png in the profile's own folder (g_state for the default one), so the backup takes it with the rest.
#include "pluma.h"
#include "json.h"
#include <ctime>
#include <thread>

static const std::vector<unsigned> kColors = { 0x2f6fed, 0x0f9d58, 0xe8710a, 0xd93025, 0x9334e6, 0x00897b, 0xc2185b, 0x5f6368 };
const std::vector<unsigned> &profile_colors() { return kColors; }
// the built-in avatars, as Chrome's grid of icons (drawn by Pluma, white on the profile's colour; in both Segoe Fluent
// Icons and Segoe MDL2 Assets): smile, heart, star, sun, moon, game, music, camera, home, work, school, plane
static const std::vector<wchar_t> kGlyphs = { 0xE76E, 0xEB51, 0xE734, 0xE706, 0xE708, 0xE7FC, 0xE8D6, 0xE722, 0xE80F, 0xE821, 0xE7BE, 0xE709 };
const std::vector<wchar_t> &profile_glyphs() { return kGlyphs; }
static bool valid_avatar(const std::wstring &a) {
    if (a.empty() || a == L"img") return true;
    if (a.size() < 2 || a.size() > 3 || a[0] != L'g') return false;
    for (size_t i = 1; i < a.size(); i++) if (a[i] < L'0' || a[i] > L'9') return false;
    return _wtoi(a.c_str() + 1) < (int)kGlyphs.size();
}
int profile_glyph_of(const Profile &p) { return p.avatar.size() >= 2 && p.avatar[0] == L'g' && valid_avatar(p.avatar) ? _wtoi(p.avatar.c_str() + 1) : -1; }
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
std::wstring profile_picture_file(const std::wstring &id) { return profile_data_dir(id) + L"\\avatar.png"; }
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
        if (f.size() > 3 && valid_avatar(f[3])) p.avatar = f[3];
        if (p.id == L"default") { if (!p.name.empty()) out[0].name = p.name; out[0].color = p.color; out[0].avatar = p.avatar; continue; }
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
        o += (p.id.empty() ? std::wstring(L"default") : p.id) + L"\t" + name + L"\t" + col + L"\t" + p.avatar + L"\n";
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
    ui_avatars_changed();
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

// ---------------------------------------------------------------- the avatar (as Chrome's and Edge's: a picture, a built-in one, or the initial)
void profile_set_avatar(const std::wstring &id, const std::wstring &avatar) {
    if (!valid_avatar(avatar)) return;
    auto v = read_list();
    bool found = false;
    for (auto &p : v) if (p.id == id) { p.avatar = avatar; found = true; }
    if (!found) return;
    if (avatar != L"img") DeleteFileW(profile_picture_file(id).c_str());
    save_list(v);
    broadcast_changed();
    profiles_changed();
}
static WICBitmapTransformOptions exif_turn(IWICBitmapFrameDecode *fr) {
    // a phone's photo says how it was held (EXIF orientation); the mirrored ones (5, 7) are left as they come
    ComPtr<IWICMetadataQueryReader> q;
    PROPVARIANT v; PropVariantInit(&v);
    WICBitmapTransformOptions t = WICBitmapTransformRotate0;
    if (SUCCEEDED(fr->GetMetadataQueryReader(&q)) && SUCCEEDED(q->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2) {
        switch (v.uiVal) {
            case 2: t = WICBitmapTransformFlipHorizontal; break;
            case 3: t = WICBitmapTransformRotate180; break;
            case 4: t = WICBitmapTransformFlipVertical; break;
            case 6: t = WICBitmapTransformRotate90; break;
            case 8: t = WICBitmapTransformRotate270; break;
        }
    }
    PropVariantClear(&v);
    return t;
}
bool profile_picture_import(const std::wstring &src, const std::wstring &dst, std::wstring *why) {
    // the file the person chose, decoded here with WIC (never by a web page): its first frame, turned as it was
    // taken, the centred square, 128 px, written again as PNG. Anything odd (not an image, cut short, huge) writes nothing.
    auto fail = [&](const wchar_t *w) { if (why) *why = w; return false; };
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return fail(L"no existe");
    if (fa.nFileSizeHigh || fa.nFileSizeLow > 40u * 1024 * 1024) return fail(L"más de 40 MB");
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return fail(L"sin WIC");
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(wic->CreateDecoderFromFilename(src.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return fail(L"no es una imagen");
    GUID container = {};
    if (SUCCEEDED(dec->GetContainerFormat(&container)) && IsEqualGUID(container, GUID_ContainerFormatPng)) {
        // WIC fills in a PNG cut short without a word (seen): its end (IEND) must be there
        static const unsigned char iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
        unsigned char tail[12] = {};
        DWORD got = 0;
        HANDLE fh = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        bool whole = fh != INVALID_HANDLE_VALUE && SetFilePointer(fh, -12, NULL, FILE_END) != INVALID_SET_FILE_POINTER &&
                     ReadFile(fh, tail, 12, &got, NULL) && got == 12 && !memcmp(tail, iend, 12);
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        if (!whole) return fail(L"cortada");
    }
    if (FAILED(dec->GetFrame(0, &fr))) return fail(L"sin fotogramas");
    UINT w = 0, h = 0;
    if (FAILED(fr->GetSize(&w, &h)) || !w || !h) return fail(L"sin tamaño");
    if (w > 16384 || h > 16384 || (unsigned long long)w * h > 100000000ull) return fail(L"demasiado grande");
    const UINT n = 128, side = std::min(w, h);
    WICRect crop = { (INT)((w - side) / 2), (INT)((h - side) / 2), (INT)side, (INT)side };
    ComPtr<IWICBitmapClipper> clip;
    ComPtr<IWICBitmapScaler> sc;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(fr.Get(), &crop)) ||
        FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(clip.Get(), n, n, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(sc.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeMedianCut)))
        return fail(L"no se pudo convertir");
    IWICBitmapSource *last = conv.Get();
    ComPtr<IWICBitmapFlipRotator> rot;
    WICBitmapTransformOptions turn = exif_turn(fr.Get());
    if (turn != WICBitmapTransformRotate0 && SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(conv.Get(), turn))) last = rot.Get();
    // all of it decoded now: a file cut short fails here, before anything is written
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromSource(last, WICBitmapCacheOnLoad, &bmp))) return fail(L"dañada");
    std::wstring tmp = dst + L".tmp";
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fe;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) && SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fe, NULL)) && SUCCEEDED(fe->Initialize(NULL)) && SUCCEEDED(fe->SetSize(n, n)) &&
              SUCCEEDED(fe->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
              SUCCEEDED(fe->WriteSource(bmp.Get(), NULL)) && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
    fe.Reset(); enc.Reset(); st.Reset();   // (the file closed before it moves)
    for (int k = 0; ok && k < 20; k++) {   // (another window may be reading the old one for a moment)
        if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return true; }
        Sleep(50);
    }
    DeleteFileW(tmp.c_str());
    return fail(L"no se pudo guardar");
}
void profile_set_picture(const std::wstring &id, const std::wstring &file) {
    // decoding a big photo takes a while: off the window's thread, then the list says "img"
    SHCreateDirectoryExW(NULL, profile_data_dir(id).c_str(), NULL);
    std::wstring dst = profile_picture_file(id);
    std::thread([id, file, dst] {
        HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        std::wstring why;
        bool ok = profile_picture_import(file, dst, &why);
        if (SUCCEEDED(co)) CoUninitialize();
        ui_post([id, ok, why] {
            log_line(L"perfil " + (id.empty() ? std::wstring(L"default") : id) + (ok ? L": imagen puesta" : L": imagen no válida (" + why + L")"));
            if (!ok) { ui_toast(T(L"No se pudo usar esa imagen")); settings_page_refresh(); return; }
            profile_set_avatar(id, L"img");   // (every window draws it again, also when it already was "img")
        });
    }).detach();
}
static void pick_picture(const std::wstring &id) {
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = { { T(L"Imágenes"), L"*.png;*.jpg;*.jpeg;*.webp;*.gif;*.bmp" } };
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(T(L"Elegir imagen"));
    IShellItem *pics = NULL;   // (starts in Pictures, unless the picker remembers another folder)
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Pictures, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&pics)))) { dlg->SetDefaultFolder(pics); pics->Release(); }
    DWORD opt = 0; dlg->GetOptions(&opt); dlg->SetOptions(opt | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    std::wstring file;
    if (SUCCEEDED(dlg->Show(g_win))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR path = NULL;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) { file = path; CoTaskMemFree(path); }
            it->Release();
        }
    }
    dlg->Release();
    if (!file.empty()) profile_set_picture(id, file);
}
void profiles_test(const std::wstring &what) {
    // tests (pluma-prueba:, test instances only): avatar-imagen=<id|default>|<file>, avatar-icono=<id|default>,<n>,
    // avatar-quitar=<id|default>, insignia=<name> (the taskbar badge into <name>.png), elegir-imagen=<id|default> (the picker, shown)
    auto id_of = [](std::wstring s) { return s == L"default" ? std::wstring() : s; };
    if (starts_with(what, L"avatar-imagen=")) {
        std::wstring r = what.substr(14);
        size_t bar = r.find(L'|');
        if (bar == std::wstring::npos) return;
        std::wstring id = id_of(r.substr(0, bar));
        if (id.empty() || profile_valid_id(id)) profile_set_picture(id, r.substr(bar + 1));
    } else if (starts_with(what, L"avatar-icono=")) {
        std::wstring r = what.substr(13);
        size_t c = r.find(L',');
        if (c == std::wstring::npos) return;
        std::wstring id = id_of(r.substr(0, c));
        if (id.empty() || profile_valid_id(id)) profile_set_avatar(id, L"g" + std::to_wstring(_wtoi(r.c_str() + c + 1)));
    } else if (starts_with(what, L"avatar-quitar=")) {
        std::wstring id = id_of(what.substr(14));
        if (id.empty() || profile_valid_id(id)) profile_set_avatar(id, L"");
    } else if (starts_with(what, L"elegir-imagen=")) {
        std::wstring id = id_of(what.substr(14));
        if (id.empty() || profile_valid_id(id)) pick_picture(id);
    } else if (starts_with(what, L"insignia=")) {
        std::wstring name = what.substr(9);
        bool safe = !name.empty() && name.size() < 40;
        for (wchar_t c : name) if (!iswalnum(c) && c != L'-') safe = false;
        if (safe) ui_taskbar_badge_png(g_state + L"\\" + name + L".png");
    }
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
static std::wstring png_data_url(const std::string &png) {
    static const char *k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring o = L"data:image/png;base64,";
    o.reserve(o.size() + png.size() * 4 / 3 + 4);
    for (size_t i = 0; i < png.size(); i += 3) {
        unsigned v = (unsigned char)png[i] << 16 | (i + 1 < png.size() ? (unsigned char)png[i + 1] << 8 : 0) | (i + 2 < png.size() ? (unsigned char)png[i + 2] : 0);
        o += k[v >> 18 & 63]; o += k[v >> 12 & 63];
        o += i + 1 < png.size() ? k[v >> 6 & 63] : '=';
        o += i + 2 < png.size() ? k[v & 63] : '=';
    }
    return o;
}
std::wstring profiles_state_json() {
    std::wstring o = L"{\"current\":" + json_escape(g_profile_id) + L",\"list\":[";
    bool first = true;
    for (auto &p : profiles_list()) {
        bool open = p.id == g_profile_id || profile_window(p.id) != NULL;
        o += (first ? L"" : L",") + std::wstring(L"{\"id\":") + json_escape(p.id) + L",\"name\":" + json_escape(profile_shown_name(p)) +
             L",\"color\":" + json_escape(color_hex(p.color)) + L",\"open\":" + (open ? L"true" : L"false") +
             L",\"avatar\":" + json_escape(p.avatar);
        // its picture goes to the page over this same message (never at a pluma.local address: any web could load it)
        std::string png;
        if (p.avatar == L"img" && read_file(profile_picture_file(p.id), png) && png.size() < 512 * 1024) o += L",\"picture\":" + json_escape(png_data_url(png));
        o += L"}";
        first = false;
    }
    o += L"],\"colors\":[";
    first = true;
    for (unsigned c : kColors) { o += (first ? L"" : L",") + json_escape(color_hex(c)); first = false; }
    o += L"],\"glyphs\":[";
    for (size_t i = 0; i < kGlyphs.size(); i++) o += (i ? L"," : L"") + json_escape(png_data_url(ui_avatar_glyph_png((int)i)));
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
    } else if (what == L"avatarFile") pick_picture(id);
    else if (what == L"avatarGlyph") {
        int n = (int)j[L"n"].n;
        if (n >= 0 && n < (int)kGlyphs.size()) profile_set_avatar(id, L"g" + std::to_wstring(n));
    } else if (what == L"avatarClear") profile_set_avatar(id, L"");
    else if (what == L"remove") {
        profile_remove(id, [](bool ok) { if (!ok) ui_toast(T(L"No se pudo quitar el perfil")); settings_page_refresh(); });
    }
    settings_page_refresh();
}
