// Pluma: import bookmarks and history from another browser. Chromium ones (Edge, Brave, Vivaldi, Opera) keep bookmarks
// in a JSON file and history in an SQLite database; Firefox keeps both in SQLite (places.sqlite). Chrome is left out on
// purpose. The history file may be in use, so it is copied first (shared read) and the copy is read.
//
// SQLite is read with winsqlite3.dll, which ships with Windows and is loaded at run time (no library to bundle).
//
// Passwords: the person's browsers store them "app-bound" (v20), tied to that browser, so Pluma cannot read or write
// them. passwords_store_kind() reports whether Pluma's own engine would even accept written passwords (it does not when
// it is app-bound). So this import does bookmarks and history only; passwords are reported, not moved.
#include "pluma.h"
#include "json.h"
#include <ctime>
#include <tlhelp32.h>

// ---------------------------------------------------------------- a thin reader over winsqlite3.dll
namespace sq {
typedef int (WINAPI *open_t)(const char *, void **, int, const char *);
typedef int (WINAPI *prep_t)(void *, const char *, int, void **, const char **);
typedef int (WINAPI *step_t)(void *);
typedef const unsigned char *(WINAPI *text_t)(void *, int);
typedef long long (WINAPI *i64_t)(void *, int);
typedef int (WINAPI *cnt_t)(void *);
typedef int (WINAPI *fin_t)(void *);
typedef int (WINAPI *close_t)(void *);
static open_t open; static prep_t prepare; static step_t step; static text_t ctext; static i64_t ci64;
static cnt_t ccount; static fin_t finalize; static close_t close;
static bool load() {
    static int state = 0;   // 0 untried, 1 ok, -1 failed
    if (state) return state == 1;
    HMODULE h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) { state = -1; return false; }
    open = (open_t)GetProcAddress(h, "sqlite3_open_v2");
    prepare = (prep_t)GetProcAddress(h, "sqlite3_prepare_v2");
    step = (step_t)GetProcAddress(h, "sqlite3_step");
    ctext = (text_t)GetProcAddress(h, "sqlite3_column_text");
    ci64 = (i64_t)GetProcAddress(h, "sqlite3_column_int64");
    ccount = (cnt_t)GetProcAddress(h, "sqlite3_column_count");
    finalize = (fin_t)GetProcAddress(h, "sqlite3_finalize");
    close = (close_t)GetProcAddress(h, "sqlite3_close");
    state = (open && prepare && step && ctext && ci64 && ccount && finalize && close) ? 1 : -1;
    return state == 1;
}
}
// each row as its text columns (int64 columns come as their decimal text, which is enough here)
static std::vector<std::vector<std::wstring>> sql_rows(const std::wstring &db_path, const char *query) {
    std::vector<std::vector<std::wstring>> out;
    if (!sq::load()) return out;
    void *db = nullptr;
    if (sq::open(utf8(db_path).c_str(), &db, 2 /* READWRITE, on a throwaway copy */, nullptr) != 0 || !db) { if (db) sq::close(db); return out; }
    void *st = nullptr;
    if (sq::prepare(db, query, -1, &st, nullptr) == 0 && st) {
        int n = sq::ccount(st);
        while (sq::step(st) == 100 /* ROW */) {
            std::vector<std::wstring> row;
            for (int i = 0; i < n; i++) {
                const unsigned char *t = sq::ctext(st, i);
                if (t) row.push_back(wide(std::string((const char *)t)));
                else row.push_back(std::to_wstring(sq::ci64(st, i)));
            }
            out.push_back(std::move(row));
        }
        sq::finalize(st);
    }
    sq::close(db);
    return out;
}

// copy a database (and its -wal) to a throwaway file, so a database in use can be read
static std::wstring copy_db(const std::wstring &src) {
    std::wstring dir = g_data + L"\\imp_tmp";
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring dst = dir + L"\\" + std::to_wstring(GetTickCount64()) + L".db";
    std::string data;
    if (!read_file(src, data)) return L"";
    if (!write_file(dst, data)) return L"";
    std::string wal;
    if (read_file(src + L"-wal", wal)) write_file(dst + L"-wal", wal);
    return dst;
}
static void drop_db(const std::wstring &dst) {
    if (dst.empty()) return;
    DeleteFileW(dst.c_str());
    DeleteFileW((dst + L"-wal").c_str());
}

static bool proc_running(const wchar_t *exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) do { if (!_wcsicmp(pe.szExeFile, exe)) { found = true; break; } } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------- Chromium bookmarks (a JSON file)
static int count_json_urls(const Json &node) {
    int n = 0;
    if (node[L"type"].s == L"url") return 1;
    const Json &kids = node[L"children"];
    if (kids.kind == Json::ARR) for (auto &c : kids.arr) n += count_json_urls(c);
    return n;
}
static int chromium_bookmark_count(const std::wstring &profile) {
    std::string data;
    if (!read_file(profile + L"\\Bookmarks", data)) return -1;
    Json j = json_parse(wide(data));
    const Json &roots = j[L"roots"];
    if (roots.kind != Json::OBJ) return 0;
    int n = 0;
    for (auto &kv : roots.obj) n += count_json_urls(kv.second);
    return n;
}
// build a Pluma subtree from a Chromium bookmark node
static void chromium_build(const Json &node, BmNode &into, int &added) {
    const Json &kids = node[L"children"];
    if (kids.kind != Json::ARR) return;
    for (auto &c : kids.arr) {
        if (c[L"type"].s == L"url") {
            BmNode b; b.id = bm_new_id(); b.url = c[L"url"].s; b.title = c[L"name"].s; b.added = (long long)time(NULL);
            if (!b.url.empty()) { into.kids.push_back(b); added++; }
        } else if (c[L"type"].s == L"folder") {
            BmNode f; f.id = bm_new_id(); f.folder = true; f.title = c[L"name"].s.empty() ? L"Carpeta" : c[L"name"].s; f.added = (long long)time(NULL);
            chromium_build(c, f, added);
            if (!f.kids.empty()) into.kids.push_back(f);
        }
    }
}

// ---------------------------------------------------------------- detection
static void add_chromium(std::vector<ImportProfile> &out, const std::wstring &browser, const std::wstring &root, const wchar_t *exe, bool single) {
    if (!PathFileExistsW(root.c_str())) return;
    bool running = proc_running(exe);
    auto one = [&](const std::wstring &dir, const std::wstring &name, const std::wstring &account = L"") {
        if (!PathFileExistsW(dir.c_str())) return;
        ImportProfile p; p.browser = browser; p.name = name; p.account = account; p.path = dir; p.running = running;
        p.bookmarks = chromium_bookmark_count(dir);
        p.history = -1;
        p.passwords = -1;
        p.extensions = ext_import_count(dir);
        if (p.bookmarks > 0 || PathFileExistsW((dir + L"\\History").c_str())) out.push_back(p);
    };
    if (single) { one(root, browser); return; }
    // the display names live in Local State (profile.info_cache)
    std::string ls;
    Json info;
    if (read_file(root + L"\\Local State", ls)) info = json_parse(wide(ls))[L"profile"][L"info_cache"];
    auto name_of = [&](const std::wstring &dir) -> std::wstring {
        if (info.kind == Json::OBJ) for (auto &kv : info.obj) if (kv.first == dir) { std::wstring n = kv.second[L"name"].s; if (!n.empty()) return n; }
        return dir;
    };
    // the account signed in, if any: what Edge shows under the name (and the better name when the profile kept its
    // default one, "Perfil 1")
    auto account_of = [&](const std::wstring &dir) -> std::wstring {
        if (info.kind == Json::OBJ) for (auto &kv : info.obj) if (kv.first == dir) return kv.second[L"user_name"].s;
        return L"";
    };
    auto default_named = [&](const std::wstring &dir) -> bool {
        if (info.kind == Json::OBJ) for (auto &kv : info.obj) if (kv.first == dir) return kv.second[L"is_using_default_name"].b;
        return false;
    };
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring d = fd.cFileName;
        if (d != L"Default" && !starts_with(d, L"Profile ")) continue;
        std::wstring acc = account_of(d);
        one(root + L"\\" + d, default_named(d) && !acc.empty() ? acc : name_of(d), acc);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void add_firefox(std::vector<ImportProfile> &out) {
    wchar_t ap[MAX_PATH]; if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ap))) return;
    std::wstring base = std::wstring(ap) + L"\\Mozilla\\Firefox";
    std::string ini;
    if (!read_file(base + L"\\profiles.ini", ini)) return;
    bool running = proc_running(L"firefox.exe");
    std::wstring w = wide(ini);
    std::wstring name, path; bool rel = true;
    auto flush = [&]() {
        if (path.empty()) return;
        std::wstring dir = rel ? base + L"\\" + path : path;
        for (auto &c : dir) if (c == L'/') c = L'\\';
        if (!PathFileExistsW((dir + L"\\places.sqlite").c_str())) return;
        ImportProfile p; p.browser = L"Firefox"; p.name = name.empty() ? L"Firefox" : name; p.path = dir; p.firefox = true; p.running = running;
        p.bookmarks = -1; p.history = -1; p.passwords = PathFileExistsW((dir + L"\\logins.json").c_str()) ? -1 : 0;
        out.push_back(p);
    };
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring line = trim(w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        if (starts_with(line, L"[Profile")) { flush(); name.clear(); path.clear(); rel = true; }
        else if (starts_with(line, L"Name=")) name = line.substr(5);
        else if (starts_with(line, L"Path=")) path = line.substr(5);
        else if (starts_with(line, L"IsRelative=")) rel = line.substr(11) != L"0";
    }
    flush();
}
std::vector<ImportProfile> import_scan() {
    std::vector<ImportProfile> out;
    wchar_t lp[MAX_PATH], ap[MAX_PATH];
    bool okl = SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, lp));
    bool oka = SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ap));
    std::wstring L = okl ? lp : L"", A = oka ? ap : L"";
    // tests: a copy of Edge's folder instead of the real one (PLUMA_EDGE_DIR), and no other browser
    wchar_t edge_copy[MAX_PATH] = L"";
    if (g_test && GetEnvironmentVariableW(L"PLUMA_EDGE_DIR", edge_copy, MAX_PATH) > 0) {
        add_chromium(out, L"Edge", edge_copy, L"msedge-copia-de-prueba.exe", false);
        return out;
    }
    if (okl) {
        add_chromium(out, L"Edge", L + L"\\Microsoft\\Edge\\User Data", L"msedge.exe", false);
        add_chromium(out, L"Brave", L + L"\\BraveSoftware\\Brave-Browser\\User Data", L"brave.exe", false);
        add_chromium(out, L"Vivaldi", L + L"\\Vivaldi\\User Data", L"vivaldi.exe", false);
    }
    if (oka) {
        add_chromium(out, L"Opera", A + L"\\Opera Software\\Opera Stable", L"opera.exe", true);
        add_chromium(out, L"Opera GX", A + L"\\Opera Software\\Opera GX Stable", L"opera.exe", true);
    }
    add_firefox(out);
    return out;
}

// ---------------------------------------------------------------- doing it
static long long chrome_to_unix(const std::wstring &micros) {
    long long t = _wtoi64(micros.c_str());
    if (t <= 0) return 0;
    return t / 1000000 - 11644473600LL;   // microseconds since 1601 -> seconds since 1970
}
static long long firefox_to_unix(const std::wstring &micros) {
    long long t = _wtoi64(micros.c_str());
    return t <= 0 ? 0 : t / 1000000;      // microseconds since 1970
}
ImportResult import_do(const ImportProfile &p, bool want_bookmarks, bool want_history, bool /*passwords*/) {
    ImportResult r;
    if (g_incognito) { r.note = T(L"No disponible en una ventana de incógnito."); return r; }   // nothing is kept from there
    // bookmarks
    if (want_bookmarks) {
        BmNode folder; folder.id = bm_new_id(); folder.folder = true;
        folder.title = L"De " + p.browser + L" (" + p.name + L")"; folder.added = (long long)time(NULL);
        if (p.firefox) {
            std::wstring db = copy_db(p.path + L"\\places.sqlite");
            for (auto &row : sql_rows(db, "select p.url, b.title from moz_bookmarks b join moz_places p on b.fk=p.id where b.type=1 and p.url like 'http%'")) {
                if (row.size() < 1 || row[0].empty()) continue;
                BmNode b; b.id = bm_new_id(); b.url = row[0]; b.title = row.size() > 1 ? row[1] : L""; b.added = (long long)time(NULL);
                folder.kids.push_back(b); r.bookmarks++;
            }
            drop_db(db);
        } else {
            std::string data;
            if (read_file(p.path + L"\\Bookmarks", data)) {
                Json j = json_parse(wide(data));
                const Json &roots = j[L"roots"];
                if (roots.kind == Json::OBJ) for (auto &kv : roots.obj) chromium_build(kv.second, folder, r.bookmarks);
            }
        }
        if (!folder.kids.empty()) { bm_root().kids.push_back(folder); bm_commit(); }
    }
    // history
    if (want_history) {
        if (p.firefox) {
            std::wstring db = copy_db(p.path + L"\\places.sqlite");
            for (auto &row : sql_rows(db, "select url, title, visit_count, last_visit_date from moz_places where url like 'http%' and hidden=0")) {
                if (row.size() < 4 || row[0].empty()) continue;
                if (history_import(row[0], row[1], _wtoi(row[2].c_str()), firefox_to_unix(row[3]))) r.history++;
            }
            drop_db(db);
        } else {
            std::wstring db = copy_db(p.path + L"\\History");
            for (auto &row : sql_rows(db, "select url, title, visit_count, last_visit_time from urls where url like 'http%'")) {
                if (row.size() < 4 || row[0].empty()) continue;
                if (history_import(row[0], row[1], _wtoi(row[2].c_str()), chrome_to_unix(row[3]))) r.history++;
            }
            drop_db(db);
        }
        history_flush();
    }
    if (p.passwords != 0) r.note = T(L"Las contraseñas no se pueden importar: el navegador las guarda protegidas.");
    return r;
}
// into another profile's files: its history and bookmarks are swapped in for the import, and this window's put back
ImportResult import_into(const std::wstring &id, const ImportProfile &p, bool bookmarks, bool history) {
    if (id == g_profile_id) return import_do(p, bookmarks, history, false);
    ImportResult r;
    if (g_incognito) { r.note = T(L"No disponible en una ventana de incógnito."); return r; }
    // its window, if open, writes what it has first, and reads it all again after
    HWND w = profile_window(id);
    DWORD_PTR res = 0;
    if (w) SendMessageTimeoutW(w, WM_APP + 41, 0, 0, SMTO_ABORTIFHUNG, 5000, &res);
    history_flush();
    std::wstring mine = g_data;
    history_stash(true); bookmarks_stash(true); g_quiet_commit = true;
    g_data = profile_data_dir(id);
    SHCreateDirectoryExW(NULL, g_data.c_str(), NULL);
    history_load();
    bookmarks_load();
    r = import_do(p, bookmarks, history, false);
    history_flush();
    g_data = mine;
    g_quiet_commit = false; bookmarks_stash(false); history_stash(false);
    if (w) PostMessageW(w, WM_APP + 40, 0, 0);
    return r;
}

// ---------------------------------------------------------------- can Pluma's engine take written passwords?
int passwords_store_kind() {
    std::wstring prof = wv_profile_dir();
    std::wstring db = copy_db(prof + L"\\Login Data");
    if (!db.empty()) {
        auto rows = sql_rows(db, "select hex(substr(password_value,1,3)) from logins limit 1");
        drop_db(db);
        if (!rows.empty() && rows[0].size() == 1) {
            std::wstring hex = lower(rows[0][0]);   // "v10" = 763130, "v20" = 763230
            if (hex == L"763130") return 10;
            if (hex == L"763230") return 20;
        }
    }
    // no stored password yet: the Local State key tells how new ones would be wrapped
    std::string ls;
    if (read_file(g_state + L"\\webview2\\EBWebView\\Local State", ls)) {
        Json oc = json_parse(wide(ls))[L"os_crypt"];
        if (!oc[L"app_bound_encrypted_key"].s.empty()) return 20;
        if (!oc[L"encrypted_key"].s.empty()) return 10;
    }
    return 0;
}
