// Pluma: import saved passwords from a CSV the other browser exports (name,url,username,password; Firefox and Bitwarden
// headers too). The browsers store their own passwords app-bound, so Pluma cannot read them; the user exports a CSV and
// Pluma writes it into its own engine's store, encrypted the way the engine expects (v10: AES-256-GCM with the key in
// Pluma's Local State, itself protected by the Windows account via DPAPI).
//
// Writing happens with the engine stopped: the rows are encrypted at once and left in a pending file, and inserted into
// Login Data at the next start, before the engine opens it. Login Data is backed up first and duplicates are skipped.
// No password, username or site is ever logged or printed.
#include "pluma.h"
#include "json.h"
#include <ctime>
#include <wincrypt.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <set>

// ---------------------------------------------------------------- winsqlite3 (loaded at run time), with binding
namespace pq {
typedef int (*open_t)(const char *, void **, int, const char *);
typedef int (*prep_t)(void *, const char *, int, void **, const char **);
typedef int (*step_t)(void *);
typedef int (*btext_t)(void *, int, const char *, int, void *);
typedef int (*bblob_t)(void *, int, const void *, int, void *);
typedef int (*bi64_t)(void *, int, long long);
typedef const unsigned char *(*ctext_t)(void *, int);
typedef int (*ccount_t)(void *);
typedef int (*fin_t)(void *);
typedef int (*close_t)(void *);
static open_t open; static prep_t prepare; static step_t step; static btext_t bind_text; static bblob_t bind_blob;
static bi64_t bind_i64; static ctext_t ctext; static ccount_t ccount; static fin_t finalize; static close_t close;
static bool load() {
    static int s = 0;
    if (s) return s == 1;
    HMODULE h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) { s = -1; return false; }
    open = (open_t)GetProcAddress(h, "sqlite3_open_v2");
    prepare = (prep_t)GetProcAddress(h, "sqlite3_prepare_v2");
    step = (step_t)GetProcAddress(h, "sqlite3_step");
    bind_text = (btext_t)GetProcAddress(h, "sqlite3_bind_text");
    bind_blob = (bblob_t)GetProcAddress(h, "sqlite3_bind_blob");
    bind_i64 = (bi64_t)GetProcAddress(h, "sqlite3_bind_int64");
    ctext = (ctext_t)GetProcAddress(h, "sqlite3_column_text");
    ccount = (ccount_t)GetProcAddress(h, "sqlite3_column_count");
    finalize = (fin_t)GetProcAddress(h, "sqlite3_finalize");
    close = (close_t)GetProcAddress(h, "sqlite3_close");
    s = (open && prepare && step && bind_text && bind_blob && bind_i64 && ctext && ccount && finalize && close) ? 1 : -1;
    return s == 1;
}
}
static const int SQLITE_TRANSIENT = -1;

// ---------------------------------------------------------------- the engine's master key (Local State -> DPAPI)
static bool master_key(std::vector<BYTE> &out) {
    std::string ls;
    if (!read_file(g_state + L"\\webview2\\EBWebView\\Local State", ls)) return false;
    std::wstring b64 = json_parse(wide(ls))[L"os_crypt"][L"encrypted_key"].s;
    if (b64.empty()) return false;
    std::string b64a = utf8(b64);
    DWORD raw = 0;
    if (!CryptStringToBinaryA(b64a.c_str(), (DWORD)b64a.size(), CRYPT_STRING_BASE64, NULL, &raw, NULL, NULL)) return false;
    std::vector<BYTE> enc(raw);
    if (!CryptStringToBinaryA(b64a.c_str(), (DWORD)b64a.size(), CRYPT_STRING_BASE64, enc.data(), &raw, NULL, NULL)) return false;
    // strip the "DPAPI" prefix, then unprotect with the Windows account
    const BYTE pfx[5] = { 'D', 'P', 'A', 'P', 'I' };
    if (enc.size() <= 5 || memcmp(enc.data(), pfx, 5) != 0) return false;
    DATA_BLOB in{ (DWORD)(enc.size() - 5), enc.data() + 5 }, keyb{};
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &keyb)) return false;
    out.assign(keyb.pbData, keyb.pbData + keyb.cbData);
    LocalFree(keyb.pbData);
    return out.size() == 32;
}
// v10 = "v10" + 12-byte nonce + AES-256-GCM ciphertext + 16-byte tag
static bool encrypt_v10(const std::vector<BYTE> &key, const std::string &plain, std::vector<BYTE> &out) {
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_KEY_HANDLE k = NULL;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) return false;
    do {
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) break;
        if (BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0) break;
        BYTE nonce[12], tag[16];
        if (BCryptGenRandom(NULL, nonce, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) break;
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce; info.cbNonce = 12; info.pbTag = tag; info.cbTag = 16;
        std::vector<BYTE> ct(plain.size());
        ULONG done = 0;
        if (BCryptEncrypt(k, (PUCHAR)plain.data(), (ULONG)plain.size(), &info, NULL, 0, ct.data(), (ULONG)ct.size(), &done, 0) != 0) break;
        out.assign({ 'v', '1', '0' });
        out.insert(out.end(), nonce, nonce + 12);
        out.insert(out.end(), ct.begin(), ct.begin() + done);
        out.insert(out.end(), tag, tag + 16);
        ok = true;
    } while (0);
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---------------------------------------------------------------- CSV (RFC 4180-ish), mapped by header name
static std::vector<std::vector<std::wstring>> parse_csv(const std::wstring &text) {
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> row;
    std::wstring field;
    bool quoted = false;
    auto endf = [&] { row.push_back(field); field.clear(); };
    auto endr = [&] { endf(); rows.push_back(row); row.clear(); };
    for (size_t i = 0; i < text.size(); i++) {
        wchar_t c = text[i];
        if (quoted) {
            if (c == L'"') { if (i + 1 < text.size() && text[i + 1] == L'"') { field += L'"'; i++; } else quoted = false; }
            else field += c;
        } else {
            if (c == L'"') quoted = true;
            else if (c == L',') endf();
            else if (c == L'\r') {}
            else if (c == L'\n') endr();
            else field += c;
        }
    }
    if (!field.empty() || !row.empty()) endr();
    return rows;
}
struct Cred { std::wstring url, user, pass; };
static int col_of(const std::vector<std::wstring> &head, std::initializer_list<const wchar_t *> names) {
    for (size_t i = 0; i < head.size(); i++) { std::wstring h = lower(trim(head[i])); for (auto n : names) if (h == n) return (int)i; }
    return -1;
}
static std::vector<Cred> read_creds(const std::wstring &path, bool &header_ok) {
    std::vector<Cred> out; header_ok = false;
    std::string data;
    if (!read_file(path, data)) return out;
    std::wstring w = wide(data);
    if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
    auto rows = parse_csv(w);
    if (rows.empty()) return out;
    const auto &h = rows[0];
    int iu = col_of(h, { L"url", L"login_uri", L"formactionorigin" });
    int in = col_of(h, { L"username", L"login_username" });
    int ip = col_of(h, { L"password", L"login_password" });
    if (iu < 0 || ip < 0) return out;
    header_ok = true;
    for (size_t r = 1; r < rows.size(); r++) {
        const auto &f = rows[r];
        if ((int)f.size() <= iu || (int)f.size() <= ip) continue;
        Cred c;
        c.url = trim(f[iu]);
        c.user = in >= 0 && (int)f.size() > in ? f[in] : L"";
        c.pass = f[ip];
        if (!c.url.empty() && !c.pass.empty()) out.push_back(c);
    }
    return out;
}
static std::wstring realm_of(const std::wstring &url) {
    std::wstring h = url_host(url);
    if (h.empty()) return L"";
    bool https = !starts_with(lower(url), L"http://");
    return (https ? L"https://" : L"http://") + h + L"/";
}

// ---------------------------------------------------------------- the pending file (ready rows, no plaintext)
static std::wstring pending_file() { return g_data + L"\\passwords-pending.tsv"; }
static std::wstring hex_of(const std::vector<BYTE> &b) {
    static const wchar_t *d = L"0123456789abcdef";
    std::wstring s; s.reserve(b.size() * 2);
    for (BYTE x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}
static std::vector<BYTE> unhex(const std::wstring &s) {
    std::vector<BYTE> b; b.reserve(s.size() / 2);
    auto v = [](wchar_t c) -> int { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : 0; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back((BYTE)((v(s[i]) << 4) | v(s[i + 1])));
    return b;
}
static std::wstring tsv_safe(const std::wstring &s) { std::wstring o = s; for (auto &c : o) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' '; return o; }

// what Login Data already holds (realm + user), read from a copy so a locked file can be read
static std::set<std::wstring> existing_logins(const std::wstring &login_data) {
    std::set<std::wstring> set;
    if (!pq::load()) return set;
    std::wstring dir = g_data + L"\\imp_tmp"; CreateDirectoryW(dir.c_str(), NULL);
    std::wstring copy = dir + L"\\ld-" + std::to_wstring(GetTickCount64()) + L".db";
    std::string d;
    if (!read_file(login_data, d) || !write_file(copy, d)) return set;
    std::string wal; if (read_file(login_data + L"-wal", wal)) write_file(copy + L"-wal", wal);
    void *db = nullptr;
    if (pq::open(utf8(copy).c_str(), &db, 2, nullptr) == 0 && db) {
        void *st = nullptr;
        if (pq::prepare(db, "select signon_realm, username_value from logins", -1, &st, nullptr) == 0 && st) {
            while (pq::step(st) == 100) {
                const unsigned char *a = pq::ctext(st, 0), *b = pq::ctext(st, 1);
                set.insert(wide(std::string(a ? (const char *)a : "")) + L"\n" + wide(std::string(b ? (const char *)b : "")));
            }
            pq::finalize(st);
        }
        pq::close(db);
    }
    DeleteFileW(copy.c_str()); DeleteFileW((copy + L"-wal").c_str());
    return set;
}

// overwrite a file with zeros, then delete it (so an exported CSV of plain passwords does not linger)
static void shred(const std::wstring &path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz; if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64 << 20)) {
            std::vector<BYTE> z((size_t)sz.QuadPart, 0); DWORD w; WriteFile(f, z.data(), (DWORD)z.size(), &w, NULL); FlushFileBuffers(f);
        }
        CloseHandle(f);
    }
    DeleteFileW(path.c_str());
}

static std::wstring g_last_csv;   // the CSV just read, so the page can offer to delete it

// import a chosen CSV: encrypt the new rows now, leave them pending for the next start
static ImportResult import_file(const std::wstring &path) {
    ImportResult r;
    if (g_incognito) { r.note = T(L"No disponible en una ventana de incógnito."); return r; }   // nothing is kept from there
    std::vector<BYTE> key;
    if (!master_key(key)) { r.note = T(L"No se pudo preparar el almacén de contraseñas de Pluma."); return r; }
    bool header_ok = false;
    auto creds = read_creds(path, header_ok);
    if (!header_ok) { r.note = T(L"El CSV no tiene las columnas esperadas (url, username, password)."); return r; }
    auto have = existing_logins(wv_profile_dir() + L"\\Login Data");
    std::wstring out;
    for (auto &c : creds) {
        std::wstring realm = realm_of(c.url);
        if (realm.empty()) continue;
        if (have.count(realm + L"\n" + c.user)) continue;   // skip duplicates
        std::vector<BYTE> blob;
        if (!encrypt_v10(key, utf8(c.pass), blob)) continue;
        out += tsv_safe(realm) + L"\t" + tsv_safe(c.url) + L"\t" + tsv_safe(c.user) + L"\t" + hex_of(blob) + L"\n";
        have.insert(realm + L"\n" + c.user);
        r.passwords++;
    }
    if (r.passwords > 0) {
        std::string existing; read_file(pending_file(), existing);
        write_file(pending_file(), existing + utf8(out));
    }
    g_last_csv = path;
    return r;
}

bool passwords_delete_csv() {
    if (g_last_csv.empty()) return false;
    shred(g_last_csv);
    bool gone = GetFileAttributesW(g_last_csv.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (gone) g_last_csv.clear();
    return gone;
}

// ---------------------------------------------------------------- wizard entry points (UI thread)
void passwords_open_edge_export() {
    // the user exports from Edge; Pluma only opens the page
    if ((INT_PTR)ShellExecuteW(NULL, L"open", L"msedge.exe", L"edge://settings/passwords", NULL, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(NULL, L"open", L"microsoft-edge:edge://settings/passwords", NULL, NULL, SW_SHOWNORMAL);
}
ImportResult passwords_import_pick() {
    if (const wchar_t *t = _wgetenv(L"PLUMA_CSV")) if (*t) return import_file(t);   // tests: skip the file dialog
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_win;
    ofn.lpstrFilter = L"CSV\0*.csv\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
    ofn.lpstrTitle = T(L"Elige el CSV de contraseñas");
    ImportResult r;
    if (!GetOpenFileNameW(&ofn)) { r.note = L""; r.passwords = -1; return r; }   // -1: cancelled, no message
    return import_file(file);
}

// ---------------------------------------------------------------- applied at the next start, engine not yet running
static void build_insert(void *db, const std::wstring &realm, const std::wstring &url, const std::wstring &user, const std::vector<BYTE> &blob, long long now, int &added) {
    // learn the columns, so the INSERT fits whatever schema this engine's Login Data has
    std::vector<std::wstring> cols, types; std::vector<int> notnull, hasdef;
    void *st = nullptr;
    if (pq::prepare(db, "pragma table_info(logins)", -1, &st, nullptr) != 0 || !st) return;
    while (pq::step(st) == 100) {
        const unsigned char *n = pq::ctext(st, 1), *t = pq::ctext(st, 2), *nn = pq::ctext(st, 3), *df = pq::ctext(st, 4);
        cols.push_back(wide(std::string(n ? (const char *)n : "")));
        types.push_back(lower(wide(std::string(t ? (const char *)t : ""))));
        notnull.push_back(nn && atoi((const char *)nn));
        hasdef.push_back(df != nullptr);
    }
    pq::finalize(st);
    auto known = [&](const std::wstring &c) {
        return c == L"origin_url" || c == L"action_url" || c == L"signon_realm" || c == L"username_value" ||
               c == L"password_value" || c == L"date_created" || c == L"date_last_used" || c == L"date_password_modified" ||
               c == L"times_used" || c == L"id";
    };
    std::string sqlc, sqlv; std::vector<int> use;
    for (size_t i = 0; i < cols.size(); i++) {
        if (cols[i] == L"id") continue;                       // auto
        if (!known(cols[i]) && hasdef[i]) continue;           // let the default stand
        if (!known(cols[i]) && !notnull[i]) continue;         // nullable: leave it out
        use.push_back((int)i);
        sqlc += (sqlc.empty() ? "" : ",") + utf8(cols[i]);
        sqlv += (sqlv.empty() ? "" : ",") + std::string("?");
    }
    std::string sql = "insert into logins (" + sqlc + ") values (" + sqlv + ")";
    if (pq::prepare(db, sql.c_str(), -1, &st, nullptr) != 0 || !st) return;
    int p = 1;
    for (int idx : use) {
        const std::wstring &c = cols[idx]; const std::wstring &ty = types[idx];
        if (c == L"password_value") pq::bind_blob(st, p, blob.data(), (int)blob.size(), (void *)(INT_PTR)SQLITE_TRANSIENT);
        else if (c == L"origin_url" || c == L"action_url") { std::string v = utf8(url); pq::bind_text(st, p, v.c_str(), (int)v.size(), (void *)(INT_PTR)SQLITE_TRANSIENT); }
        else if (c == L"signon_realm") { std::string v = utf8(realm); pq::bind_text(st, p, v.c_str(), (int)v.size(), (void *)(INT_PTR)SQLITE_TRANSIENT); }
        else if (c == L"username_value") { std::string v = utf8(user); pq::bind_text(st, p, v.c_str(), (int)v.size(), (void *)(INT_PTR)SQLITE_TRANSIENT); }
        else if (c == L"date_created" || c == L"date_last_used" || c == L"date_password_modified") pq::bind_i64(st, p, now);
        else if (c == L"times_used") pq::bind_i64(st, p, 1);
        else if (ty.find(L"int") != std::wstring::npos) pq::bind_i64(st, p, 0);
        else if (ty.find(L"blob") != std::wstring::npos) pq::bind_blob(st, p, "", 0, (void *)(INT_PTR)SQLITE_TRANSIENT);
        else pq::bind_text(st, p, "", 0, (void *)(INT_PTR)SQLITE_TRANSIENT);
        p++;
    }
    if (pq::step(st) == 101 /* DONE */) added++;
    pq::finalize(st);
}
int passwords_apply_pending() {
    if (g_incognito) return 0;   // the ordinary window's engine may have Login Data open; it applies them itself
    std::string data;
    if (!read_file(pending_file(), data)) return 0;
    std::wstring ld = wv_profile_dir() + L"\\Login Data";
    if (!PathFileExistsW(ld.c_str())) return 0;   // the engine has not made it yet: try again next start
    if (!pq::load()) return 0;
    // back up Login Data first
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[32]; swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring bdir = g_data + L"\\passwords-backup-" + stamp;
    CreateDirectoryW(bdir.c_str(), NULL);
    for (const wchar_t *suf : { L"", L"-journal", L"-wal" }) {
        std::string s; if (read_file(ld + suf, s)) write_file(bdir + L"\\Login Data" + suf, s);
    }
    void *db = nullptr;
    if (pq::open(utf8(ld).c_str(), &db, 2 /* READWRITE */, nullptr) != 0 || !db) { if (db) pq::close(db); return 0; }
    auto have = existing_logins(ld);
    int added = 0;
    long long now = ((long long)time(NULL) + 11644473600LL) * 1000000LL;
    std::wstring w = wide(data);
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        std::vector<std::wstring> f; size_t a = 0;
        while (true) { size_t tb = line.find(L'\t', a); f.push_back(line.substr(a, tb == std::wstring::npos ? std::wstring::npos : tb - a)); if (tb == std::wstring::npos) break; a = tb + 1; }
        if (f.size() < 4 || f[0].empty()) continue;
        if (have.count(f[0] + L"\n" + f[2])) continue;
        build_insert(db, f[0], f[1], f[2], unhex(f[3]), now, added);
        have.insert(f[0] + L"\n" + f[2]);
    }
    pq::close(db);
    DeleteFileW(pending_file().c_str());
    if (added) log_line(L"contraseñas importadas: " + std::to_wstring(added));
    return added;
}
