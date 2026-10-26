// Pluma: updates, from the releases of the public repository (github.com/Windows-Lite/pluma).
// actualizar= in ajustes.ini: auto (the default) | avisar | no. A release build carries its version (PLUMA_VERSION, set
// by release.yml); a local build is "dev" and never updates.
// - 30 s after starting and every 6 h, a worker thread asks GitHub for the latest release.
// - Newer, with auto: pluma-vX.Y.Z.zip is downloaded, checked against the SHA-256 GitHub publishes for it and unpacked
//   into actualizacion\ next to the exe. With the window closed (Pluma resident) it goes in at once and Pluma restarts
//   hidden; with the window open, a note offers to restart now, else it goes in when the window closes.
//   With avisar, only the note: its button downloads it and restarts.
// - Going in: a running exe can be renamed but not overwritten, so Pluma.exe -> Pluma.old.exe and the new one in its
//   place; the new process copies the rest (extensions, fonts...) before its engine starts, and deletes the old exe.
// - A folder Pluma cannot write (Program Files, as Windows Lite installs it): only the note, with the release's page.
#include "pluma.h"
#include "json.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <thread>

#ifndef PLUMA_VERSION
#define PLUMA_VERSION "dev"
#endif
static const wchar_t *kVersion = L"" PLUMA_VERSION;
static const wchar_t *kLatest = L"https://api.github.com/repos/Windows-Lite/pluma/releases/latest";
static const wchar_t *kPage = L"https://github.com/Windows-Lite/pluma/releases/latest";
static const wchar_t *kDownload = L"https://github.com/Windows-Lite/pluma/releases/download/";

static std::atomic<bool> g_busy{ false };
static std::atomic<bool> g_asked_by_hand{ false };   // someone clicked while a check was already on its way: it answers them
static std::wstring g_ready;        // the version waiting in actualizacion\ to go in (window thread)
static std::wstring g_announced;    // the version the note was shown for, once per version

const wchar_t *update_version() { return kVersion; }
static std::wstring staging() { return g_root + L"\\actualizacion"; }
// PLUMA_ACTUALIZAR_URL: another "latest release" (a test instance's, served from 127.0.0.1)
static std::wstring latest_url() {
    wchar_t u[512] = L"";
    return GetEnvironmentVariableW(L"PLUMA_ACTUALIZAR_URL", u, 512) > 0 ? u : kLatest;
}

static bool newer(const std::wstring &tag, const std::wstring &mine) {
    int a[3] = {}, b[3] = {};
    if (swscanf_s(tag.c_str() + (tag[0] == L'v'), L"%d.%d.%d", &a[0], &a[1], &a[2]) < 2) return false;
    if (swscanf_s(mine.c_str() + (mine[0] == L'v'), L"%d.%d.%d", &b[0], &b[1], &b[2]) < 2) return false;
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}
static bool writable(const std::wstring &dir) {
    std::wstring probe = dir + L"\\.pluma-escribe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
static void delete_tree(const std::wstring &dir) {
    std::wstring from = dir + L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

// a GET (worker thread), up to max bytes; GitHub's redirects to its download servers are followed by WinHTTP
static bool fetch(const std::wstring &url, std::string &out, size_t max, std::wstring *moved = nullptr) {
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && wcscmp(host, L"127.0.0.1")) return false;   // (plain http only for the tests' server)
    std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + (uc.lpszExtraInfo ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : L"");
    std::wstring agent = std::wstring(L"Pluma/") + kVersion;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 8000, 8000, 15000, 30000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r && moved) { DWORD off = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &off, sizeof(off)); }
    const wchar_t *hdr = L"Accept: application/vnd.github+json\r\n";
    if (r && WinHttpSendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (moved && code >= 300 && code < 400) {
            // (only where it goes: the address of the redirect)
            wchar_t loc[1024]; DWORD ll = sizeof(loc);
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &ll, WINHTTP_NO_HEADER_INDEX)) { *moved = loc; ok = true; }
            WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
            return ok;
        }
        char buf[65536]; DWORD got = 0;
        while (WinHttpReadData(r, buf, sizeof(buf), &got) && got > 0 && out.size() <= max) out.append(buf, got);
        ok = code == 200 && !out.empty() && out.size() <= max;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
static std::wstring sha256_hex(const std::string &data) {
    unsigned char h[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32) != 0) return L"";
    wchar_t out[65];
    for (int i = 0; i < 32; i++) swprintf_s(out + i * 2, 3, L"%02x", h[i]);
    return out;
}

struct Release { std::wstring tag, zip_url, sha256, page; };
static bool latest_from_page(Release &rel) {
    // GitHub's API allows 60 questions an hour per address (403 after): the release page's redirect has no such
    // limit, and its tag gives the zip's and the .sha256's addresses
    std::string none; std::wstring to;
    if (!fetch(kPage, none, 1 << 16, &to)) return false;
    size_t at = to.find(L"/releases/tag/");
    if (at == std::wstring::npos) return false;
    rel.tag = to.substr(at + 14);
    rel.page = to;
    std::wstring base = std::wstring(kDownload) + rel.tag + L"/pluma-" + rel.tag + L".zip";
    rel.zip_url = base;
    std::string sum;
    if (fetch(base + L".sha256", sum, 4096) && sum.size() >= 64) rel.sha256 = lower(wide(sum.substr(0, 64)));
    return !rel.tag.empty();
}
static bool latest(Release &rel) {
    std::string body;
    wchar_t test[8];
    if (!fetch(latest_url(), body, 1 << 20)) return GetEnvironmentVariableW(L"PLUMA_ACTUALIZAR_URL", test, 8) == 0 && latest_from_page(rel);
    Json j = json_parse(wide(body));
    rel.tag = j[L"tag_name"].s;
    rel.page = j[L"html_url"].s;
    std::wstring zip = L"pluma-" + rel.tag + L".zip", sum_url;
    for (auto &a : j[L"assets"].arr) {
        std::wstring name = a[L"name"].s;
        if (name == zip + L".sha256") sum_url = a[L"browser_download_url"].s;
        if (name != zip) continue;
        rel.zip_url = a[L"browser_download_url"].s;
        std::wstring d = a[L"digest"].s;   // GitHub's own, "sha256:..."
        if (starts_with(d, L"sha256:")) rel.sha256 = lower(d.substr(7));
    }
    // the release's .sha256 ("<64 hex>  <file>") wins over GitHub's digest
    std::string sum;
    if (!sum_url.empty() && fetch(sum_url, sum, 4096) && sum.size() >= 64) rel.sha256 = lower(wide(sum.substr(0, 64)));
    return !rel.tag.empty();
}
// downloaded, checked and unpacked into actualizacion\<version>\ (worker thread)
static bool stage(const Release &rel) {
    if (rel.zip_url.empty() || rel.sha256.size() != 64) { log_line(L"actualización " + rel.tag + L": sin zip o sin su SHA-256"); return false; }
    std::string zip;
    if (!fetch(rel.zip_url, zip, 64 << 20)) { log_line(L"actualización " + rel.tag + L": no se pudo bajar"); return false; }
    if (sha256_hex(zip) != rel.sha256) { log_line(L"actualización " + rel.tag + L": el SHA-256 no coincide"); return false; }
    std::wstring dir = staging();
    delete_tree(dir);
    SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
    std::wstring file = dir + L"\\pluma.zip";
    if (!write_file(file, zip)) return false;
    // Windows' own tar reads zip files (and refuses paths that climb out of the folder)
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + file + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    DWORD code = 1;
    if (CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000); GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    DeleteFileW(file.c_str());
    std::wstring top = dir + L"\\pluma-" + rel.tag;
    if (code != 0 || !PathFileExistsW((top + L"\\Pluma.exe").c_str())) { log_line(L"actualización " + rel.tag + L": el zip no trae Pluma.exe"); delete_tree(dir); return false; }
    log_line(L"actualización " + rel.tag + L" lista para entrar");
    return true;
}

// in place: the exe swapped (window thread). The rest comes in when the new one starts (update_finish)
static bool swap_exe() {
    std::wstring top = staging() + L"\\pluma-" + g_ready;
    std::wstring exe = g_root + L"\\Pluma.exe", old = g_root + L"\\Pluma.old.exe";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { log_line(L"actualización: no se pudo apartar Pluma.exe"); return false; }
    if (!MoveFileExW((top + L"\\Pluma.exe").c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        log_line(L"actualización: no se pudo poner el Pluma.exe nuevo");
        return false;
    }
    log_line(L"actualización: entra " + g_ready);
    g_ready.clear();
    return true;
}
// ... and Pluma started again from it, this one gone
static void go_in(bool hidden) { if (swap_exe()) app_restart(hidden); }

// the window thread decides once a version is waiting
static void ready(const Release &rel) {
    g_ready = rel.tag;
    log_line(std::wstring(L"actualización: esperando a entrar, ventana ") + (IsWindowVisible(g_win) ? L"abierta" : L"cerrada"));
    if (g_set.update == 2 && !IsWindowVisible(g_win)) { go_in(true); return; }
    if (g_announced == rel.tag) return;
    g_announced = rel.tag;
    ui_notice(T(L"Nueva versión de Pluma lista: ") + rel.tag + T(L". Se instala al cerrar la ventana."), T(L"Reiniciar ahora"), [] { go_in(false); });
}
static void check(bool by_hand);
static void CALLBACK tick(HWND h, UINT, UINT_PTR id, DWORD) {
    SetTimer(h, id, 6 * 3600 * 1000, tick);   // then every 6 h
    check(false);
}
static void retry_soon() { SetTimer(g_win, 70, 30 * 60 * 1000, tick); }   // no answer (offline...): again in 30 min
static void check(bool by_hand) {
    if (by_hand) {
        if (!wcscmp(kVersion, L"dev")) { ui_toast(T(L"Esta copia de Pluma no se actualiza sola")); return; }
        ui_toast(T(L"Buscando actualizaciones..."));
        g_asked_by_hand = true;
    }
    if (g_busy.exchange(true)) return;
    std::thread([] {
        Release rel;
        bool asked = latest(rel);
        log_line(asked ? L"actualización: la última es " + rel.tag + L" (esta, " + kVersion + L")" : L"actualización: no se pudo consultar");
        bool found = asked && newer(rel.tag, kVersion);
        bool can = found && writable(g_root);
        bool staged = found && can && g_set.update == 2 && stage(rel);
        ui_post([rel, asked, found, can, staged] {
            g_busy = false;
            bool by_hand = g_asked_by_hand.exchange(false);
            if (!asked) {
                if (by_hand) ui_toast(T(L"No se pudo comprobar: sin respuesta de GitHub"));
                else retry_soon();
                return;
            }
            if (!found) { if (by_hand) ui_toast(std::wstring(T(L"Pluma está al día")) + L" (" + kVersion + L")"); return; }
            if (staged) { ready(rel); return; }
            if (g_announced == rel.tag && !by_hand) return;
            g_announced = rel.tag;
            // avisar, or a folder Pluma cannot write: the note; its button downloads and restarts (or shows the release)
            std::wstring text = T(L"Hay una versión nueva de Pluma: ") + rel.tag;
            if (!can) ui_notice(text, T(L"Ver"), [page = rel.page] { tab_new(page, true); });
            else ui_notice(text, T(L"Actualizar y reiniciar"), [rel] {
                ui_toast(T(L"Descargando la actualización..."));
                std::thread([rel] {
                    bool ok = stage(rel);
                    ui_post([rel, ok] { if (!ok) { ui_toast(T(L"No se pudo actualizar")); return; } g_ready = rel.tag; go_in(false); });
                }).detach();
            });
        });
    }).detach();
}

void update_check_now() { check(true); }
void update_on_hide() { if (!g_ready.empty() && g_set.update == 2) go_in(true); }
void update_on_quit() { if (!g_ready.empty()) swap_exe(); }   // closing for good: the next start is the new one

// at start, before the engine: what the last update left (window thread)
void update_finish() {
    if (g_incognito) return;   // (the ordinary Pluma updates; the incognito window only runs it)
    DeleteFileW((g_root + L"\\Pluma.old.exe").c_str());
    std::wstring top = staging() + L"\\pluma-" + kVersion;
    if (PathFileExistsW(top.c_str())) {
        // the rest of the release over the old files: extensions, fonts, the installer
        std::wstring from = top + L"\\*" + L'\0', to = g_root + L'\0';
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_COPY;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_NO_UI;
        if (SHFileOperationW(&op) == 0) log_line(std::wstring(L"actualización ") + kVersion + L" completa");
    }
    if (PathFileExistsW(staging().c_str())) delete_tree(staging());
}
void update_init() {
    wchar_t u[8];
    bool test_url = GetEnvironmentVariableW(L"PLUMA_ACTUALIZAR_URL", u, 8) > 0;
    if (g_incognito || (g_test && !test_url) || g_set.update == 0 || !wcscmp(kVersion, L"dev")) return;
    SetTimer(g_win, 70, test_url ? 3000 : 30000, tick);
}
