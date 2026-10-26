// Pluma: sync / backup. An encrypted copy of the profile (bookmarks, history, open tabs, settings, scripts,
// extensions and their settings) that goes to a folder, or to the account's private cloud folder once its app is
// registered. The file format and the crypto are in backup.h (checked by tools\sync-test.cpp); this file gathers
// the data, runs the backup off the window's thread, and restores it at the next start with the engine stopped.
//
// Every profile goes in. Passwords and cookies (sessions) too: the engine keeps them encrypted with its key, which
// Windows guards for this account only (a new Windows can't open it). The backup carries that key (meta/clave),
// sealed with the backup password like the rest, and a restore guards it again for the new account.
//
// What is kept on this machine (never in the backup): the backup password, remembered as a key derived with Argon2id
// and then DPAPI-protected (sync-clave.bin), and the cloud refresh token, DPAPI-protected (sync-oauth.bin). The rest
// of the config (destination, folder, hint, last backup) is in sync-estado.json, in clear (nothing secret there).
#include "pluma.h"
#include "json.h"
#include "backup.h"
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <commdlg.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>

// the loopback address the OAuth sign-in comes back to; register this exact value (Dropbox needs the port)
static const int kLoopbackPort = 47615;
static std::wstring loopback_uri() { return L"http://127.0.0.1:" + std::to_wstring(kLoopbackPort) + L"/"; }

// App identifiers: from secrets.h, which release.yml writes from the repo's secrets (not in git), or [sync] in
// ajustes.ini; empty = the cloud destinations stay off with a clear error. The folder destination never needs them.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef PLUMA_GOOGLE_CLIENT_ID
#define PLUMA_GOOGLE_CLIENT_ID L""
#endif
#ifndef PLUMA_GOOGLE_CLIENT_SECRET
#define PLUMA_GOOGLE_CLIENT_SECRET L""
#endif
static const wchar_t *kGoogleClientId = PLUMA_GOOGLE_CLIENT_ID;
// a Google "Desktop app" client still sends its secret at token time; Google treats it as public for installed apps
static const wchar_t *kGoogleClientSecret = PLUMA_GOOGLE_CLIENT_SECRET;
static const wchar_t *kOneDriveClientId = L"";
static const wchar_t *kDropboxClientId = L"";

// ---------------------------------------------------------------- config and status
struct SyncConfig {
    std::wstring dest;        // "" off, "carpeta", "gdrive", "onedrive", "dropbox"
    std::wstring folder;      // for "carpeta"
    std::wstring hint;        // the password hint (shown, never the password)
    std::wstring account;     // the connected account's label (email/name)
    bool automatic = true;    // hourly if changed
    std::wstring last_iso;    // when the last backup finished
    long long last_size = 0;  // its size in bytes
    std::wstring signature;   // the last backed-up content's signature (to skip when nothing changed)
};
static SyncConfig g_cfg;
static std::mutex g_cfg_mtx;
static std::wstring g_status;          // a short line for the settings page ("", "Copiando...", "Copia hecha", an error)
static std::atomic<bool> g_busy{ false };

static std::thread g_worker;
static HANDLE g_wake = NULL;           // set to run a backup now
static HANDLE g_stop = NULL;          // set to end the worker
static std::atomic<bool> g_req_now{ false };    // a manual backup: always writes, and shows a toast
static std::atomic<bool> g_req_due{ false };    // a close or the hourly tick: only if the content changed
static std::atomic<bool> g_restoring{ false };  // a restore is staging: do not back up on the way out
static double g_last_run_ms = 0;

static std::wstring estado_file() { return g_state + L"\\sync-estado.json"; }
static std::wstring clave_file() { return g_state + L"\\sync-clave.bin"; }
static std::wstring oauth_file() { return g_state + L"\\sync-oauth.bin"; }
static std::wstring engine_base() { return g_state + L"\\webview2\\EBWebView"; }

// ---------------------------------------------------------------- DPAPI (bound to this Windows account)
static bool dpapi_protect(const std::string &in, std::string &out) {
    DATA_BLOB i{ (DWORD)in.size(), (BYTE *)in.data() }, o{};
    if (!CryptProtectData(&i, L"Pluma sync", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &o)) return false;
    out.assign((char *)o.pbData, o.cbData);
    LocalFree(o.pbData);
    return true;
}
static bool dpapi_unprotect(const std::string &in, std::string &out) {
    DATA_BLOB i{ (DWORD)in.size(), (BYTE *)in.data() }, o{};
    if (!CryptUnprotectData(&i, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &o)) return false;
    out.assign((char *)o.pbData, o.cbData);
    SecureZeroMemory(o.pbData, o.cbData);
    LocalFree(o.pbData);
    return true;
}

// ---------------------------------------------------------------- small helpers
static std::wstring now_iso() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t s[32]; swprintf_s(s, L"%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return s;
}
static std::wstring stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t s[32]; swprintf_s(s, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}
static std::wstring machine_name() {
    wchar_t n[256] = L""; DWORD sz = 256;
    GetComputerNameW(n, &sz);
    return n;
}
static std::string sha256_raw(const std::string &data) {
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL;
    BYTE out[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
            BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
            BCryptFinishHash(h, out, 32, 0);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return std::string((char *)out, 32);
}
static std::string sha256_hex(const std::string &data) { return bk::hex(sha256_raw(data)); }
static std::string base64url(const std::string &b) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    for (size_t i = 0; i < b.size(); i += 3) {
        unsigned n = (unsigned char)b[i] << 16;
        if (i + 1 < b.size()) n |= (unsigned char)b[i + 1] << 8;
        if (i + 2 < b.size()) n |= (unsigned char)b[i + 2];
        o += t[(n >> 18) & 63]; o += t[(n >> 12) & 63];
        if (i + 1 < b.size()) o += t[(n >> 6) & 63];
        if (i + 2 < b.size()) o += t[n & 63];
    }
    return o;   // no padding, as PKCE wants
}

// ---------------------------------------------------------------- config file
static void config_load() {
    std::string d;
    std::lock_guard<std::mutex> lk(g_cfg_mtx);
    if (!read_file(estado_file(), d)) return;
    Json j = json_parse(wide(d));
    if (j.kind != Json::OBJ) return;
    g_cfg.dest = j[L"destino"].s;
    g_cfg.folder = j[L"carpeta"].s;
    g_cfg.hint = j[L"pista"].s;
    g_cfg.account = j[L"cuenta"].s;
    g_cfg.automatic = j[L"auto"].kind == Json::BOOL ? j[L"auto"].b : true;
    g_cfg.last_iso = j[L"ultima"].s;
    g_cfg.last_size = (long long)j[L"tam"].n;
    g_cfg.signature = j[L"firma"].s;
}
static void config_save_locked() {
    std::wstring o = L"{";
    o += L"\"destino\":" + json_escape(g_cfg.dest);
    o += L",\"carpeta\":" + json_escape(g_cfg.folder);
    o += L",\"pista\":" + json_escape(g_cfg.hint);
    o += L",\"cuenta\":" + json_escape(g_cfg.account);
    o += L",\"auto\":" + std::wstring(g_cfg.automatic ? L"true" : L"false");
    o += L",\"ultima\":" + json_escape(g_cfg.last_iso);
    o += L",\"tam\":" + std::to_wstring(g_cfg.last_size);
    o += L",\"firma\":" + json_escape(g_cfg.signature);
    o += L"}";
    write_file(estado_file(), utf8(o));
}
static void config_save() { std::lock_guard<std::mutex> lk(g_cfg_mtx); config_save_locked(); }

// ---------------------------------------------------------------- the remembered key (DPAPI)
// stored: salt (hex) + kdf params + the 32-byte Argon2id key, all DPAPI-protected. We keep the key, not the password.
static bool key_remembered(bk::Kdf &kdf, uint8_t key[32]) {
    std::string enc, dec;
    if (!read_file(clave_file(), enc) || !dpapi_unprotect(enc, dec)) return false;
    Json j = json_parse(wide(dec));
    SecureZeroMemory(&dec[0], dec.size());
    if (j.kind != Json::OBJ) return false;
    std::string salt, kb;
    std::string salt_hex, key_hex;
    for (wchar_t c : j[L"sal"].s) salt_hex += (char)(c < 128 ? c : '?');
    for (wchar_t c : j[L"key"].s) key_hex += (char)(c < 128 ? c : '?');
    if (!bk::unhex(salt_hex, kdf.salt) || !bk::unhex(key_hex, kb) || kb.size() != 32) return false;
    kdf.m_kib = (uint32_t)j[L"m"].n; kdf.t = (uint32_t)j[L"t"].n; kdf.p = (uint32_t)j[L"p"].n;
    memcpy(key, kb.data(), 32);
    SecureZeroMemory(&kb[0], kb.size());
    return bk::kdf_sane(kdf);
}
static bool key_is_set() { return GetFileAttributesW(clave_file().c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------- gather the profile
static void walk(const std::wstring &dir, const std::string &prefix, std::vector<bk::Entry> &out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        std::wstring name = fd.cFileName;
        std::string child = prefix + "/" + utf8(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walk(dir + L"\\" + name, child, out); continue; }
        if (!bk::allowed_target(child)) continue;   // stay inside what a restore may write
        std::string data;
        if (!read_file(dir + L"\\" + name, data)) continue;
        out.push_back({ child, std::move(data) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static std::wstring back_slashes(std::string s) { for (auto &c : s) if (c == '/') c = '\\'; return wide(s); }
// the engine's profile folders under base: Default and one per Pluma profile
static std::vector<std::string> engine_profiles(const std::wstring &base) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = utf8(fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && bk::engine_profile(n)) out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
// the engine keeps its cookies locked for itself while it runs (nobody else can even read them), so each start copies
// its profile's databases before the engine opens them; the backup takes the live file when it can, else this copy
static std::wstring snapshot_dir() { return g_state + L"\\sync-motor"; }
void sync_snapshot_engine() {
    std::string prof = utf8(wv_profile_dir().substr(wv_profile_dir().find_last_of(L'\\') + 1));
    if (!bk::engine_profile(prof)) return;
    for (auto &db : bk::engine_dbs())
        for (const wchar_t *suf : { L"", L"-wal" }) {
            std::wstring rel = wide(prof) + L"\\" + back_slashes(db) + suf;
            std::wstring to = snapshot_dir() + L"\\" + rel;
            std::string data;
            if (read_file(engine_base() + L"\\" + rel, data)) {
                size_t cut = to.find_last_of(L'\\');
                SHCreateDirectoryExW(NULL, to.substr(0, cut).c_str(), NULL);
                write_file(to, data);
            } else DeleteFileW(to.c_str());   // a -wal gone since the last start must not come back with the copy
        }
}
static std::vector<bk::Entry> gather(std::string &signature) {
    std::vector<bk::Entry> out;
    for (auto &f : bk::pluma_files()) {
        std::string data;
        if (!read_file(g_state + L"\\" + wide(f), data)) continue;
        out.push_back({ "pluma/" + f, std::move(data) });
    }
    for (auto &d : bk::pluma_dirs()) walk(g_state + L"\\" + wide(d), "pluma/" + d, out);
    for (auto &prof : engine_profiles(engine_base()))
        for (auto &p : bk::engine_paths()) {
            std::string name = "motor/" + prof + "/" + p;
            std::wstring full = engine_base() + L"\\" + wide(prof) + L"\\" + back_slashes(p);
            DWORD at = GetFileAttributesW(full.c_str());
            if (at == INVALID_FILE_ATTRIBUTES) continue;
            if (at & FILE_ATTRIBUTE_DIRECTORY) walk(full, name, out);
            else {
                std::string data;
                bool ok = read_file(full, data);
                if (!ok) ok = read_file(snapshot_dir() + L"\\" + wide(prof) + L"\\" + back_slashes(p), data);   // locked: the start's copy
                if (ok && bk::allowed_target(name)) out.push_back({ name, std::move(data) });
            }
        }
    // the signature is of the contents, not the file times: quitting rewrites the session with the same bytes
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE hh = NULL; BYTE dg[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0) {
            for (auto &e : out) {
                uint64_t n = e.data.size();
                BCryptHashData(hh, (PUCHAR)e.name.data(), (ULONG)e.name.size() + 1, 0);   // with its NUL, so names can't run into data
                BCryptHashData(hh, (PUCHAR)&n, sizeof(n), 0);
                if (n) BCryptHashData(hh, (PUCHAR)e.data.data(), (ULONG)n, 0);
            }
            BCryptFinishHash(hh, dg, 32, 0);
            BCryptDestroyHash(hh);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    signature = bk::hex(std::string((char *)dg, 32));
    return out;
}

// ---------------------------------------------------------------- WinHTTP (one request, returns status and body)
static bool http(const std::wstring &method, const std::wstring &url, const std::wstring &headers,
                 const std::string &body, DWORD &status, std::string &resp) {
    status = 0; resp.clear();
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = L"", path[8192] = L"", extra[8192] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 8192;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring object = std::wstring(path) + extra;   // path plus ?query
    HINTERNET s = WinHttpOpen(L"Pluma", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) {
        DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET r = WinHttpOpenRequest(c, method.c_str(), object.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r) {
            if (WinHttpSendRequest(r, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1L,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(r, NULL)) {
                DWORD code = 0, len = sizeof(code);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &len, NULL);
                status = code;
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                    std::string chunk(avail, 0); DWORD got = 0;
                    if (!WinHttpReadData(r, &chunk[0], avail, &got)) break;
                    resp.append(chunk.data(), got);
                }
                ok = true;
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}
static std::wstring json_str(const std::string &body, const wchar_t *key) { return json_parse(wide(body))[key].s; }

// ---------------------------------------------------------------- provider table
struct Provider { const wchar_t *id, *name, *auth, *token, *scope; };
static Provider provider(const std::wstring &dest) {
    if (dest == L"gdrive") return { L"gdrive", L"Google Drive", L"https://accounts.google.com/o/oauth2/v2/auth",
                                    L"https://oauth2.googleapis.com/token", L"https://www.googleapis.com/auth/drive.appdata" };
    if (dest == L"onedrive") return { L"onedrive", L"OneDrive", L"https://login.microsoftonline.com/common/oauth2/v2.0/authorize",
                                      L"https://login.microsoftonline.com/common/oauth2/v2.0/token", L"Files.ReadWrite.AppFolder offline_access" };
    if (dest == L"dropbox") return { L"dropbox", L"Dropbox", L"https://www.dropbox.com/oauth2/authorize",
                                     L"https://api.dropboxapi.com/oauth2/token", L"files.content.write files.content.read files.metadata.read account_info.read" };
    return { L"", L"", L"", L"", L"" };
}
static std::wstring ini_or(const wchar_t *key, const wchar_t *def) {
    wchar_t v[512] = L"";
    GetPrivateProfileStringW(L"sync", key, def, v, 512, (g_state + L"\\ajustes.ini").c_str());
    return v;
}
static std::wstring client_id(const std::wstring &dest) {
    if (dest == L"gdrive") return ini_or(L"google_client_id", kGoogleClientId);
    if (dest == L"onedrive") return ini_or(L"onedrive_client_id", kOneDriveClientId);
    if (dest == L"dropbox") return ini_or(L"dropbox_client_id", kDropboxClientId);
    return L"";
}
static std::wstring client_secret(const std::wstring &dest) {
    if (dest == L"gdrive") return ini_or(L"google_client_secret", kGoogleClientSecret);
    return L"";
}

// ---------------------------------------------------------------- OAuth tokens (refresh token DPAPI on this machine)
static void token_save(const std::wstring &provider, const std::wstring &refresh) {
    std::wstring o = L"{\"proveedor\":" + json_escape(provider) + L",\"refresh\":" + json_escape(refresh) + L"}";
    std::string enc;
    if (dpapi_protect(utf8(o), enc)) write_file(oauth_file(), enc);
}
static std::wstring token_refresh_stored(std::wstring &provider) {
    std::string enc, dec;
    if (!read_file(oauth_file(), enc) || !dpapi_unprotect(enc, dec)) return L"";
    Json j = json_parse(wide(dec));
    provider = j[L"proveedor"].s;
    return j[L"refresh"].s;
}
// a fresh access token from the stored refresh token
static std::wstring access_token(const std::wstring &dest, std::wstring &err) {
    std::wstring stored_prov, refresh = token_refresh_stored(stored_prov);
    if (refresh.empty() || stored_prov != dest) { err = T(L"No hay ninguna cuenta conectada."); return L""; }
    std::wstring id = client_id(dest);
    if (id.empty()) { err = T(L"Falta el identificador de la app de la nube. Regístrala y ponlo en la configuración."); return L""; }
    Provider p = provider(dest);
    std::wstring bodyw = L"client_id=" + url_encode(id) + L"&grant_type=refresh_token&refresh_token=" + url_encode(refresh);
    if (dest == L"gdrive") bodyw += L"&client_secret=" + url_encode(client_secret(dest));
    DWORD st = 0; std::string resp;
    http(L"POST", p.token, L"Content-Type: application/x-www-form-urlencoded\r\n", utf8(bodyw), st, resp);
    if (st != 200) { err = T(L"No se pudo renovar la sesión de la cuenta."); return L""; }
    std::wstring at = json_str(resp, L"access_token");
    std::wstring nr = json_str(resp, L"refresh_token");
    if (!nr.empty()) token_save(dest, nr);   // Google rotates refresh tokens sometimes
    if (at.empty()) err = T(L"No se pudo renovar la sesión de la cuenta.");
    return at;
}

// ---------------------------------------------------------------- cloud: upload, list, delete (<=5 kept, like the folder)
struct Remote { std::wstring id, name; };
static bool cloud_list(const std::wstring &dest, const std::wstring &at, std::vector<Remote> &out) {
    DWORD st = 0; std::string resp;
    std::wstring auth = L"Authorization: Bearer " + at + L"\r\n";
    if (dest == L"gdrive") {
        http(L"GET", L"https://www.googleapis.com/drive/v3/files?spaces=appDataFolder&orderBy=createdTime&fields=files(id,name)", auth, "", st, resp);
        if (st != 200) return false;
        for (auto &f : json_parse(wide(resp))[L"files"].arr) out.push_back({ f[L"id"].s, f[L"name"].s });
        return true;
    }
    if (dest == L"onedrive") {
        http(L"GET", L"https://graph.microsoft.com/v1.0/me/drive/special/approot/children?$select=id,name", auth, "", st, resp);
        if (st != 200) return false;
        for (auto &f : json_parse(wide(resp))[L"value"].arr) out.push_back({ f[L"id"].s, f[L"name"].s });
        return true;
    }
    if (dest == L"dropbox") {
        http(L"POST", L"https://api.dropboxapi.com/2/files/list_folder", auth + L"Content-Type: application/json\r\n", "{\"path\":\"\"}", st, resp);
        if (st != 200) return false;
        for (auto &f : json_parse(wide(resp))[L"entries"].arr) out.push_back({ f[L"path_lower"].s, f[L"name"].s });
        return true;
    }
    return false;
}
static bool cloud_delete(const std::wstring &dest, const std::wstring &at, const Remote &r) {
    DWORD st = 0; std::string resp;
    std::wstring auth = L"Authorization: Bearer " + at + L"\r\n";
    if (dest == L"gdrive") { http(L"DELETE", L"https://www.googleapis.com/drive/v3/files/" + r.id, auth, "", st, resp); return st == 204 || st == 200; }
    if (dest == L"onedrive") { http(L"DELETE", L"https://graph.microsoft.com/v1.0/me/drive/items/" + r.id, auth, "", st, resp); return st == 204; }
    if (dest == L"dropbox") { http(L"POST", L"https://api.dropboxapi.com/2/files/delete_v2", auth + L"Content-Type: application/json\r\n", utf8(L"{\"path\":" + json_escape(r.id) + L"}"), st, resp); return st == 200; }
    return false;
}
static bool onedrive_big(const std::wstring &at, const std::wstring &name, const std::string &data);
static bool cloud_upload(const std::wstring &dest, const std::wstring &at, const std::wstring &name, const std::string &data) {
    DWORD st = 0; std::string resp;
    std::wstring auth = L"Authorization: Bearer " + at + L"\r\n";
    if (dest == L"gdrive") {
        std::string b = "--pbk\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n";
        b += utf8(L"{\"name\":" + json_escape(name) + L",\"parents\":[\"appDataFolder\"]}");
        b += "\r\n--pbk\r\nContent-Type: application/octet-stream\r\n\r\n" + data + "\r\n--pbk--\r\n";
        http(L"POST", L"https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart",
             auth + L"Content-Type: multipart/related; boundary=pbk\r\n", b, st, resp);
        return st == 200;
    }
    if (dest == L"onedrive") {
        if (data.size() > 4 * 1024 * 1024) return onedrive_big(at, name, data);
        http(L"PUT", L"https://graph.microsoft.com/v1.0/me/drive/special/approot:/" + url_encode(name) + L":/content",
             auth + L"Content-Type: application/octet-stream\r\n", data, st, resp);
        return st == 200 || st == 201;
    }
    if (dest == L"dropbox") {
        std::wstring arg = L"{\"path\":\"/" + name + L"\",\"mode\":\"overwrite\",\"mute\":true}";
        http(L"POST", L"https://content.dropboxapi.com/2/files/upload",
             auth + L"Dropbox-API-Arg: " + arg + L"\r\nContent-Type: application/octet-stream\r\n", data, st, resp);
        return st == 200;
    }
    return false;
}
// OneDrive over 4 MB: an upload session in 10 MB chunks (Graph's createUploadSession)
static bool onedrive_big(const std::wstring &at, const std::wstring &name, const std::string &data) {
    DWORD st = 0; std::string resp;
    std::wstring auth = L"Authorization: Bearer " + at + L"\r\n";
    http(L"POST", L"https://graph.microsoft.com/v1.0/me/drive/special/approot:/" + url_encode(name) + L":/createUploadSession",
         auth + L"Content-Type: application/json\r\n", "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"replace\"}}", st, resp);
    if (st != 200) return false;
    std::wstring url = json_str(resp, L"uploadUrl");
    if (url.empty()) return false;
    const size_t chunk = 10 * 1024 * 1024;
    for (size_t off = 0; off < data.size(); off += chunk) {
        size_t n = (std::min)(chunk, data.size() - off);
        wchar_t range[128];
        swprintf_s(range, L"Content-Range: bytes %zu-%zu/%zu\r\n", off, off + n - 1, data.size());
        if (!http(L"PUT", url, range, data.substr(off, n), st, resp)) return false;
        if (st != 202 && st != 200 && st != 201) return false;
    }
    return true;
}

// ---------------------------------------------------------------- write a backup to the destination
static void set_status(const std::wstring &s) {
    { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_status = s; }
    ui_post([] { sync_page_refresh(); });
}
static void keep_five_folder(const std::wstring &folder) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\Pluma-*.plumabk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());   // Pluma-YYYYMMDD-HHMMSS sorts oldest first
    for (size_t i = 0; i + 5 < files.size(); i++) DeleteFileW((folder + L"\\" + files[i]).c_str());
}
// returns "" on success, else an error line for the status
static std::wstring do_backup(bool force) {
    bk::Kdf kdf; uint8_t key[32] = {};
    if (!key_remembered(kdf, key)) return T(L"Pon una contraseña para la copia.");
    std::wstring dest, folder;
    { std::lock_guard<std::mutex> lk(g_cfg_mtx); dest = g_cfg.dest; folder = g_cfg.folder; }
    if (dest.empty()) { SecureZeroMemory(key, 32); return T(L"Elige dónde guardar la copia."); }

    std::string signature;
    std::vector<bk::Entry> entries = gather(signature);
    { std::lock_guard<std::mutex> lk(g_cfg_mtx); if (!force && wide(signature) == g_cfg.signature) { SecureZeroMemory(key, 32); return L""; } }

    std::wstring hint;
    { std::lock_guard<std::mutex> lk(g_cfg_mtx); hint = g_cfg.hint; }
    std::wstring header_extra = L",\"equipo\":" + json_escape(machine_name()) + L",\"cuando\":" + json_escape(now_iso());
    if (!hint.empty()) header_extra += L",\"pista\":" + json_escape(hint);
    {   // the engine's key, without which the passwords and cookies can't be read on another Windows
        std::string k;
        if (engine_key_get(k)) entries.push_back({ "meta/clave", std::move(k) });
    }
    std::string file;
    bool sealed = bk::seal(key, bk::header_json(kdf, utf8(header_extra)), entries, file);
    SecureZeroMemory(key, 32);
    for (auto &e : entries) bk::wipe(e.data);
    if (!sealed) return T(L"No se pudo cifrar la copia.");

    std::wstring name = L"Pluma-" + stamp() + L".plumabk";
    bool ok = false;
    if (dest == L"carpeta") {
        if (folder.empty()) { bk::wipe(file); return T(L"Elige una carpeta para la copia."); }
        SHCreateDirectoryExW(NULL, folder.c_str(), NULL);
        std::wstring tmp = folder + L"\\" + name + L".tmp";
        if (write_file(tmp, file)) {
            DeleteFileW((folder + L"\\" + name).c_str());
            ok = MoveFileExW(tmp.c_str(), (folder + L"\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
            if (!ok) DeleteFileW(tmp.c_str());
        }
        if (ok) keep_five_folder(folder);
    } else {
        std::wstring err;
        std::wstring at = access_token(dest, err);
        if (at.empty()) { bk::wipe(file); return err; }
        ok = cloud_upload(dest, at, name, file);
        if (ok) {
            std::vector<Remote> have;
            if (cloud_list(dest, at, have)) {
                std::sort(have.begin(), have.end(), [](const Remote &a, const Remote &b) { return a.name < b.name; });
                for (size_t i = 0; i + 5 < have.size(); i++) cloud_delete(dest, at, have[i]);
            }
        }
    }
    long long size = (long long)file.size();
    bk::wipe(file);
    if (!ok) return T(L"No se pudo guardar la copia.");
    { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.signature = wide(signature); g_cfg.last_iso = now_iso(); g_cfg.last_size = size; config_save_locked(); }
    int n_pluma = 0, n_motor = 0, n_meta = 0;   // counts only: never names, sites or contents in the log
    for (auto &e : entries) (e.name.compare(0, 6, "pluma/") == 0 ? n_pluma : e.name.compare(0, 6, "motor/") == 0 ? n_motor : n_meta)++;
    log_line(L"copia de seguridad hecha (" + std::to_wstring(size / 1024) + L" KB; ficheros: pluma " + std::to_wstring(n_pluma) +
             L", motor " + std::to_wstring(n_motor) + L", meta " + std::to_wstring(n_meta) + L")");
    return L"";
}

// ---------------------------------------------------------------- the worker
static void run_backup(bool force, bool toast) {
    if (g_busy.exchange(true)) return;
    set_status(T(L"Copiando..."));
    std::wstring err = do_backup(force);
    g_last_run_ms = now_ms();
    set_status(err.empty() ? T(L"Copia hecha") : err);
    if (toast && err.empty()) ui_post([] { ui_toast(T(L"Copia hecha")); });
    g_busy = false;
}
static void worker_main() {
    HANDLE handles[2] = { g_stop, g_wake };
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, handles, FALSE, 60000);
        if (r == WAIT_OBJECT_0) break;   // stop
        bool forced = g_req_now.exchange(false);
        bool due = g_req_due.exchange(false);
        if (!forced && !due) {   // the 60 s tick: back up at most hourly, and only if something changed (checked inside)
            std::lock_guard<std::mutex> lk(g_cfg_mtx);
            due = g_cfg.automatic && !g_cfg.dest.empty() && key_is_set() &&
                  (g_last_run_ms == 0 || now_ms() - g_last_run_ms > 3600000.0);
        }
        if (forced || due) run_backup(forced, forced);   // forced also shows a toast
    }
}

// ---------------------------------------------------------------- public API
void sync_init() {
    config_load();
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_worker = std::thread(worker_main);
}
void sync_shutdown() {
    if (g_stop) SetEvent(g_stop);
    if (g_worker.joinable()) g_worker.join();
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }
    if (g_stop) { CloseHandle(g_stop); g_stop = NULL; }
}
void sync_on_close() {
    if (g_restoring) return;   // a restart for a restore: do not back up the old data on the way out
    std::lock_guard<std::mutex> lk(g_cfg_mtx);
    if (g_cfg.dest.empty() || !g_cfg.automatic || !key_is_set()) return;
    g_req_due = true;   // only if the content changed since the last backup (the worker checks the signature)
    if (g_wake) SetEvent(g_wake);   // the pages and history are already saved by the caller (hide_window / quit_now)
}
bool sync_wait_idle(int ms) {
    // wait for a queued backup to be picked up and for a running one to finish
    for (int i = 0; i < ms / 20 && (g_req_now || g_req_due || g_busy); i++) Sleep(20);
    return !(g_req_now || g_req_due || g_busy);
}

void sync_backup_now() { g_req_now = true; if (g_wake) SetEvent(g_wake); }
void sync_set_auto(bool on) { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.automatic = on; config_save_locked(); }
void sync_set_destination(const std::wstring &kind) {
    { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.dest = kind; config_save_locked(); }
    ui_post([] { sync_page_refresh(); });
}
bool sync_pick_folder() {
    if (g_test) {   // tests: PLUMA_SYNC_CARPETA skips the dialog (it would show on the screen and take the focus)
        wchar_t env[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"PLUMA_SYNC_CARPETA", env, MAX_PATH) && *env) {
            { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.folder = env; g_cfg.dest = L"carpeta"; config_save_locked(); }
            ui_post([] { sync_page_refresh(); });
            return true;
        }
    }
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opt = 0; dlg->GetOptions(&opt); dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(g_win))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR path = NULL;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.folder = path; g_cfg.dest = L"carpeta"; config_save_locked(); }
                CoTaskMemFree(path); ok = true;
            }
            it->Release();
        }
    }
    dlg->Release();
    if (ok) ui_post([] { sync_page_refresh(); });
    return ok;
}
void sync_set_password(const std::wstring &pw, const std::wstring &hint) {
    if (pw.empty()) {
        DeleteFileW(clave_file().c_str());
        { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.hint.clear(); config_save_locked(); }
        ui_post([] { sync_page_refresh(); });
        return;
    }
    std::wstring hintc = hint;
    std::string pwu = utf8(pw);
    std::thread([pwu, hintc]() mutable {
        set_status(T(L"Preparando la contraseña..."));
        bk::Kdf kdf;
        kdf.salt.resize(16); bk::random_bytes(&kdf.salt[0], 16);
        uint8_t key[32] = {};
        bool ok = bk::derive(pwu, kdf, key);
        SecureZeroMemory(&pwu[0], pwu.size());
        if (ok) {
            std::wstring o = L"{\"sal\":\"" + wide(bk::hex(kdf.salt)) + L"\",\"m\":" + std::to_wstring(kdf.m_kib) +
                             L",\"t\":" + std::to_wstring(kdf.t) + L",\"p\":" + std::to_wstring(kdf.p) +
                             L",\"key\":\"" + wide(bk::hex(std::string((char *)key, 32))) + L"\"}";
            SecureZeroMemory(key, 32);
            std::string plain = utf8(o), enc;
            if (dpapi_protect(plain, enc)) write_file(clave_file(), enc);
            SecureZeroMemory(&plain[0], plain.size());
        }
        { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.hint = hintc; config_save_locked(); }
        set_status(ok ? T(L"Contraseña guardada") : T(L"No se pudo preparar la contraseña."));
        if (ok) sync_backup_now();
    }).detach();
}
void sync_disconnect() {
    DeleteFileW(oauth_file().c_str());
    { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.account.clear(); if (g_cfg.dest != L"carpeta") g_cfg.dest.clear(); config_save_locked(); }
    ui_post([] { sync_page_refresh(); });
}

std::wstring sync_state_json() {
    std::lock_guard<std::mutex> lk(g_cfg_mtx);
    std::wstring o = L"{";
    o += L"\"dest\":" + json_escape(g_cfg.dest);
    o += L",\"folder\":" + json_escape(g_cfg.folder);
    o += L",\"account\":" + json_escape(g_cfg.account);
    o += L",\"auto\":" + std::wstring(g_cfg.automatic ? L"true" : L"false");
    o += L",\"hasPassword\":" + std::wstring(key_is_set() ? L"true" : L"false");
    o += L",\"hint\":" + json_escape(g_cfg.hint);
    o += L",\"last\":" + json_escape(g_cfg.last_iso);
    o += L",\"size\":" + std::to_wstring(g_cfg.last_size);
    o += L",\"status\":" + json_escape(g_status);
    o += L",\"busy\":" + std::wstring(g_busy ? L"true" : L"false");
    o += L",\"port\":" + std::to_wstring(kLoopbackPort);
    o += L"}";
    return o;
}
void sync_page_refresh() { settings_page_refresh(); }

// ---------------------------------------------------------------- OAuth sign-in (system browser, PKCE, loopback)
// url-decode an ASCII query value (%XX and '+')
static std::string urldecode_a(const std::string &s) {
    std::string o;
    auto hx = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && hx(s[i + 1]) >= 0 && hx(s[i + 2]) >= 0) { o += (char)((hx(s[i + 1]) << 4) | hx(s[i + 2])); i += 2; }
        else o += s[i];
    }
    return o;
}
// pull one exact query parameter out of a query string ("a=1&b=2"): matches the whole key, not a substring
static std::string query_param(const std::string &query, const std::string &key) {
    size_t i = 0;
    while (i < query.size()) {
        size_t amp = query.find('&', i);
        std::string pair = query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) return urldecode_a(pair.substr(eq + 1));
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return "";
}
static bool const_eq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return d == 0;
}
// waits for the provider's callback: a GET to "/" with a matching state and a code (or an error). Keeps listening
// (a preconnect or a stray request is answered and ignored) until a valid callback or the deadline.
static bool loopback_wait(const std::string &want_state, std::string &code, std::string &err) {
    WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    bool done = false;
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv != INVALID_SOCKET) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)kLoopbackPort);
        InetPtonW(AF_INET, L"127.0.0.1", &a.sin_addr);
        if (bind(srv, (sockaddr *)&a, sizeof(a)) == 0 && listen(srv, 4) == 0) {
            ULONGLONG deadline = GetTickCount64() + 180000;   // three minutes for the person to sign in
            while (!done && GetTickCount64() < deadline) {
                fd_set fds; FD_ZERO(&fds); FD_SET(srv, &fds);
                timeval tv{ 2, 0 };
                if (select(0, &fds, NULL, NULL, &tv) <= 0) continue;
                SOCKET cl = accept(srv, NULL, NULL);
                if (cl == INVALID_SOCKET) continue;
                char buf[8192]; int n = recv(cl, buf, sizeof(buf) - 1, 0);
                bool valid = false;
                if (n > 0) {
                    std::string req(buf, n);
                    // the request line: "GET /path?query HTTP/1.1"
                    if (req.compare(0, 4, "GET ") == 0) {
                        size_t sp = req.find(' ', 4);
                        std::string target = req.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
                        size_t q = target.find('?');
                        if (q != std::string::npos) {
                            std::string query = target.substr(q + 1);
                            std::string st = query_param(query, "state");
                            std::string er = query_param(query, "error");
                            std::string cd = query_param(query, "code");
                            if (!st.empty() && const_eq(st, want_state)) {
                                if (!cd.empty()) { code = cd; valid = true; done = true; }
                                else if (!er.empty()) { err = er; valid = true; done = true; }
                            }
                        }
                    }
                }
                std::string page = utf8(T(L"Ya puedes volver a Pluma."));
                std::string body = "<!doctype html><meta charset=utf-8><body style=\"font-family:system-ui;text-align:center;padding:60px\"><h2>Pluma</h2><p>" + page + "</p>";
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                                   std::to_string(body.size()) + "\r\n\r\n" + body;
                send(cl, resp.c_str(), (int)resp.size(), 0);
                closesocket(cl);
                (void)valid;
            }
        }
        closesocket(srv);
    }
    WSACleanup();
    return done && code.size() > 0;
}
void sync_connect(const std::wstring &dest) {
    std::wstring id = client_id(dest);
    if (id.empty()) { set_status(T(L"Falta el identificador de la app de la nube. Regístrala y ponlo en la configuración.")); return; }
    Provider p = provider(dest);
    if (!p.id[0]) return;
    std::thread([dest, id, p]() {
        std::string verifier_raw(32, 0); bk::random_bytes(&verifier_raw[0], 32);
        std::string verifier = base64url(verifier_raw);
        std::string challenge = base64url(sha256_raw(verifier));   // S256 = base64url(sha256(verifier))
        std::string state_raw(32, 0); bk::random_bytes(&state_raw[0], 32);   // >= 128 bits against a forged callback (CSRF)
        std::string state = base64url(state_raw);
        std::wstring redirect = loopback_uri();
        std::wstring url = std::wstring(p.auth) + L"?client_id=" + url_encode(id) + L"&response_type=code&redirect_uri=" + url_encode(redirect) +
                           L"&scope=" + url_encode(p.scope) + L"&code_challenge=" + wide(challenge) + L"&code_challenge_method=S256&state=" + wide(state);
        if (dest == L"gdrive") url += L"&access_type=offline&prompt=consent";
        if (dest == L"dropbox") url += L"&token_access_type=offline";
        set_status(T(L"Abre la ventana de inicio de sesión..."));
        ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
        std::string code, cberr;
        if (!loopback_wait(state, code, cberr)) { set_status(T(L"No se completó el inicio de sesión.")); return; }
        std::wstring bodyw = L"client_id=" + url_encode(id) + L"&grant_type=authorization_code&code=" + wide(code) +
                             L"&redirect_uri=" + url_encode(redirect) + L"&code_verifier=" + wide(verifier);
        if (dest == L"gdrive") bodyw += L"&client_secret=" + url_encode(client_secret(dest));
        DWORD st = 0; std::string resp;
        http(L"POST", p.token, L"Content-Type: application/x-www-form-urlencoded\r\n", utf8(bodyw), st, resp);
        if (st != 200) { set_status(T(L"No se pudo completar el inicio de sesión.")); return; }
        std::wstring refresh = json_str(resp, L"refresh_token");
        if (refresh.empty()) { set_status(T(L"La cuenta no dio un permiso duradero; vuelve a intentarlo.")); return; }
        token_save(dest, refresh);
        std::wstring at = json_str(resp, L"access_token");
        std::wstring who = dest;   // a best-effort account label
        if (dest == L"dropbox") { DWORD s2; std::string r2; http(L"POST", L"https://api.dropboxapi.com/2/users/get_current_account", L"Authorization: Bearer " + at + L"\r\n", "", s2, r2); who = json_parse(wide(r2))[L"email"].s; }
        else if (dest == L"onedrive") { DWORD s2; std::string r2; http(L"GET", L"https://graph.microsoft.com/v1.0/me", L"Authorization: Bearer " + at + L"\r\n", "", s2, r2); who = json_parse(wide(r2))[L"userPrincipalName"].s; }
        else if (dest == L"gdrive") { DWORD s2; std::string r2; http(L"GET", L"https://www.googleapis.com/drive/v3/about?fields=user(emailAddress)", L"Authorization: Bearer " + at + L"\r\n", "", s2, r2); who = json_parse(wide(r2))[L"user"][L"emailAddress"].s; }
        { std::lock_guard<std::mutex> lk(g_cfg_mtx); g_cfg.dest = dest; g_cfg.account = who.empty() ? provider(dest).name : who; config_save_locked(); }
        set_status(T(L"Cuenta conectada"));
        sync_backup_now();
    }).detach();
}

// ---------------------------------------------------------------- restore
std::wstring sync_pick_restore_file() {
    if (g_test) { wchar_t env[MAX_PATH] = L""; if (GetEnvironmentVariableW(L"PLUMA_SYNC_FICHERO", env, MAX_PATH) && *env) return env; }
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_win;
    ofn.lpstrFilter = L"Pluma\0*.plumabk\0\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
    return GetOpenFileNameW(&ofn) ? std::wstring(file) : L"";
}
std::wstring sync_inspect_json(const std::wstring &path) {
    std::string d;
    if (!read_file(path, d)) return L"{\"ok\":false}";
    bk::Header h;
    if (!bk::read_header(d, h)) return L"{\"ok\":false}";
    std::wstring o = L"{\"ok\":true";
    o += L",\"when\":" + json_escape(h.j[L"cuando"].s);
    o += L",\"equipo\":" + json_escape(h.j[L"equipo"].s);
    o += L",\"hint\":" + json_escape(h.j[L"pista"].s);
    o += L",\"size\":" + std::to_wstring((long long)d.size());
    o += L"}";
    return o;
}
// a backup entry name -> its path under the data folder (backslashes); "" = not restored as a file
static std::wstring target_rel(const std::string &name) {
    if (name.compare(0, 6, "pluma/") == 0) return back_slashes(name.substr(6));
    if (name.compare(0, 6, "motor/") == 0) return L"webview2\\EBWebView\\" + back_slashes(name.substr(6));
    return L"";   // meta/clave: staged apart (below), not as a profile file
}
static void make_parent(const std::wstring &full) {
    size_t cut = full.find_last_of(L'\\');
    if (cut != std::wstring::npos) SHCreateDirectoryExW(NULL, full.substr(0, cut).c_str(), NULL);
}
static void rmtree(const std::wstring &dir) {
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::vector<wchar_t> buf(dir.begin(), dir.end()); buf.push_back(0); buf.push_back(0);
    SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = buf.data(); op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}
bool sync_has_pending_restore() {
    return GetFileAttributesW((g_state + L"\\restore-pending\\APLICAR").c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void post_restore_result(int code) {
    std::wstring m = L"{\"op\":\"restoreResult\",\"code\":" + std::to_wstring(code) + L"}";
    ui_post([m] { settings_broadcast(m); });
}
// 0 ok (restarts), 1 wrong password, 2 damaged, 3 not a backup, 4 error (reported via restoreResult)
void sync_restore(const std::wstring &path, const std::wstring &password) {
    std::wstring p = path, pw = password;
    std::thread([p, pw]() {
        std::string d;
        if (!read_file(p, d)) { post_restore_result(3); return; }
        bk::Header h;
        if (!bk::read_header(d, h)) { post_restore_result(3); return; }
        uint8_t key[32] = {};
        if (!bk::derive(utf8(pw), h.kdf, key)) { post_restore_result(4); return; }
        std::vector<bk::Entry> entries;
        bk::OpenResult r = bk::open(d, h, key, entries);
        SecureZeroMemory(key, 32);
        if (r == bk::OPEN_WRONG_PASSWORD) { post_restore_result(1); return; }   // nothing written on a wrong password
        if (r != bk::OPEN_OK) { post_restore_result(2); return; }
        g_restoring = true;
        std::wstring dir = g_state + L"\\restore-pending";
        rmtree(dir);   // a failed earlier attempt must not linger
        std::wstring files = dir + L"\\files";
        SHCreateDirectoryExW(NULL, files.c_str(), NULL);
        bool ok = true;
        for (auto &e : entries) {
            if (e.name == "meta/clave") {   // never in clear on disk: guarded for this account until it is applied
                DATA_BLOB in{ (DWORD)e.data.size(), (BYTE *)e.data.data() }, prot{};
                if (CryptProtectData(&in, NULL, NULL, NULL, NULL, 0, &prot)) {
                    if (!write_file(dir + L"\\clave.bin", std::string((const char *)prot.pbData, prot.cbData))) ok = false;
                    LocalFree(prot.pbData);
                } else ok = false;
                bk::wipe(e.data);
                continue;
            }
            std::wstring rel = target_rel(e.name);
            if (rel.empty()) continue;
            std::wstring full = files + L"\\" + rel;
            make_parent(full);
            if (!write_file(full, e.data)) ok = false;
            bk::wipe(e.data);
        }
        if (!ok) { rmtree(dir); g_restoring = false; post_restore_result(4); return; }
        write_file(dir + L"\\APLICAR", "1");
        log_line(L"restauración preparada: " + std::to_wstring((int)entries.size()) + L" ficheros; reiniciando");
        post_restore_result(0);
        sync_wait_idle(8000);   // let a backup in flight finish before the restart
        ui_post([] { app_restart(false); });   // app_restart touches the window: do it on its thread
    }).detach();
}
// move one root (a file or a whole folder, relative to the data folder) from staging into place, the live one aside
// the Pluma that restarted for this restore may still be closing: its engine lets go of its files a moment later
static bool move_retry(const std::wstring &from, const std::wstring &to) {
    for (int i = 0; i < 75; i++) {   // up to 15 s
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return true;
        DWORD e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION) return false;
        Sleep(200);
    }
    return false;
}
static int apply_root(const std::wstring &staged_files, const std::wstring &rel, const std::wstring &aside) {
    std::wstring from = staged_files + L"\\" + rel;
    if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;   // not in this backup
    std::wstring live = g_state + L"\\" + rel;
    if (GetFileAttributesW(live.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring keep = aside + L"\\" + rel;
        make_parent(keep);
        if (!move_retry(live, keep)) {   // the live one stays as it was; the staged one waits for a look
            log_line(L"restauración: no se pudo apartar " + rel + L" (error " + std::to_wstring(GetLastError()) + L")");
            return 1;
        }
    }
    make_parent(live);
    if (move_retry(from, live)) return 0;
    log_line(L"restauración: no se pudo poner " + rel + L" (error " + std::to_wstring(GetLastError()) + L")");
    return 1;
}
bool sync_apply_restore() {
    std::wstring dir = g_state + L"\\restore-pending";
    if (GetFileAttributesW((dir + L"\\APLICAR").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring aside = g_state + L"\\antes-de-restaurar-" + stamp();
    SHCreateDirectoryExW(NULL, aside.c_str(), NULL);
    std::wstring files = dir + L"\\files";
    int fails = 0;
    // each root is moved whole, so leveldb folders are never merged and stale items do not survive the restore
    for (auto &f : bk::pluma_files()) fails += apply_root(files, wide(f), aside);
    for (auto &d : bk::pluma_dirs()) fails += apply_root(files, wide(d), aside);
    for (auto &prof : engine_profiles(files + L"\\webview2\\EBWebView")) {
        std::wstring rp = L"webview2\\EBWebView\\" + wide(prof) + L"\\";
        // a database coming back takes the place of the live one with its journals: stale ones would be replayed on it
        for (auto &db : bk::engine_dbs()) {
            if (GetFileAttributesW((files + L"\\" + rp + back_slashes(db)).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            for (const wchar_t *suf : { L"-journal", L"-wal", L"-shm" }) {
                std::wstring rel = rp + back_slashes(db) + suf;
                std::wstring live = g_state + L"\\" + rel;
                if (GetFileAttributesW(live.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                if (GetFileAttributesW((files + L"\\" + rel).c_str()) != INVALID_FILE_ATTRIBUTES) continue;   // apply_root moves it
                std::wstring keep = aside + L"\\" + rel;
                make_parent(keep);
                if (!move_retry(live, keep)) fails++;
            }
        }
        for (auto &p : bk::engine_paths()) fails += apply_root(files, rp + back_slashes(p), aside);
    }
    // the engine's key, guarded again for this Windows account: the passwords and cookies above open with it
    std::string prot;
    if (read_file(dir + L"\\clave.bin", prot)) {
        DATA_BLOB in{ (DWORD)prot.size(), (BYTE *)prot.data() }, key{};
        if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &key)) {
            if (!engine_key_put(std::string((const char *)key.pbData, key.cbData), aside)) {
                fails++;
                log_line(L"restauración: no se pudo poner la llave del motor");
            }
            SecureZeroMemory(key.pbData, key.cbData);
            LocalFree(key.pbData);
        } else fails++;
    }
    DeleteFileW((dir + L"\\APLICAR").c_str());
    if (fails == 0) rmtree(dir);   // keep the staging area on any failure, for a look
    log_line(L"restauración aplicada (" + std::to_wstring(fails) + L" fallos); copia de lo anterior en " + aside);
    return true;
}
