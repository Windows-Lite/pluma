// Pluma: downloads, handled by Pluma itself.
// The engine's own downloads flyout crashed the engine when its "Open file" was pressed (26/10, WebView2 155 with Edge
// uninstalled: an access violation in the browser process on freed memory, the engine gone with every tab), and
// WebView2 gives no way into that button. So every download is marked Handled in DownloadStarting (the flyout never
// shows) and Pluma keeps the list: the engine still names the file, saves it and checks it (SmartScreen). Its panel is
// a page of the menu (ui.cpp, PAGE_DOWNLOADS). Opening a finished file is Pluma's: what a tab shows (PDF, pages,
// pictures, text) opens in a tab; anything else goes to Windows (ShellExecute). The list lives in memory only.
// Except the types the engine holds for a "keep it?" confirmation (programs, scripts...): WebView2 has no way to give
// that answer, so a handled one stayed at 100 % for ever (26/10, .exe and .msi). Those still get the engine's flyout,
// with its Keep and Delete (and its own Open, as before); Pluma's list follows them too, without popping up.
#include "pluma.h"

struct Download {
    int id = 0;
    ComPtr<ICoreWebView2DownloadOperation> op;
    std::wstring path, url;
    int state = 0;                 // 0 running, 1 done, 2 failed or cancelled
    INT64 got = 0, total = -1;
    COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
    bool paused = false;
    bool engine = false;           // left to the engine's flyout (a type it asks about)
    int tab = 0;                   // the tab it came from: its panel shows in that tab's window (wherever it is by then)
    EventRegistrationToken t_state{}, t_bytes{};
};
static std::vector<Download> g_dl;   // newest last
static int g_dl_next = 1;
static bool g_dl_dirty;
static const size_t kMaxShown = 30;

static Download *dl_find(int id) { for (auto &d : g_dl) if (d.id == id) return &d; return nullptr; }
static std::wstring file_name(const std::wstring &p) { size_t s = p.find_last_of(L"\\/"); return s == std::wstring::npos ? p : p.substr(s + 1); }
static std::wstring ext_of(const std::wstring &p) {
    std::wstring n = file_name(p);
    size_t d = n.rfind(L'.');
    return d == std::wstring::npos ? L"" : lower(n.substr(d + 1));
}
static std::wstring size_text(INT64 n) {
    wchar_t b[64] = L"";
    StrFormatByteSizeEx((ULONGLONG)std::max<INT64>(0, n), SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, b, 64);
    return b;
}
static void changed_soon() {
    // the panel follows (at most 4 times a second: bytes arrive far more often)
    if (g_dl_dirty) return;
    g_dl_dirty = true;
    SetTimer(g_app, 0xD0A1, 250, [](HWND h, UINT, UINT_PTR id, DWORD) { KillTimer(h, id); g_dl_dirty = false; ui_downloads_changed(); });
}
static Window *window_of(const Download &d) {
    // the window of the tab it came from (it may have moved to another one since), or the one used last
    Tab *t = d.tab ? tab_by_id(d.tab) : nullptr;
    Window *w = t ? win_of_tab(t) : nullptr;
    return w ? w : win_last_used();
}
static void read_state(Download &d) {
    if (!d.op) return;
    COREWEBVIEW2_DOWNLOAD_STATE s = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    d.op->get_State(&s);
    d.op->get_BytesReceived(&d.got);
    d.op->get_TotalBytesToReceive(&d.total);
    BOOL can_resume = FALSE;
    d.op->get_CanResume(&can_resume);
    LPWSTR p = nullptr;
    if (SUCCEEDED(d.op->get_ResultFilePath(&p)) && p) { if (*p) d.path = p; CoTaskMemFree(p); }
    int was = d.state;
    if (s == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) d.state = 1;
    else if (s == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
        d.op->get_InterruptReason(&d.reason);
        // paused (by the engine or a page): still on its way for the list
        d.paused = can_resume && d.reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED;
        d.state = d.paused ? 0 : 2;
    } else d.state = 0;
    if (d.state != was) {
        if (g_incognito) log_line(L"descarga " + std::to_wstring(d.id) + L": estado " + std::to_wstring(d.state));
        else log_line(L"descarga " + std::to_wstring(d.id) + L": estado " + std::to_wstring(d.state) + L" (" + std::to_wstring((int)d.reason) +
                      L"), " + std::to_wstring(d.got) + L" de " + std::to_wstring(d.total) + L" bytes, " + file_name(d.path));
        if (d.state != 0) {
            d.op->remove_StateChanged(d.t_state);
            d.op->remove_BytesReceivedChanged(d.t_bytes);
            d.op.Reset();
            if (d.state == 1 && !d.engine) { WinScope ws(window_of(d)); ui_open_downloads(true); }   // as Edge: the list again when it is done
        }
    }
}
static void CALLBACK poll_tick(HWND h, UINT, UINT_PTR id, DWORD) {
    // a backstop for StateChanged, which some runtimes do not raise (WebView2Feedback #5638): the running ones, read
    bool any = false;
    for (auto &d : g_dl) if (d.state == 0 && d.op) { read_state(d); any |= d.state == 0; }
    changed_soon();
    if (!any) KillTimer(h, id);
}
static bool engine_asks(const std::wstring &path) {
    // Windows types the engine holds until the person says "keep" (Chromium's download_file_types.asciipb, danger level
    // DANGEROUS or ALLOW_ON_USER_GESTURE on Windows, 26/10/2026)
    static const wchar_t *kAsk[] = { L"accda", L"accdb", L"accde", L"accdr", L"ad", L"ade", L"adp", L"app", L"application",
        L"appref-ms", L"appx", L"appxbundle", L"asp", L"asx", L"bas", L"bat", L"bgi", L"cer", L"cfg", L"chi", L"chm", L"class",
        L"cmd", L"com", L"cpi", L"cpl", L"crt", L"crx", L"der", L"diagcab", L"diagcfg", L"diagpkg", L"dll", L"drv", L"efi",
        L"eml", L"exe", L"fon", L"fxp", L"gadget", L"grp", L"hlp", L"hta", L"htt", L"img", L"inf", L"ini", L"ins", L"inx",
        L"iqy", L"isp", L"isu", L"jar", L"jnlp", L"job", L"js", L"jse", L"library-ms", L"lnk", L"local", L"mad", L"maf",
        L"mag", L"mam", L"manifest", L"maq", L"mar", L"mas", L"mat", L"mau", L"mav", L"maw", L"mda", L"mdb", L"mde", L"mdt",
        L"mdw", L"mdz", L"mht", L"mhtml", L"mmc", L"mof", L"msc", L"msg", L"msh", L"msh1", L"msh1xml", L"msh2", L"msh2xml",
        L"mshxml", L"msi", L"msix", L"msixbundle", L"msp", L"mst", L"ocx", L"ops", L"oxt", L"paf", L"partial", L"pcd", L"pif",
        L"pl", L"plg", L"prf", L"prg", L"ps1", L"ps1xml", L"ps2", L"ps2xml", L"psc1", L"psc2", L"psm1", L"pst", L"py", L"pyc",
        L"pyd", L"pyo", L"pyw", L"pyz", L"pyzw", L"rb", L"reg", L"rels", L"rgs", L"scf", L"scr", L"sct", L"search-ms",
        L"settingcontent-ms", L"shb", L"shs", L"slk", L"spl", L"swf", L"sys", L"u3p", L"url", L"vb", L"vbe", L"vbs",
        L"vbscript", L"vdx", L"vsd", L"vsdm", L"vsdx", L"vsmacros", L"vss", L"vssm", L"vssx", L"vst", L"vstm", L"vstx", L"vsw",
        L"vsx", L"vtx", L"website", L"ws", L"wsc", L"wsf", L"wsh", L"xbap", L"xll", L"xml", L"xnk", L"xrm-ms", L"xsd", L"xsl" };
    std::wstring e = ext_of(path);
    for (auto k : kAsk) if (e == k) return true;
    return false;
}
static void on_starting(ICoreWebView2 *sender, ICoreWebView2DownloadStartingEventArgs *a) {
    ComPtr<ICoreWebView2DownloadOperation> op;
    if (FAILED(a->get_DownloadOperation(&op)) || !op) return;
    Download d;
    d.id = g_dl_next++;
    d.op = op;
    LPWSTR p = nullptr;
    if (SUCCEEDED(a->get_ResultFilePath(&p)) && p) { d.path = p; CoTaskMemFree(p); }
    // the engine's flyout does not show: its "Open file" took the engine down (see above). It still names and saves
    // the file. A type it asks about keeps its flyout (only there can the person keep it).
    d.engine = engine_asks(d.path);
    for (Tab *t : all_tabs()) if (t->view && t->view.Get() == sender) d.tab = t->id;
    if (!d.engine) a->put_Handled(TRUE);
    if (SUCCEEDED(op->get_Uri(&p)) && p) { d.url = p; CoTaskMemFree(p); }
    op->get_TotalBytesToReceive(&d.total);
    int id = d.id;
    op->add_StateChanged(Callback<ICoreWebView2StateChangedEventHandler>([id](ICoreWebView2DownloadOperation *, IUnknown *) -> HRESULT {
        if (Download *x = dl_find(id)) { read_state(*x); changed_soon(); }
        return S_OK;
    }).Get(), &d.t_state);
    op->add_BytesReceivedChanged(Callback<ICoreWebView2BytesReceivedChangedEventHandler>([id](ICoreWebView2DownloadOperation *, IUnknown *) -> HRESULT {
        if (Download *x = dl_find(id)) { if (x->op) x->op->get_BytesReceived(&x->got); changed_soon(); }
        return S_OK;
    }).Get(), &d.t_bytes);
    if (!g_incognito) log_line(L"descarga " + std::to_wstring(id) + L": empieza " + file_name(d.path) + (d.engine ? L" (con el panel del motor)" : L""));
    g_dl.push_back(std::move(d));
    while (g_dl.size() > kMaxShown) {
        auto it = std::find_if(g_dl.begin(), g_dl.end(), [](const Download &x) { return x.state != 0; });
        if (it == g_dl.end()) break;
        g_dl.erase(it);
    }
    SetTimer(g_app, 0xD0A2, 500, poll_tick);
    if (!g_dl.back().engine) { WinScope ws(window_of(g_dl.back())); ui_open_downloads(true); }   // (the engine's flyout is out already)
}
void downloads_watch(ICoreWebView2 *v) {
    ComPtr<ICoreWebView2_4> v4;
    if (!v || FAILED(v->QueryInterface(IID_PPV_ARGS(&v4)))) return;
    EventRegistrationToken tok;
    v4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
        [](ICoreWebView2 *s, ICoreWebView2DownloadStartingEventArgs *a) -> HRESULT { on_starting(s, a); return S_OK; }).Get(), &tok);
}

std::vector<DownloadRow> downloads_rows() {
    std::vector<DownloadRow> v;
    for (auto it = g_dl.rbegin(); it != g_dl.rend(); ++it) {
        const Download &d = *it;
        DownloadRow r;
        r.id = d.id; r.state = d.state; r.name = file_name(d.path);
        if (r.name.empty()) r.name = d.url;
        if (d.state == 0) {
            bool known = d.total > 0;
            r.progress = known ? std::min(1.f, (float)((double)d.got / (double)d.total)) : -1.f;
            std::wstring s = known ? std::to_wstring((int)(r.progress * 100)) + L" % · " + size_text(d.got) + L" / " + size_text(d.total)
                                   : size_text(d.got);
            r.status = d.paused ? std::wstring(T(L"En pausa")) + L" · " + s : s;
        } else if (d.state == 1) {
            r.status = size_text(d.got > 0 ? d.got : d.total);
            if (GetFileAttributesW(d.path.c_str()) == INVALID_FILE_ATTRIBUTES) { r.status = T(L"Eliminado"); r.state = 3; }
        } else {
            switch (d.reason) {
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED: r.status = T(L"Cancelada"); break;
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_MALICIOUS:
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED_BY_POLICY:
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_SECURITY_CHECK_FAILED: r.status = T(L"Bloqueada por seguridad"); break;
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE: r.status = T(L"No queda espacio en el disco"); break;
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_FAILED:
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_TIMEOUT:
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED:
            case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_SERVER_DOWN: r.status = T(L"Error de red"); break;
            default: r.status = T(L"No se pudo descargar"); break;
            }
        }
        v.push_back(r);
    }
    return v;
}
bool downloads_any() { return !g_dl.empty(); }

static bool opens_in_tab(const std::wstring &path) {
    // what a tab shows by itself (a file:// address of any other type would only be downloaded again)
    static const wchar_t *kTab[] = { L"pdf", L"htm", L"html", L"xhtml", L"svg", L"png", L"jpg", L"jpeg", L"gif", L"webp",
                                     L"bmp", L"ico", L"avif", L"txt", L"json" };
    std::wstring e = ext_of(path);
    for (auto k : kTab) if (e == k) return true;
    return false;
}
static void shell(const wchar_t *verb, const std::wstring &path) {
    // test instances never open anything of Windows (it would land on the person's screen): only the log says what
    if (g_test) { log_line(std::wstring(L"descarga: ") + verb + L" " + file_name(path) + L" (prueba: no se abre)"); return; }
    SHELLEXECUTEINFOW si = { sizeof(si) };
    si.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    si.hwnd = g_win; si.lpVerb = verb; si.lpFile = path.c_str(); si.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&si)) return;
    DWORD e = GetLastError();
    if (!wcscmp(verb, L"open") && (e == ERROR_NO_ASSOCIATION || e == ERROR_FILE_NOT_FOUND || e == SE_ERR_NOASSOC)) {
        // no program for it (or the one it pointed to is gone): Windows's "Open with"
        si.lpVerb = L"openas"; si.fMask = SEE_MASK_NOASYNC;
        if (ShellExecuteExW(&si)) return;
    }
    log_line(L"descarga: no se pudo abrir (" + std::to_wstring(e) + L")");
    ui_toast(T(L"No se pudo abrir el archivo"));
}
void download_open(int id) {
    Download *d = dl_find(id);
    if (!d || d->state != 1) return;
    if (GetFileAttributesW(d->path.c_str()) == INVALID_FILE_ATTRIBUTES) { ui_toast(T(L"El archivo ya no está")); return; }
    if (opens_in_tab(d->path)) {
        wchar_t buf[4096]; DWORD n = 4096;
        if (SUCCEEDED(UrlCreateFromPathW(d->path.c_str(), buf, &n, 0))) {
            if (!g_incognito) log_line(L"descarga " + std::to_wstring(id) + L": se abre en una pestaña");
            tab_new(buf, true, -1, g_incognito);
            return;
        }
    }
    shell(L"open", d->path);
}
static void show_in_folder(const std::wstring &path) {
    if (g_test) { log_line(L"descarga: mostrar en la carpeta " + file_name(path) + L" (prueba: no se abre)"); return; }
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return;
    }
    // (the file is gone: its folder)
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    ShellExecuteW(g_win, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void download_show(int id) { if (Download *d = dl_find(id)) show_in_folder(d->path); }
void download_cancel(int id) {
    Download *d = dl_find(id);
    if (d && d->state == 0 && d->op) { d->op->Cancel(); read_state(*d); changed_soon(); }
}
void downloads_open_folder() {
    // the engine's folder (Windows's Downloads unless changed)
    with_profile([](ICoreWebView2Profile *p) {
        std::wstring dir;
        LPWSTR s = nullptr;
        if (p && SUCCEEDED(p->get_DefaultDownloadFolderPath(&s)) && s) { dir = s; CoTaskMemFree(s); }
        if (dir.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &s))) { dir = s; CoTaskMemFree(s); }
        if (dir.empty()) return;
        if (g_test) { log_line(L"descarga: abrir la carpeta de descargas (prueba: no se abre)"); return; }
        ShellExecuteW(g_win, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
}
