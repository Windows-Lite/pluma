// Pluma: saved passwords, view and copy (Configuración > Contraseñas, and the key in the address bar), as Edge's and
// Chrome's password managers.
//
// The list (site, username) comes from the engine's Login Data (passwords.cpp reads a copy; nothing is decrypted
// for it). A password is decrypted in Pluma only when the person asks, one at a time, and only after Windows has
// verified that it is them: Windows Hello (face, fingerprint, PIN) through UserConsentVerifier, or, without Hello,
// the account's password through CredUIPromptForWindowsCredentials checked with LsaLogonUser against this account's
// SID (Chromium's chrome/browser/password_manager/password_manager_util_win.cc and device_reauth/win). Once verified,
// that settings page (or the key panel) may reveal and copy for 60 s, as Chromium's password access authenticator.
// The value goes only to the pluma.local settings page that asked (its source checked again when the answer is
// ready), or to the clipboard, marked out of the clipboard history and the cloud clipboard, and cleared after 30 s if
// it is still what is there. No password, username or site is ever logged.
//
// Tests (instances with PLUMA_INSTANCIA only, whose data and engine key are their own, in prueba.<name>):
// PLUMA_PRUEBA_VERIFICAR=si|no|lento answers the verification without Windows (lento: yes, after 2 s);
// PLUMA_PRUEBA_PORTAPAPELES_S=<s> clears the clipboard sooner.
#include "pluma.h"
#include "json.h"
#include <thread>
#include <atomic>
#include <roapi.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.security.credentials.ui.h>
#include <UserConsentVerifierInterop.h>
#include <wincred.h>
#include <ntsecapi.h>
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "credui.lib")
#pragma comment(lib, "secur32.lib")

using namespace ABI::Windows::Security::Credentials::UI;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Foundation::IAsyncInfo;
using Microsoft::WRL::Wrappers::HStringReference;

static const double kGrantMs = 60000;   // reveal and copy without asking again, as Chromium (kAuthValidityPeriod)
static const int kClipS = 30;

// ---------------------------------------------------------------- the list, read off the window's thread
static std::vector<SavedLogin> g_rows;
static int g_from = -2;                 // -2 not read yet, -1 failed, 0 the live file, 1 the start's copy
static bool g_reading;
static std::wstring g_stamp;            // the files as they were when read
static double g_checked;
static std::vector<std::function<void()>> g_waiting;

static std::wstring file_stamp() {
    std::wstring s, ld = wv_profile_dir() + L"\\Login Data";
    for (const std::wstring &f : { ld, ld + L"-wal", g_data + L"\\passwords-delete.tsv" }) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a))
            s += std::to_wstring(a.ftLastWriteTime.dwLowDateTime) + L"." + std::to_wstring(a.nFileSizeLow) + L";";
        else s += L"-;";
    }
    return s;
}
static void redraw_bars() {
    for (auto &w : g_windows) { WinScope ws(w.get()); ui_invalidate(); }
}
static void reload(std::function<void()> then) {
    if (then) g_waiting.push_back(then);
    if (g_reading) return;
    g_reading = true;
    g_stamp = file_stamp();
    std::wstring prof = wv_profile_dir(), data = g_data;   // (now: an import may change g_data for a moment later)
    std::thread([prof, data] {
        auto *rows = new std::vector<SavedLogin>;
        int from = passwords_read_saved(*rows, prof, data);
        ui_post([rows, from] {
            g_rows.swap(*rows);
            delete rows;
            if (from != g_from) log_line(from == 1 ? L"contraseñas: lista del arranque (el motor tiene el fichero)" : from == 0 ? L"contraseñas: lista leída" : L"contraseñas: no se pudo leer la lista");
            g_from = from;
            g_reading = false;
            g_checked = now_ms();
            auto w = std::move(g_waiting);
            g_waiting.clear();
            for (auto &f : w) f();
            redraw_bars();
        });
    }).detach();
}
static void maybe_reload() {
    // at most every 2 s, and only if Login Data (or the removal queue) changed
    if (g_reading || g_incognito) return;
    double now = now_ms();
    if (g_from != -2 && now - g_checked < 2000) return;
    g_checked = now;
    if (g_from == -2 || file_stamp() != g_stamp) reload(nullptr);
}
static const SavedLogin *row_by_id(long long id) {
    for (auto &r : g_rows) if (r.id == id) return &r;
    return nullptr;
}

// the site: scheme, host and port, with "www." the same as without (Chromium also matches those)
static std::wstring site_key(const std::wstring &url) {
    std::wstring u = lower(url);
    size_t s = u.find(L"://");
    if (s == std::wstring::npos) return L"";
    std::wstring scheme = u.substr(0, s);
    if (scheme != L"http" && scheme != L"https") return L"";
    size_t e = u.find_first_of(L"/?#", s + 3);
    std::wstring host = u.substr(s + 3, e == std::wstring::npos ? std::wstring::npos : e - s - 3);
    size_t at = host.rfind(L'@');
    if (at != std::wstring::npos) host = host.substr(at + 1);
    if (starts_with(host, L"www.")) host = host.substr(4);
    return host.empty() ? L"" : scheme + L"://" + host;
}
static std::wstring site_shown(const SavedLogin &r) {
    std::wstring h = url_host(r.realm);
    if (h.empty()) return r.realm;
    return starts_with(h, L"www.") ? h.substr(4) : h;
}
bool vault_site_has(const std::wstring &url) {
    if (g_incognito) return false;
    std::wstring k = site_key(url);
    if (k.empty()) return false;
    maybe_reload();
    for (auto &r : g_rows) if (!r.forgotten && site_key(r.realm) == k) return true;
    return false;
}

// ---------------------------------------------------------------- is it the person? (Windows Hello, else the password)
static bool g_verifying;
static std::atomic<bool> g_cancel;
static std::function<void(bool)> g_then;   // what to do once answered
struct Grant { const void *who; double until; };
static std::vector<Grant> g_grants;
static int g_panel_tag;   // the key panel's grant (the settings pages use their view)
static bool granted(const void *who) {
    double now = now_ms();
    for (auto &g : g_grants) if (g.who == who && now < g.until) return true;
    return false;
}
static void revoke(const void *who) {
    g_grants.erase(std::remove_if(g_grants.begin(), g_grants.end(), [&](const Grant &g) { return g.who == who; }), g_grants.end());
}
static void grant(const void *who) { revoke(who); g_grants.push_back({ who, now_ms() + kGrantMs }); }
// each settings page's loads: an answer that comes after the page went (or loaded again) counts for nothing
static std::vector<std::pair<const void *, int>> g_epochs;
static int epoch_of(const void *who) { for (auto &e : g_epochs) if (e.first == who) return e.second; return 0; }

template <class Op, class R> static bool wait_op(Op *op, R *out) {
    ComPtr<IAsyncInfo> info;
    if (FAILED(op->QueryInterface(IID_PPV_ARGS(&info)))) return false;
    ABI::Windows::Foundation::AsyncStatus st;
    for (;;) {
        if (FAILED(info->get_Status(&st))) return false;
        if ((int)st != 0 /* Started */) break;
        if (g_cancel.exchange(false)) info->Cancel();   // (tests: the prompt closed by Pluma itself)
        Sleep(30);
    }
    return (int)st == 1 /* Completed */ && SUCCEEDED(op->GetResults(out));
}
// 1 verified, 0 not (cancelled, wrong), -1 Hello is not there (then the password)
static int verify_hello(HWND owner, const std::wstring &msg) {
    ComPtr<IUserConsentVerifierStatics> st;
    HStringReference cls(RuntimeClass_Windows_Security_Credentials_UI_UserConsentVerifier);
    if (FAILED(RoGetActivationFactory(cls.Get(), IID_PPV_ARGS(&st)))) return -1;
    ComPtr<IAsyncOperation<UserConsentVerifierAvailability>> av;
    UserConsentVerifierAvailability a = UserConsentVerifierAvailability::UserConsentVerifierAvailability_DeviceNotPresent;
    if (FAILED(st->CheckAvailabilityAsync(&av)) || !wait_op(av.Get(), &a)) return -1;
    if (a != UserConsentVerifierAvailability::UserConsentVerifierAvailability_Available) return -1;
    ComPtr<IUserConsentVerifierInterop> interop;
    if (FAILED(st.As(&interop))) return -1;
    ComPtr<IAsyncOperation<UserConsentVerificationResult>> op;
    HStringReference m(msg.c_str());
    if (FAILED(interop->RequestVerificationForWindowAsync(owner, m.Get(), IID_PPV_ARGS(&op)))) return -1;
    log_line(L"contraseñas: Windows Hello pregunta");
    UserConsentVerificationResult r = UserConsentVerificationResult::UserConsentVerificationResult_Canceled;
    if (!wait_op(op.Get(), &r)) return 0;
    if (r == UserConsentVerificationResult::UserConsentVerificationResult_Verified) return 1;
    if (r == UserConsentVerificationResult::UserConsentVerificationResult_Canceled || r == UserConsentVerificationResult::UserConsentVerificationResult_RetriesExhausted) return 0;
    return -1;   // busy, off by policy, not set up...: the password instead
}
// the credentials typed must be this account's (the prompt lets the person switch to another one)
static bool same_account(HANDLE lsa, ULONG pkg, void *buf, ULONG n, PSID me) {
    LSA_STRING origin; char oname[] = "Pluma";
    origin.Buffer = oname; origin.Length = (USHORT)strlen(oname); origin.MaximumLength = origin.Length + 1;
    TOKEN_SOURCE src = {};
    memcpy(src.SourceName, "Pluma", 5);
    if (!AllocateLocallyUniqueId(&src.SourceIdentifier)) return false;
    void *profile = nullptr; ULONG plen = 0; LUID luid; HANDLE token = NULL; QUOTA_LIMITS q; NTSTATUS sub = 0;
    NTSTATUS s = LsaLogonUser(lsa, &origin, Interactive, pkg, buf, n, nullptr, &src, &profile, &plen, &luid, &token, &q, &sub);
    if (profile) LsaFreeReturnBuffer(profile);
    bool ok = false;
    if (s == 0 && token) {
        BYTE info[256]; DWORD len = 0;
        if (GetTokenInformation(token, TokenUser, info, sizeof(info), &len)) ok = EqualSid(((TOKEN_USER *)info)->User.Sid, me) != FALSE;
    }
    if (token) CloseHandle(token);
    return ok;
}
static int verify_password(HWND owner, const std::wstring &msg) {
    BYTE me[256]; DWORD len = 0; HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    bool got = GetTokenInformation(tok, TokenUser, me, sizeof(me), &len) != FALSE;
    CloseHandle(tok);
    if (!got) return 0;
    HANDLE lsa = NULL;
    if (LsaConnectUntrusted(&lsa) != 0) return 0;
    CREDUI_INFOW cui = { sizeof(cui) };
    cui.hwndParent = owner;
    cui.pszMessageText = msg.c_str();
    cui.pszCaptionText = L"Pluma";
    DWORD err = 0;
    int r = 0;
    log_line(L"contraseñas: Windows pide la contraseña de la cuenta");
    for (int tries = 0; tries < 3 && r == 0; tries++) {
        ULONG pkg = 0, n = 0; void *buf = nullptr; BOOL save = FALSE;
        err = CredUIPromptForWindowsCredentialsW(&cui, err, &pkg, nullptr, 0, &buf, &n, &save, CREDUIWIN_ENUMERATE_CURRENT_USER);
        if (err != ERROR_SUCCESS) break;   // cancelled
        if (same_account(lsa, pkg, buf, n, ((TOKEN_USER *)me)->User.Sid)) r = 1;
        else err = ERROR_LOGON_FAILURE;
        SecureZeroMemory(buf, n);
        CoTaskMemFree(buf);
    }
    LsaDeregisterLogonProcess(lsa);
    return r;
}
static int test_answer() {
    // test instances only: 1 yes, 2 no, 3 yes after 2 s; 0 the real prompt
    if (!g_test) return 0;
    wchar_t v[16] = L"";
    if (!GetEnvironmentVariableW(L"PLUMA_PRUEBA_VERIFICAR", v, 16)) return 0;
    return !wcscmp(v, L"si") ? 1 : !wcscmp(v, L"no") ? 2 : !wcscmp(v, L"lento") ? 3 : 0;
}
// then(true) once it is them (at once while who's permission lasts); one prompt at a time
static void verify(const void *who, std::function<void(bool)> then) {
    if (granted(who)) { then(true); return; }
    if (g_verifying) return;
    g_verifying = true;
    g_cancel = false;
    HWND owner = g_win;
    std::wstring msg = T(L"Pluma quiere mostrar o copiar una contraseña guardada.");
    int test = test_answer();
    int ep = epoch_of(who);
    g_then = then;   // (stays on this thread: it may hold the page's view, which is only touched here)
    std::thread([owner, msg, test, who, ep] {
        int r;
        if (test) { if (test == 3) Sleep(2000); r = test == 2 ? 0 : 1; }
        else {
            RoInitialize(RO_INIT_MULTITHREADED);
            r = verify_hello(owner, msg);
            if (r < 0) r = verify_password(owner, msg);
            RoUninitialize();
        }
        ui_post([r, who, ep] {
            g_verifying = false;
            log_line(r == 1 ? L"contraseñas: verificado" : L"contraseñas: sin verificar");
            bool ok = r == 1 && epoch_of(who) == ep;   // (the page that asked is still the same load)
            if (ok) grant(who);
            auto then = std::move(g_then);
            g_then = nullptr;
            if (then) then(ok);
        });
    }).detach();
}
bool vault_verifying() { return g_verifying; }

// ---------------------------------------------------------------- the clipboard
static DWORD g_clip_seq;
static void CALLBACK clip_clear(HWND h, UINT, UINT_PTR id, DWORD) {
    KillTimer(h, id);
    // only if what is there is still the password Pluma put (anything copied since stays)
    if (!g_clip_seq || GetClipboardSequenceNumber() != g_clip_seq) { g_clip_seq = 0; return; }
    if (OpenClipboard(g_app)) { EmptyClipboard(); CloseClipboard(); log_line(L"contraseñas: portapapeles vaciado"); }
    g_clip_seq = 0;
}
static bool clip_put(const std::wstring &text, bool secret) {
    if (!OpenClipboard(g_app)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        void *p = GlobalLock(h);
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
        if (!ok) GlobalFree(h);
    }
    if (ok && secret) {
        // out of the clipboard history (Win+V) and the cloud clipboard, and clipboard managers leave it alone
        auto put = [](const wchar_t *name, DWORD v) {
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!g) return;
            *(DWORD *)GlobalLock(g) = v;
            GlobalUnlock(g);
            if (!SetClipboardData(RegisterClipboardFormatW(name), g)) GlobalFree(g);
        };
        put(L"ExcludeClipboardContentFromMonitorProcessing", 0);
        put(L"CanIncludeInClipboardHistory", 0);
        put(L"CanUploadToCloudClipboard", 0);
    }
    CloseClipboard();
    if (ok && secret) {
        g_clip_seq = GetClipboardSequenceNumber();
        int s = kClipS;
        wchar_t v[16] = L"";
        if (g_test && GetEnvironmentVariableW(L"PLUMA_PRUEBA_PORTAPAPELES_S", v, 16)) s = std::max(1, _wtoi(v));
        SetTimer(g_app, 0x7A17, s * 1000, clip_clear);
    }
    return ok;
}
static void wipe(std::string &s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size()); s.clear(); }
static void wipe(std::wstring &s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t)); s.clear(); }
// decrypt one row; 0 ok, 1 app-bound, 2 failed
static int open_row(const SavedLogin &r, std::wstring &out) {
    std::string plain;
    int k = passwords_decrypt(r.blob, plain);
    if (k == 0) out = wide(plain);
    wipe(plain);
    return k;
}

// ---------------------------------------------------------------- the settings page (Configuración > Contraseñas)
static bool page_still_ours(ICoreWebView2 *v) {
    LPWSTR src = NULL;
    if (FAILED(v->get_Source(&src)) || !src) return false;
    bool ok = settings_main_page(src);
    CoTaskMemFree(src);
    return ok;
}
static void post(ICoreWebView2 *v, const std::wstring &msg) { if (page_still_ours(v)) v->PostWebMessageAsJson(msg.c_str()); }
static void post_list(ICoreWebView2 *v) {
    std::wstring o = L"{\"op\":\"pwList\",\"from\":" + std::to_wstring(g_incognito ? -1 : g_from) + L",\"incognito\":" +
                     (g_incognito ? L"true" : L"false") + L",\"rows\":[";
    bool first = true;
    for (auto &r : g_rows) {
        std::wstring url = site_key(r.origin).empty() ? L"" : r.origin;
        o += (first ? L"" : L",") + std::wstring(L"{\"id\":") + std::to_wstring(r.id) + L",\"site\":" + json_escape(site_shown(r)) +
             L",\"url\":" + json_escape(url) + L",\"user\":" + json_escape(r.user) + L",\"del\":" + (r.forgotten ? L"true" : L"false") +
             L",\"bound\":" + (r.blob.compare(0, 3, "v20") == 0 ? L"true" : L"false") + L"}";
        first = false;
    }
    post(v, o + L"]}");
}
static void post_done(ICoreWebView2 *v, long long id, const wchar_t *what) {
    post(v, L"{\"op\":\"pwDone\",\"id\":" + std::to_wstring(id) + L",\"what\":\"" + what + L"\"}");
}
void vault_page_navigating(ICoreWebView2 *v) {
    revoke(v);
    for (auto &e : g_epochs) if (e.first == v) { e.second++; return; }
    g_epochs.push_back({ v, 1 });
}
void vault_page_do(ICoreWebView2 *v, const Json &j) {
    std::wstring cmd = j[L"cmd"].s;
    long long id = (long long)j[L"id"].n;
    ComPtr<ICoreWebView2> keep(v);   // (the answer may come after a prompt: the view stays alive, and is checked again)
    if (cmd == L"list") { if (g_incognito) post_list(v); else reload([keep] { post_list(keep.Get()); }); return; }
    if (g_incognito) return;
    const SavedLogin *r = row_by_id(id);
    if (!r) { post_done(v, id, L"failed"); return; }
    if (cmd == L"copyUser") { post_done(v, id, clip_put(r->user, false) ? L"copiedUser" : L"failed"); return; }
    if (cmd == L"open") { if (!site_key(r->origin).empty()) tab_new(r->origin, true); return; }
    if (cmd == L"delete" || cmd == L"undelete") {
        passwords_forget(*r, cmd == L"undelete");
        reload([keep] { post_list(keep.Get()); });
        return;
    }
    if (cmd != L"reveal" && cmd != L"copy") return;
    if (r->blob.compare(0, 3, "v20") == 0) { post_done(v, id, L"bound"); return; }
    bool copy = cmd == L"copy";
    verify(v, [keep, id, copy](bool ok) {
        if (!page_still_ours(keep.Get())) { log_line(L"contraseñas: la página ya no está, no se manda nada"); return; }
        if (!ok) { post_done(keep.Get(), id, L"denied"); return; }
        const SavedLogin *r = row_by_id(id);
        std::wstring pw;
        int k = r ? open_row(*r, pw) : 2;
        if (k != 0) { post_done(keep.Get(), id, k == 1 ? L"bound" : L"failed"); return; }
        if (copy) {
            bool put = clip_put(pw, true);
            if (put) log_line(L"contraseñas: copiada (se borra del portapapeles en " + std::to_wstring(kClipS) + L" s)");
            post_done(keep.Get(), id, put ? L"copied" : L"failed");
        } else {
            std::wstring msg = L"{\"op\":\"pwValue\",\"id\":" + std::to_wstring(id) + L",\"value\":" + json_escape(pw) + L"}";
            post(keep.Get(), msg);
            wipe(msg);
        }
        wipe(pw);
    });
}

// ---------------------------------------------------------------- the key panel (ui.cpp draws it)
static long long g_shown_id = -1;
static std::wstring g_shown;
std::vector<VaultRow> vault_site_rows(const std::wstring &url) {
    std::vector<VaultRow> out;
    std::wstring k = site_key(url);
    if (k.empty() || g_incognito) return out;
    for (auto &r : g_rows)
        if (!r.forgotten && site_key(r.realm) == k) out.push_back({ (int)r.id, r.user, r.id == g_shown_id ? g_shown : L"" });
    return out;
}
void vault_panel_closed() {
    if (g_verifying) return;   // (Windows's prompt took the focus: the panel comes back when it is answered)
    wipe(g_shown);
    g_shown_id = -1;
    revoke(&g_panel_tag);
}
void vault_panel_do(int what, int id, const std::wstring &url) {
    const SavedLogin *r = row_by_id(id);
    if (!r || site_key(r->realm) != site_key(url)) return;
    if (what == 3) { if (clip_put(r->user, false)) ui_toast(T(L"Usuario copiado")); return; }
    if (what == 1 && g_shown_id == id) { wipe(g_shown); g_shown_id = -1; ui_open_passwords_panel(g_test); return; }
    if (r->blob.compare(0, 3, "v20") == 0) { ui_toast(T(L"Pluma no puede leer esta contraseña: el motor la guarda cifrada para él solo.")); return; }
    HWND owner = g_win;
    verify(&g_panel_tag, [what, id, owner](bool ok) {
        Window *w = win_of_hwnd(owner);
        if (!w) return;
        WinScope ws(w);
        if (!ok) { ui_toast(T(L"No se ha podido comprobar que eres tú.")); return; }
        const SavedLogin *r = row_by_id(id);
        std::wstring pw;
        int k = r ? open_row(*r, pw) : 2;
        if (k != 0) { ui_toast(k == 1 ? T(L"Pluma no puede leer esta contraseña: el motor la guarda cifrada para él solo.") : T(L"No se pudo leer la contraseña.")); return; }
        if (what == 2) {
            if (clip_put(pw, true)) {
                log_line(L"contraseñas: copiada (se borra del portapapeles en " + std::to_wstring(kClipS) + L" s)");
                ui_toast(T(L"Contraseña copiada. Se borra del portapapeles en 30 s."));
            }
            wipe(pw);
        } else { wipe(g_shown); g_shown = pw; g_shown_id = id; wipe(pw); }
        // the panel back (Windows's prompt took the focus and it went), with the site still on screen
        Tab *t = active_tab();
        if (t && r && site_key(t->url) == site_key(r->realm)) ui_open_passwords_panel(g_test);   // (tests: never the focus)
    });
}

// ---------------------------------------------------------------- tests
void vault_test(const std::wstring &what) {
    if (!g_test) return;
    if (what == L"cancelar") { g_cancel = true; return; }   // (Hello's prompt, through its operation)
    if (what == L"sembrar") {
        // invented logins for the next start (never anyone's real ones): two sites and the local test pages
        int a = passwords_keep(L"https://cuenta.ejemplo-pluma.test/entrar", L"ana.prueba", L"Prueba-Uno-123");
        int b = passwords_keep(L"https://cuenta.ejemplo-pluma.test/entrar", L"beto@ejemplo-pluma.test", L"Otra.Clave-456");
        int c = passwords_keep(L"https://tienda.ejemplo-pluma.test/login", L"carla", L"Tienda_789-xyz");
        int d = passwords_keep(L"http://127.0.0.1:9513/login.html", L"usuario.prueba", L"Local-Prueba-321");
        log_line(L"prueba: sembradas " + std::to_wstring((a == 1) + (b == 1) + (c == 1) + (d == 1)));
        return;
    }
    if (what == L"lista") {   // what the list holds now: counts only
        int n = 0, gone = 0;
        for (auto &r : g_rows) { n++; if (r.forgotten) gone++; }
        log_line(L"prueba: contraseñas " + std::to_wstring(n) + L", por quitar " + std::to_wstring(gone) + L", origen " + std::to_wstring(g_from));
        return;
    }
    if (what == L"claves") { ui_open_passwords_panel(true); return; }
    // clave-ver|clave-copiar|clave-usuario=<n>: the n-th row of the panel, as its button
    size_t eq = what.find(L'=');
    if (eq == std::wstring::npos) return;
    Tab *t = active_tab();
    if (!t) return;
    auto rows = vault_site_rows(t->url);
    int n = _wtoi(what.c_str() + eq + 1);
    if (n < 1 || n > (int)rows.size()) return;
    std::wstring verb = what.substr(0, eq);
    vault_panel_do(verb == L"clave-ver" ? 1 : verb == L"clave-copiar" ? 2 : 3, rows[n - 1].id, t->url);
}
