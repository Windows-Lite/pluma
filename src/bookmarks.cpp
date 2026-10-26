// Pluma: bookmarks (marcadores.json, a tree of folders) and the two pages that go with them, served from the exe like
// the settings page: the manager (pluma://bookmarks) and the import wizard (pluma://import). settings.cpp serves the
// HTML and routes the messages here.
//
// The tree has two permanent folders at the top: "Barra de marcadores" (role 1, the optional bar) and "Tus páginas"
// (role 2, what the new tab page shows). The star in the address bar adds to "Tus páginas"; the imported ones and any
// the person makes are normal folders. g_pins is a flat view of "Tus páginas", so the new tab page code does not change.
#include "pluma.h"
#include "json.h"
#include <ctime>

std::vector<HistoryEntry> g_pins;
BmNode g_bm_root;
static int g_bm_next = 1;
static std::wstring bm_file() { return g_state + L"\\marcadores.json"; }

static std::wstring pin_key(const std::wstring &url) {
    std::wstring k = url;
    while (!k.empty() && k.back() == L'/') k.pop_back();
    return k;
}

// ---------------------------------------------------------------- find, by id
static BmNode *find_in(BmNode &n, int id) {
    if (n.id == id) return &n;
    for (auto &k : n.kids) if (BmNode *r = find_in(k, id)) return r;
    return nullptr;
}
BmNode *bm_find(int id) { return find_in(g_bm_root, id); }
static BmNode *find_role(int role) {
    for (auto &k : g_bm_root.kids) if (k.role == role) return &k;
    return nullptr;
}
int bm_bar_folder() { BmNode *n = find_role(1); return n ? n->id : 0; }
int bm_pins_folder() { BmNode *n = find_role(2); return n ? n->id : 0; }

// ---------------------------------------------------------------- the new tab page's view of "Tus páginas"
static void pins_sync() {
    g_pins.clear();
    BmNode *f = find_role(2);
    if (!f) return;
    for (auto &k : f->kids) if (!k.folder) { HistoryEntry e; e.url = k.url; e.title = k.title; g_pins.push_back(e); }
}
bool pins_has(const std::wstring &url) {
    BmNode *f = find_role(2);
    if (!f) return false;
    for (auto &k : f->kids) if (!k.folder && pin_key(k.url) == pin_key(url)) return true;
    return false;
}

// ---------------------------------------------------------------- read and write marcadores.json
static void write_node(std::wstring &o, const BmNode &n) {
    o += L"{\"id\":" + std::to_wstring(n.id) + L",\"role\":" + std::to_wstring(n.role);
    o += L",\"folder\":" + std::wstring(n.folder ? L"true" : L"false");
    o += L",\"title\":" + json_escape(n.title);
    if (!n.folder) o += L",\"url\":" + json_escape(n.url);
    o += L",\"added\":" + std::to_wstring(n.added);
    if (n.folder) {
        o += L",\"kids\":[";
        for (size_t i = 0; i < n.kids.size(); i++) { if (i) o += L","; write_node(o, n.kids[i]); }
        o += L"]";
    }
    o += L"}";
}
void bookmarks_save() {
    if (g_incognito) return;   // the incognito window keeps nothing (and must not overwrite the ordinary window's file)
    std::wstring o = L"{\"v\":1,\"next\":" + std::to_wstring(g_bm_next) + L",\"roots\":[";
    for (size_t i = 0; i < g_bm_root.kids.size(); i++) { if (i) o += L","; write_node(o, g_bm_root.kids[i]); }
    o += L"]}";
    write_file(bm_file(), utf8(o));
}
static BmNode read_node(const Json &j) {
    BmNode n;
    n.id = (int)j[L"id"].n;
    n.role = (int)j[L"role"].n;
    n.folder = j[L"folder"].b;
    n.title = j[L"title"].s;
    n.url = j[L"url"].s;
    n.added = (long long)j[L"added"].n;
    if (n.id >= g_bm_next) g_bm_next = n.id + 1;
    const Json &kids = j[L"kids"];
    if (kids.kind == Json::ARR) for (auto &c : kids.arr) n.kids.push_back(read_node(c));
    return n;
}
void bookmarks_load() {
    g_bm_root = BmNode(); g_bm_root.folder = true;
    g_bm_next = 1;
    std::string data;
    if (read_file(bm_file(), data)) {
        bool ok = false;
        Json j = json_parse(wide(data), &ok);
        if (ok && j[L"roots"].kind == Json::ARR) {
            g_bm_next = std::max(1, (int)j[L"next"].n);
            for (auto &c : j[L"roots"].arr) g_bm_root.kids.push_back(read_node(c));
        }
    }
    if (!find_role(1) || !find_role(2)) {
        // first run (or an old install with only paginas.tsv): build what is missing
        if (!find_role(1)) { BmNode bar; bar.id = g_bm_next++; bar.role = 1; bar.folder = true; bar.title = L"Barra de marcadores"; g_bm_root.kids.insert(g_bm_root.kids.begin(), bar); }
        if (!find_role(2)) {
            BmNode pins; pins.id = g_bm_next++; pins.role = 2; pins.folder = true; pins.title = L"Tus páginas";
            for (auto &f : tsv_read(L"paginas.tsv")) { if (f[0].empty()) continue; BmNode b; b.id = g_bm_next++; b.url = f[0]; b.title = f.size() > 1 ? f[1] : L""; pins.kids.push_back(b); }
            g_bm_root.kids.push_back(pins);
        }
        bookmarks_save();
    }
    pins_sync();
}

// ---------------------------------------------------------------- changes
int bm_count() {
    int n = 0;
    std::function<void(const BmNode &)> go = [&](const BmNode &x) { for (auto &k : x.kids) { if (!k.folder) n++; else go(k); } };
    go(g_bm_root);
    return n;
}
bool bm_url_in(int folder, const std::wstring &url) {
    BmNode *f = bm_find(folder);
    if (!f) return false;
    for (auto &k : f->kids) if (!k.folder && pin_key(k.url) == pin_key(url)) return true;
    return false;
}
int bm_add(int parent, const std::wstring &url, const std::wstring &title) {
    BmNode *f = bm_find(parent);
    if (!f || !f->folder || url.empty() || bm_url_in(parent, url)) return -1;
    BmNode b; b.id = g_bm_next++; b.url = url; b.title = title; b.added = (long long)time(NULL);
    f->kids.push_back(b);
    bookmarks_save();
    if (f->role == 2) { pins_sync(); ui_ntp_changed(); }
    bm_pages_refresh();
    return b.id;
}
int bm_add_folder(int parent, const std::wstring &title) {
    BmNode *f = bm_find(parent);
    if (!f || !f->folder) return -1;
    BmNode d; d.id = g_bm_next++; d.folder = true; d.title = title.empty() ? L"Carpeta" : title; d.added = (long long)time(NULL);
    f->kids.push_back(d);
    bookmarks_save();
    bm_pages_refresh();
    return d.id;
}
static bool remove_in(BmNode &n, int id, bool *touched_pins) {
    for (size_t i = 0; i < n.kids.size(); i++) {
        if (n.kids[i].id == id) { if (n.role == 2) *touched_pins = true; n.kids.erase(n.kids.begin() + i); return true; }
        if (n.kids[i].folder && remove_in(n.kids[i], id, touched_pins)) return true;
    }
    return false;
}
void bm_remove(int id) {
    BmNode *n = bm_find(id);
    if (!n || n->role != 0) return;   // the two permanent folders cannot be removed
    bool touched_pins = false;
    if (remove_in(g_bm_root, id, &touched_pins)) {
        bookmarks_save();
        pins_sync();
        ui_ntp_changed();
        bm_pages_refresh();
    }
}
void bm_rename(int id, const std::wstring &title) {
    BmNode *n = bm_find(id);
    if (!n || title.empty()) return;
    n->title = title;
    bookmarks_save();
    pins_sync(); ui_ntp_changed();
    bm_pages_refresh();
}
// detach a node (returns it by value), without touching the permanent folders
static bool detach(BmNode &parent, int id, BmNode &out) {
    for (size_t i = 0; i < parent.kids.size(); i++) {
        if (parent.kids[i].id == id) { out = parent.kids[i]; parent.kids.erase(parent.kids.begin() + i); return true; }
        if (parent.kids[i].folder && detach(parent.kids[i], id, out)) return true;
    }
    return false;
}
static bool is_inside(int ancestor, int id) {
    BmNode *a = bm_find(ancestor);
    if (!a) return false;
    return find_in(*a, id) != nullptr;
}
void bm_move(int id, int new_parent, int at) {
    if (id == new_parent || is_inside(id, new_parent)) return;   // never into itself or its own subtree
    BmNode *dst = bm_find(new_parent);
    if (!dst || !dst->folder) return;
    BmNode *n = bm_find(id);
    if (!n || n->role != 0) return;
    BmNode moved;
    if (!detach(g_bm_root, id, moved)) return;
    dst = bm_find(new_parent);   // detach may have invalidated the pointer
    if (!dst) { g_bm_root.kids.push_back(moved); }
    else { at = std::max(0, std::min(at, (int)dst->kids.size())); dst->kids.insert(dst->kids.begin() + at, moved); }
    bookmarks_save();
    pins_sync(); ui_ntp_changed();
    bm_pages_refresh();
}
void bm_toggle_in(int folder, const std::wstring &url, const std::wstring &title) {
    BmNode *f = bm_find(folder);
    if (!f) return;
    for (size_t i = 0; i < f->kids.size(); i++)
        if (!f->kids[i].folder && pin_key(f->kids[i].url) == pin_key(url)) { int id = f->kids[i].id; bm_remove(id); return; }
    bm_add(folder, url, title);
}

// the star adds to / removes from "Tus páginas", and the new tab page code calls these
void pins_add(const std::wstring &url, const std::wstring &title, size_t at) {
    BmNode *f = find_role(2);
    if (!f || url.empty() || pins_has(url)) return;
    BmNode b; b.id = g_bm_next++; b.url = url; b.title = title; b.added = (long long)time(NULL);
    f->kids.insert(f->kids.begin() + std::min(at, f->kids.size()), b);
    bookmarks_save();
    pins_sync();
    bm_pages_refresh();
}
void pins_remove(const std::wstring &url) {
    BmNode *f = find_role(2);
    if (!f) return;
    for (size_t i = 0; i < f->kids.size(); i++)
        if (!f->kids[i].folder && pin_key(f->kids[i].url) == pin_key(url)) { f->kids.erase(f->kids.begin() + i); bookmarks_save(); pins_sync(); bm_pages_refresh(); return; }
}
bool pins_toggle(const std::wstring &url, const std::wstring &title) {
    if (pins_has(url)) { pins_remove(url); return false; }
    pins_add(url, title, g_pins.size());
    return true;
}

// for import: it builds a whole subtree and commits once
int bm_new_id() { return g_bm_next++; }
BmNode &bm_root() { return g_bm_root; }
void bm_commit() { bookmarks_save(); pins_sync(); ui_ntp_changed(); bm_pages_refresh(); }

// ---------------------------------------------------------------- the pages (bookmarks.cpp answers; settings.cpp serves)
void bookmarks_open() {
    std::wstring page = L"https://pluma.local/bookmarks";
    for (size_t i = 0; i < g_tabs.size(); i++) if (settings_page_url(g_tabs[i]->url) && g_tabs[i]->url.find(L"/bookmarks") != std::wstring::npos) { tab_activate((int)i); return; }
    tab_new(page, true);
}
void import_open() {
    std::wstring page = L"https://pluma.local/import";
    for (size_t i = 0; i < g_tabs.size(); i++) if (settings_page_url(g_tabs[i]->url) && g_tabs[i]->url.find(L"/import") != std::wstring::npos) { tab_activate((int)i); return; }
    tab_new(page, true);
}

// the permanent folders keep their stored name, but show it in the UI language unless the person renamed them
std::wstring bm_shown_title(const BmNode &n) {
    if (n.role == 1 && n.title == L"Barra de marcadores") return T(L"Barra de marcadores");
    if (n.role == 2 && n.title == L"Tus páginas") return T(L"Tus páginas");
    return n.title;
}

static void json_tree(std::wstring &o, const BmNode &n) {
    o += L"{\"id\":" + std::to_wstring(n.id) + L",\"role\":" + std::to_wstring(n.role);
    o += L",\"folder\":" + std::wstring(n.folder ? L"true" : L"false");
    o += L",\"title\":" + json_escape(bm_shown_title(n));
    o += L",\"url\":" + json_escape(n.url);
    if (n.folder) {
        o += L",\"kids\":[";
        for (size_t i = 0; i < n.kids.size(); i++) { if (i) o += L","; json_tree(o, n.kids[i]); }
        o += L"]";
    }
    o += L"}";
}
static std::wstring texts_json(const Json &texts) {
    if (texts.kind != Json::ARR) return L"";
    std::wstring o = L",\"t\":{";
    bool first = true;
    for (auto &s : texts.arr) { if (!s.is_str()) continue; o += (first ? L"" : L",") + json_escape(s.s) + L":" + json_escape(T(s.s.c_str())); first = false; }
    return o + L"}";
}
static std::wstring profiles_json() {
    std::wstring o = L"[";
    auto ps = import_scan();
    for (size_t i = 0; i < ps.size(); i++) {
        if (i) o += L",";
        o += L"{\"i\":" + std::to_wstring(i) + L",\"browser\":" + json_escape(ps[i].browser) + L",\"name\":" + json_escape(ps[i].name);
        o += L",\"firefox\":" + std::wstring(ps[i].firefox ? L"true" : L"false") + L",\"running\":" + std::wstring(ps[i].running ? L"true" : L"false");
        o += L",\"bookmarks\":" + std::to_wstring(ps[i].bookmarks) + L",\"history\":" + std::to_wstring(ps[i].history) + L",\"passwords\":" + std::to_wstring(ps[i].passwords) + L"}";
    }
    return o + L"]";
}
std::wstring bm_page_state(const std::wstring &path, const Json &texts) {
    std::wstring o = L"{\"op\":\"state\",\"dark\":" + std::wstring(dark_theme() ? L"true" : L"false") + L",\"lang\":" + json_escape(lang_code());
    if (path.find(L"/import") != std::wstring::npos) {
        o += L",\"profiles\":" + profiles_json();
        o += L",\"passwordKind\":" + std::to_wstring(passwords_store_kind());
    } else {
        o += L",\"bar\":" + std::wstring(g_set.bookmarks_bar ? L"true" : L"false");
        o += L",\"pinsFolder\":" + std::to_wstring(bm_pins_folder()) + L",\"barFolder\":" + std::to_wstring(bm_bar_folder());
        o += L",\"tree\":"; json_tree(o, g_bm_root);
    }
    o += texts_json(texts);
    return o + L"}";
}
void bm_pages_refresh() {
    for (auto &t : g_tabs)
        if (t->view && settings_page_url(t->url) && (t->url.find(L"/bookmarks") != std::wstring::npos))
            t->view->PostWebMessageAsJson(bm_page_state(t->url, Json()).c_str());
}
void bm_page_do(ICoreWebView2 *v, const std::wstring &path, const Json &j) {
    std::wstring op = j[L"op"].s;
    if (op == L"get") { v->PostWebMessageAsJson(bm_page_state(path, j[L"texts"]).c_str()); return; }
    if (op == L"open") { std::wstring u = j[L"url"].s; if (!u.empty()) tab_new(u, !j[L"background"].b); return; }
    if (op == L"import" && path.find(L"/import") == std::wstring::npos) { import_open(); return; }
    if (path.find(L"/import") != std::wstring::npos) {
        if (op == L"import") {
            auto ps = import_scan();
            int i = (int)j[L"profile"].n;
            if (i < 0 || i >= (int)ps.size()) return;
            ImportResult r = import_do(ps[i], j[L"bookmarks"].b, j[L"history"].b, false);
            std::wstring m = L"{\"op\":\"done\",\"bookmarks\":" + std::to_wstring(r.bookmarks) + L",\"history\":" + std::to_wstring(r.history) + L",\"note\":" + json_escape(r.note) + L"}";
            v->PostWebMessageAsJson(m.c_str());
        } else if (op == L"openEdgePasswords") passwords_open_edge_export();
        else if (op == L"importPasswords") {
            ImportResult r = passwords_import_pick();
            if (r.passwords < 0) return;   // cancelled
            std::wstring m = L"{\"op\":\"passwords\",\"n\":" + std::to_wstring(r.passwords) + L",\"note\":" + json_escape(r.note) + L"}";
            v->PostWebMessageAsJson(m.c_str());
        } else if (op == L"deleteCsv") {
            std::wstring m = std::wstring(L"{\"op\":\"csvDeleted\",\"ok\":") + (passwords_delete_csv() ? L"true" : L"false") + L"}";
            v->PostWebMessageAsJson(m.c_str());
        } else if (op == L"restart") app_restart(false);
        return;
    }
    // the bookmarks manager
    if (op == L"addFolder") bm_add_folder((int)j[L"parent"].n, j[L"title"].s);
    else if (op == L"rename") bm_rename((int)j[L"id"].n, j[L"title"].s);
    else if (op == L"remove") bm_remove((int)j[L"id"].n);
    else if (op == L"move") bm_move((int)j[L"id"].n, (int)j[L"parent"].n, (int)j[L"at"].n);
    else if (op == L"bar") { g_set.bookmarks_bar = j[L"on"].b; settings_save(); ui_layout(); }
    v->PostWebMessageAsJson(bm_page_state(path, Json()).c_str());
}
