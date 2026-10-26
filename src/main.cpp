// Pluma: the window, its frame, settings, history, and starting up. See pluma.h.
#include "pluma.h"
#include <unordered_map>
#include <ctime>
#include <atomic>
#include <thread>

HINSTANCE g_inst;
HWND g_win, g_bar, g_edit;
std::wstring g_root, g_state, g_data, g_profile_id;
bool g_incognito, g_test, g_test_visible, g_dev, g_quitting, g_fullscreen, g_motion = true;
double g_open_asked = 0;
ComPtr<ICoreWebView2Environment> g_env;
Settings g_set;
std::vector<HistoryEntry> g_history;

static std::wstring g_class = L"PlumaVentana", g_mutex = L"PlumaNavegadorUnico";
static const UINT WM_APP_SHOW = WM_APP + 1, WM_APP_QUIT = WM_APP + 2, WM_APP_OPEN = WM_APP + 3;
static const ULONG_PTR kCopyOpen = 0x504C55;   // WM_COPYDATA from a second Pluma.exe: urls, one per line
static const ULONG_PTR kCopyProfile = 0x504C56;   // ... from another profile's window: "open this profile" (id, then urls)
static const ULONG_PTR kCopyOpenHere = 0x504C57;  // ... urls for this window itself (not routed to the last used profile)
static const UINT_PTR kTimerQuit = 1, kTimerHistory = 2, kTimerTabs = 3, kTimerSession = 4, kTimerBurst = 5, kTimerTransitions = 6;
static int g_burst_left, g_burst_n;   // tests: a burst of pictures
static double g_burst_t0;
static LARGE_INTEGER g_t0, g_freq;
static bool g_start_hidden, g_env_ready;
static std::vector<std::wstring> g_waiting_urls;   // asked for before the engine was ready
static WINDOWPLACEMENT g_before_fs = { sizeof(WINDOWPLACEMENT) };
static int g_caption_down;

// ---------------------------------------------------------------- small helpers
double now_ms() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (t.QuadPart - g_t0.QuadPart) * 1000.0 / g_freq.QuadPart;
}
std::string utf8(const std::wstring &s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, NULL, NULL);
    return u;
}
std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
void log_line(const std::wstring &s) {
    // (shared: every profile's window writes here too)
    HANDLE f = CreateFileW((g_state + L"\\pluma.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size)) {
        if (size.QuadPart > (2 << 20)) { CloseHandle(f); DeleteFileW((g_state + L"\\pluma.log").c_str()); return log_line(s); }
        if (size.QuadPart == 0) { DWORD w; WriteFile(f, "\xEF\xBB\xBF", 3, &w, NULL); }
    }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t head[64];
    swprintf_s(head, L"%04d-%02d-%02d %02d:%02d:%02d.%03d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::string u = utf8(head + (g_profile_id.empty() ? L"" : L"[" + g_profile_id + L"] ") + s + L"\r\n");
    DWORD w; WriteFile(f, u.data(), (DWORD)u.size(), &w, NULL);
    CloseHandle(f);
}
bool read_file(const std::wstring &p, std::string &out) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size; GetFileSizeEx(f, &size);
    if (size.QuadPart > (256 << 20)) { CloseHandle(f); return false; }
    out.resize((size_t)size.QuadPart);
    DWORD r = 0;
    BOOL ok = size.QuadPart == 0 || ReadFile(f, out.data(), (DWORD)out.size(), &r, NULL);
    CloseHandle(f);
    return ok && r == out.size();
}
bool write_file(const std::wstring &p, const std::string &data) {
    // write beside and swap, so a crash never leaves half a file
    std::wstring tmp = p + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = data.empty() || WriteFile(f, data.data(), (DWORD)data.size(), &w, NULL);
    CloseHandle(f);
    if (!ok || w != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}
std::wstring trim(const std::wstring &s) {
    size_t a = s.find_first_not_of(L" \t\r\n\x3000\xA0"), b = s.find_last_not_of(L" \t\r\n\x3000\xA0");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
bool starts_with(const std::wstring &s, const wchar_t *p) { return s.rfind(p, 0) == 0; }
bool ends_with_i(const std::wstring &s, const wchar_t *p) {
    size_t n = wcslen(p);
    return s.size() >= n && !_wcsicmp(s.c_str() + s.size() - n, p);
}
std::wstring url_host(const std::wstring &url) {
    size_t a = url.find(L"://");
    if (a == std::wstring::npos) return L"";
    a += 3;
    size_t b = url.find_first_of(L"/?#", a);
    std::wstring h = url.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
    size_t at = h.rfind(L'@');
    if (at != std::wstring::npos) h = h.substr(at + 1);
    return lower(h);
}
std::wstring url_decode(const std::wstring &v) {
    std::string bytes;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == L'%' && i + 2 < v.size() && iswxdigit(v[i + 1]) && iswxdigit(v[i + 2])) {
            bytes += (char)wcstol(v.substr(i + 1, 2).c_str(), NULL, 16); i += 2;
        } else bytes += utf8(std::wstring(1, v[i]));
    }
    return wide(bytes);
}
std::wstring url_encode(const std::wstring &s) {
    std::string u = utf8(s);
    std::wstring out;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (wchar_t)c;
        else if (c == ' ') out += L'+';
        else { wchar_t b[4]; swprintf_s(b, L"%%%02X", c); out += b; }
    }
    return out;
}
std::wstring json_escape(const std::wstring &s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c < 0x20 || c == 0x2028 || c == 0x2029) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", (unsigned)c); out += b; }
        else out += c;
    }
    return out + L"\"";
}
std::wstring hash_hex(const std::wstring &s) {
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : s) { h ^= (unsigned long long)c; h *= 1099511628211ULL; }
    wchar_t b[24]; swprintf_s(b, L"%016llx", h);
    return b;
}
void Anim::go(float target, double d) {
    if (target == to) return;
    double t = now_ms();
    from = at(t); to = target; t0 = t; dur = g_motion ? d : 0;
}

// ---------------------------------------------------------------- settings (ajustes.ini)
static std::wstring ini() { return g_state + L"\\ajustes.ini"; }
static std::wstring ini_get(const wchar_t *key, const wchar_t *def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"pluma", key, def, buf, 1024, ini().c_str());
    return buf;
}
void settings_load() {
    g_set.engine = ini_get(L"buscador", L"google");
    // as Windows by default; "tema" saved before ajustes_v 2 was the old default (dark), not a choice
    std::wstring th = _wtoi(ini_get(L"ajustes_v", L"1").c_str()) >= 2 ? ini_get(L"tema", L"sistema") : L"sistema";
    g_set.theme = th == L"claro" ? 0 : th == L"oscuro" ? 1 : 2;
    g_set.font = ini_get(L"fuente", L"");
    g_set.lang = ini_get(L"idioma", L"");
    std::wstring up = ini_get(L"actualizar", L"auto");
    g_set.update = up == L"no" ? 0 : up == L"avisar" ? 1 : 2;
    g_set.sleep_s = std::max(10, _wtoi(ini_get(L"dormir_s", L"45").c_str()));
    g_set.discard_s = std::max(0, _wtoi(ini_get(L"descargar_s", L"60").c_str()));
    if (g_set.discard_s > 0 && g_set.discard_s < 10) g_set.discard_s = 10;
    g_set.keep_recent = std::max(0, _wtoi(ini_get(L"recientes", L"2").c_str()));
    g_set.recent_discard_min = std::max(0, _wtoi(ini_get(L"descargar_min", L"20").c_str()));
    g_set.spare = ini_get(L"reserva", L"0") == L"1";
    g_set.resident = ini_get(L"residente", L"1") != L"0";
    g_set.native_video = ini_get(L"video_propio", L"1") != L"0";
    g_set.youtube_lite = ini_get(L"youtube_ligero", L"1") != L"0";
    g_set.music_lite = ini_get(L"music_ligero", L"1") != L"0";
    g_set.suggest_top = ini_get(L"sugerir_visitadas", L"1") != L"0";
    g_set.sound.preset = ini_get(L"efecto", L"no");
    sound_eq_parse(ini_get(L"eq", L""), g_set.sound.custom.eq);
    g_set.sound.custom.bass = _wtoi(ini_get(L"graves", L"0").c_str());
    g_set.sound.custom.reverb = _wtoi(ini_get(L"reverb", L"0").c_str());
    g_set.sound.pitch = _wtoi(ini_get(L"tono", L"0").c_str());
    sound_clamp(g_set.sound);
    g_set.bookmarks_bar = ini_get(L"barra_marcadores", L"0") == L"1";
    g_set.reader_size = std::min(40, std::max(12, _wtoi(ini_get(L"lectura_letra", L"20").c_str())));
    g_set.schemes.clear();
    std::wstring sc = ini_get(L"protocolos", L"ytmp3");
    size_t pos = 0;
    while (pos <= sc.size()) {
        size_t c = sc.find(L',', pos);
        std::wstring one = lower(trim(sc.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos)));
        if (!one.empty()) g_set.schemes.push_back(one);
        if (c == std::wstring::npos) break;
        pos = c + 1;
    }
}
void settings_save() {
    std::wstring f = ini();
    WritePrivateProfileStringW(L"pluma", L"buscador", g_set.engine.c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"ajustes_v", L"2", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"tema", g_set.theme == 1 ? L"oscuro" : g_set.theme == 2 ? L"sistema" : L"claro", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"fuente", g_set.font.c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"idioma", g_set.lang.c_str(), f.c_str());
    settings_page_refresh_soon();
    WritePrivateProfileStringW(L"pluma", L"actualizar", g_set.update == 0 ? L"no" : g_set.update == 1 ? L"avisar" : L"auto", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"dormir_s", std::to_wstring(g_set.sleep_s).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"descargar_s", std::to_wstring(g_set.discard_s).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"recientes", std::to_wstring(g_set.keep_recent).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"descargar_min", std::to_wstring(g_set.recent_discard_min).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"residente", g_set.resident ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"video_propio", g_set.native_video ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"youtube_ligero", g_set.youtube_lite ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"music_ligero", g_set.music_lite ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"sugerir_visitadas", g_set.suggest_top ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"lectura_letra", std::to_wstring(g_set.reader_size).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"efecto", g_set.sound.preset.c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"eq", sound_eq_text(g_set.sound.custom.eq).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"graves", std::to_wstring(g_set.sound.custom.bass).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"reverb", std::to_wstring(g_set.sound.custom.reverb).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"tono", std::to_wstring(g_set.sound.pitch).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"barra_marcadores", g_set.bookmarks_bar ? L"1" : L"0", f.c_str());
    std::wstring sc;
    for (auto &s : g_set.schemes) sc += (sc.empty() ? L"" : L",") + s;
    WritePrivateProfileStringW(L"pluma", L"protocolos", sc.c_str(), f.c_str());
}
static const struct { const wchar_t *id, *name, *url; } kEngines[] = {
    { L"google", L"Google", L"https://www.google.com/search?q=" },
    { L"duckduckgo", L"DuckDuckGo", L"https://duckduckgo.com/?q=" },
    { L"bing", L"Bing", L"https://www.bing.com/search?q=" },
    { L"startpage", L"Startpage", L"https://www.startpage.com/do/search?q=" },
    { L"brave", L"Brave Search", L"https://search.brave.com/search?q=" } };
std::wstring search_url(const std::wstring &q) {
    for (auto &e : kEngines) if (g_set.engine == e.id) return e.url + url_encode(q);
    return kEngines[0].url + url_encode(q);
}
std::wstring engine_name(const std::wstring &id) {
    for (auto &e : kEngines) if (id == e.id) return e.name;
    return kEngines[0].name;
}

// ---------------------------------------------------------------- history (historial.tsv: url, title, visits, last visit)
static std::unordered_map<std::wstring, size_t> g_hist_index;
static bool g_hist_dirty;
static const size_t kHistoryMax = 8000;
static std::wstring tsv_clean(const std::wstring &s) {
    std::wstring o = s;
    for (auto &c : o) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return o;
}
void history_load() {
    std::string data;
    if (!read_file(g_data + L"\\historial.tsv", data)) return;
    std::wstring w = wide(data);
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> f;
        size_t a = 0;
        while (true) { size_t t = line.find(L'\t', a); f.push_back(line.substr(a, t == std::wstring::npos ? std::wstring::npos : t - a)); if (t == std::wstring::npos) break; a = t + 1; }
        if (f.size() < 4 || f[0].empty() || g_hist_index.count(f[0])) continue;
        HistoryEntry e; e.url = f[0]; e.title = f[1]; e.visits = _wtoi(f[2].c_str()); e.last = _wtoi64(f[3].c_str());
        g_hist_index[e.url] = g_history.size();
        g_history.push_back(e);
    }
}
static void history_reindex() {
    g_hist_index.clear();
    for (size_t i = 0; i < g_history.size(); i++) g_hist_index[g_history[i].url] = i;
}
void history_flush() {
    if (!g_hist_dirty || g_incognito) return;
    g_hist_dirty = false;
    if (g_history.size() > kHistoryMax) {
        // keep the most recent
        std::sort(g_history.begin(), g_history.end(), [](const HistoryEntry &a, const HistoryEntry &b) { return a.last > b.last; });
        g_history.resize(kHistoryMax);
        history_reindex();
    }
    std::wstring out;
    out.reserve(g_history.size() * 120);
    for (auto &e : g_history)
        out += e.url + L"\t" + tsv_clean(e.title) + L"\t" + std::to_wstring(e.visits) + L"\t" + std::to_wstring(e.last) + L"\n";
    write_file(g_data + L"\\historial.tsv", utf8(out));
}
void history_clear() {
    g_history.clear();
    g_hist_index.clear();
    g_hist_dirty = false;
    DeleteFileW((g_data + L"\\historial.tsv").c_str());
}
void history_stash(bool push) {
    // importing into another profile: its history in for a moment, this one's put aside untouched
    static std::vector<HistoryEntry> saved; static bool saved_dirty;
    if (push) { saved = std::move(g_history); saved_dirty = g_hist_dirty; g_history.clear(); g_hist_dirty = false; }
    else { g_history = std::move(saved); saved.clear(); g_hist_dirty = saved_dirty; }
    history_reindex();
}
void profile_reload_state() {
    // another Pluma wrote this profile's history and bookmarks (an import from its window): read them again
    g_history.clear(); g_hist_dirty = false; history_reindex();
    history_load();
    bookmarks_load();
    ui_ntp_changed();
    bm_pages_refresh();
    ext_sync([] { ui_layout(); });   // (and extensions it was given)
    ui_layout();
}
static bool history_worthy(const std::wstring &url) {
    return (starts_with(url, L"https://") || starts_with(url, L"http://")) && url.size() < 2048;
}
void history_add(const std::wstring &url, const std::wstring &title) {
    if (!history_worthy(url)) return;
    auto it = g_hist_index.find(url);
    if (it == g_hist_index.end()) {
        g_hist_index[url] = g_history.size();
        g_history.push_back({ url, title, 0, 0 });
        it = g_hist_index.find(url);
    }
    HistoryEntry &e = g_history[it->second];
    e.visits++;
    e.last = (long long)time(NULL);
    if (!title.empty()) e.title = title;
    g_hist_dirty = true;
    SetTimer(g_win, kTimerHistory, 4000, NULL);
}
void history_title(const std::wstring &url, const std::wstring &title) {
    auto it = g_hist_index.find(url);
    if (it == g_hist_index.end() || title.empty() || g_history[it->second].title == title) return;
    g_history[it->second].title = title;
    g_hist_dirty = true;
    SetTimer(g_win, kTimerHistory, 4000, NULL);
}
bool history_import(const std::wstring &url, const std::wstring &title, int visits, long long last) {
    // from another browser: add if new, else keep the more recent visit and add up the counts (the caller flushes once)
    if (!history_worthy(url)) return false;
    auto it = g_hist_index.find(url);
    if (it == g_hist_index.end()) {
        g_hist_index[url] = g_history.size();
        g_history.push_back({ url, title, std::max(1, visits), last });
        g_hist_dirty = true;
        return true;
    }
    HistoryEntry &e = g_history[it->second];
    e.visits += std::max(1, visits);
    if (last > e.last) e.last = last;
    if (e.title.empty() && !title.empty()) e.title = title;
    g_hist_dirty = true;
    return false;
}
static std::wstring bare(const std::wstring &url) {
    // what people type: no scheme, no "www."
    std::wstring s = lower(url);
    if (starts_with(s, L"https://")) s = s.substr(8);
    else if (starts_with(s, L"http://")) s = s.substr(7);
    if (starts_with(s, L"www.")) s = s.substr(4);
    return s;
}
static double frecency(const HistoryEntry &e, long long now) {
    double days = (double)(now - e.last) / 86400.0;
    return e.visits / (1.0 + days / 5.0);
}
std::vector<Suggestion> history_suggest(const std::wstring &typed, std::wstring *inline_completion) {
    std::vector<Suggestion> out;
    if (inline_completion) inline_completion->clear();
    std::wstring t = lower(trim(typed));
    if (t.empty()) return out;
    std::wstring tb = bare(t);
    std::vector<std::wstring> words;
    for (size_t a = 0; a < t.size();) {
        size_t b = t.find(L' ', a);
        std::wstring w = t.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        if (!w.empty()) words.push_back(w);
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    long long now = (long long)time(NULL);
    struct Hit { double score; size_t i; };
    std::vector<Hit> hits;
    double best_prefix = -1; size_t best_i = 0;
    for (size_t i = 0; i < g_history.size(); i++) {
        const HistoryEntry &e = g_history[i];
        std::wstring b = bare(e.url);
        double fr = frecency(e, now);
        if (!tb.empty() && starts_with(b, tb.c_str())) {
            // the shorter the address, the more likely it is the one meant ("you" -> youtube.com, not a video)
            double sc = 1000 + fr * 10 - b.size() * 0.5;
            hits.push_back({ sc, i });
            if (sc > best_prefix) { best_prefix = sc; best_i = i; }
            continue;
        }
        std::wstring hay = b + L" " + lower(e.title);
        bool all = !words.empty();
        for (auto &w : words) if (hay.find(w) == std::wstring::npos) { all = false; break; }
        if (all) hits.push_back({ 100 + fr * 10, i });
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; });
    for (auto &h : hits) {
        if (out.size() >= 6) break;
        out.push_back({ g_history[h.i].url, g_history[h.i].title, false });
    }
    if (inline_completion && best_prefix > 0 && typed.find(L' ') == std::wstring::npos) {
        std::wstring b = bare(g_history[best_i].url);
        size_t slash = b.find(L'/');
        // complete to the site while typing its name; to the whole address once past it
        std::wstring site = slash == std::wstring::npos ? b : b.substr(0, slash);
        std::wstring full = b;
        while (!full.empty() && full.back() == L'/') full.pop_back();
        std::wstring c = tb.size() < site.size() ? site : full;
        if (c.size() > tb.size()) *inline_completion = typed + c.substr(tb.size());
    }
    return out;
}
// ---------------------------------------------------------------- the new tab page: the sites the person took off its
// suggestions (ocultas.tsv: one a line). The person's own pages ("Tus páginas") are bookmarks now (bookmarks.cpp).
static std::vector<std::wstring> g_hidden_sites;
std::vector<std::vector<std::wstring>> tsv_read(const std::wstring &file) {
    std::vector<std::vector<std::wstring>> rows;
    std::string data;
    if (!read_file(g_data + L"\\" + file, data)) return rows;
    std::wstring w = wide(data);
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::wstring> f;
        size_t a = 0;
        while (true) { size_t t = line.find(L'\t', a); f.push_back(line.substr(a, t == std::wstring::npos ? std::wstring::npos : t - a)); if (t == std::wstring::npos) break; a = t + 1; }
        rows.push_back(f);
    }
    return rows;
}
static std::wstring site_key(const std::wstring &url) {
    std::wstring h = lower(url_host(url));
    return starts_with(h, L"www.") ? h.substr(4) : h;
}
void hidden_load() {
    g_hidden_sites.clear();
    for (auto &f : tsv_read(L"ocultas.tsv")) if (!f[0].empty()) g_hidden_sites.push_back(f[0]);
}
void history_hide_site(const std::wstring &url, bool hide) {
    std::wstring k = site_key(url);
    auto it = std::find(g_hidden_sites.begin(), g_hidden_sites.end(), k);
    if (hide && it == g_hidden_sites.end()) g_hidden_sites.push_back(k);
    else if (!hide && it != g_hidden_sites.end()) g_hidden_sites.erase(it);
    else return;
    std::wstring out;
    for (auto &s : g_hidden_sites) out += s + L"\n";
    write_file(g_data + L"\\ocultas.tsv", utf8(out));
}
std::vector<HistoryEntry> history_top(int n) {
    // the most visited sites, one entry per site (its front page if it was visited), but those the person took off
    long long now = (long long)time(NULL);
    struct Site { double score = 0; size_t best = 0; double best_score = -1; bool front = false; };
    std::unordered_map<std::wstring, Site> sites;
    for (size_t i = 0; i < g_history.size(); i++) {
        const HistoryEntry &e = g_history[i];
        std::wstring h = url_host(e.url);
        if (h.empty()) continue;
        if (!g_hidden_sites.empty() && std::find(g_hidden_sites.begin(), g_hidden_sites.end(), site_key(e.url)) != g_hidden_sites.end()) continue;
        Site &s = sites[h];
        double fr = frecency(e, now);
        s.score += fr;
        std::wstring b = bare(e.url);
        bool front = b.find(L'/') == std::wstring::npos || b.find(L'/') == b.size() - 1;
        if ((front && !s.front) || (front == s.front && fr > s.best_score)) { s.best = i; s.best_score = fr; s.front = front; }
    }
    std::vector<std::pair<double, size_t>> v;
    for (auto &kv : sites) v.push_back({ kv.second.score, kv.second.best });
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
    std::vector<HistoryEntry> out;
    for (auto &p : v) { if ((int)out.size() >= n) break; out.push_back(g_history[p.second]); }
    return out;
}

// ---------------------------------------------------------------- the window
static void save_placement() {
    if (g_incognito) return;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (g_fullscreen) wp = g_before_fs;
    else if (!GetWindowPlacement(g_win, &wp)) return;
    write_file(g_data + L"\\ventana.bin", std::string((const char *)&wp, sizeof(wp)));
}
static void restore_placement() {
    std::string s;
    if (!read_file(g_data + L"\\ventana.bin", s) || s.size() != sizeof(WINDOWPLACEMENT)) {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int w = (wa.right - wa.left) * 86 / 100, h = (wa.bottom - wa.top) * 88 / 100;
        SetWindowPos(g_win, NULL, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    WINDOWPLACEMENT wp; memcpy(&wp, s.data(), sizeof(wp));
    wp.length = sizeof(wp);
    UINT show = wp.showCmd;
    wp.showCmd = SW_HIDE;   // placed now, shown later
    SetWindowPlacement(g_win, &wp);
    if (show == SW_SHOWMAXIMIZED) SetPropW(g_win, L"max", (HANDLE)1);
}
static RECT g_test_rect;   // tests: where the window was before fullscreen
RECT fullscreen_rect() {
    // the screen the window is on; in tests, a screen of that size where the window is (off the user's screen)
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_win, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r = mi.rcMonitor;
    if (g_test) {
        RECT w; GetWindowRect(g_win, &w);
        if (g_fullscreen) return w;
        OffsetRect(&r, w.left - r.left, w.top - r.top);
    }
    return r;
}
void fullscreen_now(bool on) {
    if (on == g_fullscreen) return;
    // without Windows' own animation of the window (it scaled the old picture of the page, half see-through, over
    // the new one: "tosco", in the user's recording of a page in fullscreen, 25/10)
    BOOL off = TRUE;
    DwmSetWindowAttribute(g_win, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    SetTimer(g_win, kTimerTransitions, 700, NULL);
    if (on) {
        g_before_fs.length = sizeof(g_before_fs);
        GetWindowPlacement(g_win, &g_before_fs);
        GetWindowRect(g_win, &g_test_rect);
        RECT m = fullscreen_rect();
        g_fullscreen = true;
        SetWindowLongW(g_win, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        // (a test on the screen stays where it is, over the rest: the zoom's photo has to stay over it)
        SetWindowPos(g_win, g_test ? HWND_BOTTOM : HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | (g_test ? SWP_NOACTIVATE : 0) | (g_test_visible ? SWP_NOZORDER : 0));
    } else {
        g_fullscreen = false;
        SetWindowLongW(g_win, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        // (tests: not SetWindowPlacement, which brings a window off the screen back onto it)
        if (g_test) SetWindowPos(g_win, HWND_BOTTOM, g_test_rect.left, g_test_rect.top, g_test_rect.right - g_test_rect.left,
                                 g_test_rect.bottom - g_test_rect.top, SWP_NOACTIVATE | (g_test_visible ? SWP_NOZORDER : 0));
        else SetWindowPlacement(g_win, &g_before_fs);
        SetWindowPos(g_win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    ui_layout();
    tabs_layout_views();
}
void set_fullscreen(bool on) {
    // with the picture moving (zoom.cpp) for Pluma's video player; at once for a page (the page lays out its own
    // fullscreen: a zoom of the whole page looked doubled and wrong in the user's recording, 25/10)
    if (zoom_busy()) { zoom_want(on); return; }   // on its way: it ends where asked last
    if (on == g_fullscreen) return;
    if (!g_motion || !IsWindowVisible(g_win) || IsIconic(g_win) || video_aspect() <= 0) { fullscreen_now(on); return; }
    zoom_start(on);
}
static void show_window() {
    g_start_hidden = false;
    if (g_test) {
        // a test instance draws for real but off the screen, without taking the focus from the user
        int w = 1400, h = 900, x = -7000, y = 100;
        wchar_t size[32] = L"", at[32] = L"";
        if (GetEnvironmentVariableW(L"PLUMA_TAMANO", size, 32)) swscanf_s(size, L"%dx%d", &w, &h);   // e.g. 1920x1040
        // PLUMA_A_LA_VISTA=x,y: on the screen, over the rest (to record a transition as the user sees it)
        if (GetEnvironmentVariableW(L"PLUMA_A_LA_VISTA", at, 32) && swscanf_s(at, L"%d,%d", &x, &y) == 2) g_test_visible = true;
        SetWindowPos(g_win, g_test_visible ? HWND_TOPMOST : HWND_BOTTOM, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    if (!IsWindowVisible(g_win)) ShowWindow(g_win, GetPropW(g_win, L"max") ? SW_SHOWMAXIMIZED : SW_SHOW);
    else if (IsIconic(g_win)) ShowWindow(g_win, SW_RESTORE);
    SetForegroundWindow(g_win);
}
static void hide_window() {
    // closed but kept warm: the session is saved, the pages frozen and the memory given back (tabs_on_hide)
    ui_close_popups();
    zoom_stop();
    if (g_fullscreen) fullscreen_now(false);   // (at once: the window goes)
    save_placement();
    if (IsZoomed(g_win)) SetPropW(g_win, L"max", (HANDLE)1); else RemovePropW(g_win, L"max");
    session_save();
    history_flush();
    ShowWindow(g_win, SW_HIDE);
    tabs_on_hide();
    update_on_hide();
}
static void open_urls(const std::vector<std::wstring> &urls) {
    if (!g_env_ready) { for (auto &u : urls) g_waiting_urls.push_back(u); show_window(); return; }
    bool was_hidden = !IsWindowVisible(g_win);
    if (was_hidden) g_open_asked = now_ms();
    if (was_hidden && g_tabs.empty() && !g_incognito) session_restore();
    for (auto &u : urls) tab_new(u, true);
    if (urls.empty() && g_tabs.empty()) tab_new(L"", true);
    if (urls.empty() && was_hidden) { Tab *t = active_tab(); if (t) tab_activate(g_active); }
    show_window();
    if (urls.empty() && active_tab() && active_tab()->url.empty()) ui_focus_address();
}
static void quit_now() {
    log_line(L"saliendo");
    g_quitting = true;
    SetPropW(g_win, L"saliendo", (HANDLE)1);   // a Pluma.exe started now waits for this one to go, then starts
    ui_close_popups();
    if (IsWindowVisible(g_win)) { save_placement(); session_save(); }
    history_flush();
    update_on_quit();
    SetTimer(g_win, kTimerQuit, 150, NULL);
}

static int frame_y(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}
static int frame_x(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}
static bool caption_code(int c) { return c == HTMINBUTTON || c == HTMAXBUTTON || c == HTCLOSE; }

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (msg == taskbar_created && msg) { ui_taskbar_badge(); return 0; }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            // no title bar: the tabs are the title bar. The side and bottom frames stay (they resize the window).
            if (g_fullscreen) return 0;
            auto *p = (NCCALCSIZE_PARAMS *)lp;
            int fx = frame_x(h), fy = frame_y(h);
            p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].bottom -= fy;
            if (IsZoomed(h)) p->rgrc[0].top += fy;   // maximized, the frame hangs off the screen
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        if (g_fullscreen) return HTCLIENT;
        LRESULT d = DefWindowProcW(h, msg, wp, lp);
        if (d != HTCLIENT) return d;
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        RECT rc; GetClientRect(h, &rc);
        int fy = frame_y(h);
        if (!IsZoomed(h) && p.y < fy) {
            if (p.x < fy * 2) return HTTOPLEFT;
            if (p.x >= rc.right - fy * 2) return HTTOPRIGHT;
            return HTTOP;
        }
        if (p.y < (LONG)ui_band_height()) return ui_hit_caption(p);
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
        ui_caption_hover(caption_code((int)wp) ? (int)wp : 0);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, h, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_NCMOUSELEAVE: ui_caption_hover(0); break;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
        if (caption_code((int)wp)) { g_caption_down = (int)wp; ui_caption_press((int)wp, true); return 0; }
        ui_close_popups();
        break;
    case WM_NCLBUTTONUP:
        if (caption_code((int)wp) || g_caption_down) {
            int down = g_caption_down;
            g_caption_down = 0;
            ui_caption_press(0, false);
            if ((int)wp == down) {
                if (down == HTMINBUTTON) ShowWindow(h, SW_MINIMIZE);
                else if (down == HTMAXBUTTON) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
                else if (down == HTCLOSE) PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        break;
    case WM_SIZE:
        ui_layout();
        tabs_layout_views();
        return 0;
    case WM_MOVE: case WM_MOVING:
        for (auto &t : g_tabs) if (t->ctrl) t->ctrl->NotifyParentWindowPositionChanged();
        ui_close_popups();
        video_layout();   // the player's controls are windows of their own: they follow
        break;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize = { 480, 320 }; return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ui_dpi_changed();
        return 0;
    }
    case WM_ACTIVATE:
        ui_window_activated(LOWORD(wp) != WA_INACTIVE);
        if (LOWORD(wp) != WA_INACTIVE && !g_incognito) profile_touch();
        break;
    case WM_SETFOCUS: {
        Tab *t = active_tab();
        if (!ui_address_focused() && video_focus()) return 0;
        if (t && t->ctrl && !t->url.empty() && !ui_address_focused()) t->ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        else if (t && t->url.empty()) ui_focus_address(false);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (handle_shortcut((UINT)wp, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0)) return 0;
        break;
    case WM_SETTINGCHANGE: {
        BOOL anim = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
        g_motion = anim != FALSE;
        if (lp && !wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") && g_set.theme == 2) { ui_theme_changed(); tabs_apply_theme(); }
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        const Palette &p = pal();
        HBRUSH b = CreateSolidBrush(RGB((int)(p.bg.r * 255), (int)(p.bg.g * 255), (int)(p.bg.b * 255)));
        FillRect(dc, &ps.rcPaint, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (g_quitting) break;
        // (the incognito window and other profiles' windows go for good: only the default one stays warm)
        if (g_set.resident && !g_incognito && g_profile_id.empty()) { hide_window(); return 0; }
        quit_now();
        return 0;
    case WM_COPYDATA: {
        auto cds = (COPYDATASTRUCT *)lp;
        if ((cds->dwData != kCopyOpen && cds->dwData != kCopyProfile && cds->dwData != kCopyOpenHere) || cds->cbData > (1 << 20)) return FALSE;
        std::wstring all((const wchar_t *)cds->lpData, cds->cbData / sizeof(wchar_t));
        std::vector<std::wstring> urls;
        size_t pos = 0;
        while (pos < all.size()) {
            size_t nl = all.find(L'\n', pos);
            std::wstring u = all.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
            if (!u.empty()) urls.push_back(u);
            if (nl == std::wstring::npos) break;
            pos = nl + 1;
        }
        if (cds->dwData == kCopyProfile) {
            // another profile's window asks for a profile's window: started from here, so it groups with this one
            if (urls.empty()) return TRUE;
            std::wstring id = urls[0] == L"default" ? L"" : urls[0];
            urls.erase(urls.begin());
            if (profile_valid_id(id) || id.empty()) profile_open(id, urls);
            return TRUE;
        }
        // links from other apps go to the profile used last (as in Edge)
        std::wstring last = cds->dwData == kCopyOpen && !urls.empty() && g_profile_id.empty() && !g_incognito ? profile_last_used() : L"";
        if (!last.empty()) { profile_open(last, urls); return TRUE; }
        open_urls(urls);
        return TRUE;
    }
    case WM_APP_SHOW: open_urls({}); return 0;
    case WM_APP_QUIT: if (g_profile_id.empty() && !g_incognito) profiles_quit_others(); quit_now(); return 0;
    case WM_APP + 40: profile_reload_state(); return 0;   // another process wrote this profile's history and bookmarks
    case WM_APP + 41: history_flush(); bookmarks_save(); return 1;   // ... is about to: what is in memory goes to disk first
    case WM_APP + 42: profiles_changed(); return 0;       // the list of profiles changed (names, colours)
    case WM_APP + 30: zoom_message(wp, lp); return 0;   // the fullscreen zoom (zoom.cpp)
    case WM_APP + 90: if (g_test) video_debug_shot(g_state + L"\\foto.png"); return 0;   // tests: a picture with the player's controls
    case WM_APP + 91:
        // tests: wp pictures, one every lp ms (foto-00.png...), to see a transition; with the time of each in the log
        if (g_test) { g_burst_left = (int)wp; g_burst_n = 0; g_burst_t0 = now_ms(); SetTimer(h, kTimerBurst, (UINT)lp, NULL); }
        return 0;
    case WM_TIMER:
        if (wp == kTimerQuit) { KillTimer(h, kTimerQuit); DestroyWindow(h); }
        else if (wp == kTimerHistory) { KillTimer(h, kTimerHistory); history_flush(); }
        else if (wp == kTimerTabs) tabs_tick();
        else if (wp == kTimerSession) { KillTimer(h, kTimerSession); session_save(); }
        else if (wp == kTimerTransitions) {
            KillTimer(h, kTimerTransitions);
            BOOL off = FALSE;
            if (!zoom_busy()) DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        }
        else if (wp == kTimerBurst) {
            wchar_t name[32]; swprintf_s(name, L"\\foto-%02d.png", g_burst_n);
            double t = now_ms() - g_burst_t0;
            video_debug_shot(g_state + name);
            log_line(L"foto " + std::to_wstring(g_burst_n) + L" a los " + std::to_wstring((int)t) + L" ms (pantalla completa " +
                     (g_fullscreen ? L"sí)" : L"no)"));
            g_burst_n++;
            if (--g_burst_left <= 0) KillTimer(h, kTimerBurst);
        }
        return 0;
    case WM_ENDSESSION:
        if (wp) { if (IsWindowVisible(h)) { save_placement(); session_save(); } history_flush(); }
        return 0;
    case WM_DESTROY:
        log_line(L"ventana destruida");
        g_quitting = true;
        for (auto &t : g_tabs) if (t->ctrl) t->ctrl->Close();
        g_tabs.clear();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void session_save_soon() { if (g_win) SetTimer(g_win, kTimerSession, 1500, NULL); }
void app_quit() { if (g_profile_id.empty() && !g_incognito) profiles_quit_others(); quit_now(); }
void app_close_window() { PostMessageW(g_win, WM_CLOSE, 0, 0); }
std::wstring app_name() {
    if (g_incognito) return std::wstring(L"Pluma (") + T(L"Incógnito") + L")";
    // with more than one profile, the window says which one it is (in the title and the taskbar)
    if (profiles_list().size() > 1) return L"Pluma (" + profile_shown_name(profile_current()) + L")";
    return L"Pluma";
}
void app_open_incognito(const std::wstring &url) {
    if (g_incognito) { tab_new(url, true); return; }
    // the incognito window is another Pluma.exe (--incognito); if it is already open, it takes the url
    spawn_pluma(L"--incognito" + (url.empty() ? std::wstring() : L" \"" + url + L"\""));
}
void app_restart(bool hidden) {
    // the new one waits for this one to go (the "saliendo" handover) and opens what there was: the session, or hidden
    // (another profile's window comes back as that profile)
    spawn_pluma(g_profile_id.empty() ? std::wstring(hidden ? L"--oculto" : L"") : L"--perfil=" + g_profile_id);
    quit_now();
}

static std::wstring arg_to_url(const std::wstring &a) {
    // a file from the Explorer becomes a file:// address; anything else is taken as typed
    if (a.size() > 2 && (a[1] == L':' || starts_with(a, L"\\\\")) && PathFileExistsW(a.c_str())) {
        wchar_t buf[4096]; DWORD n = 4096;
        if (SUCCEEDED(UrlCreateFromPathW(a.c_str(), buf, &n, 0))) return buf;
    }
    return a;
}

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
    bool quit = false, explicit_profile = false;   // (--perfil=default: the default window itself, not the one used last)
    std::vector<std::wstring> urls;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--oculto")) g_start_hidden = true;
        else if (!wcscmp(argv[i], L"--salir")) quit = true;
        else if (!wcscmp(argv[i], L"--incognito")) g_incognito = true;
        else if (!wcsncmp(argv[i], L"--perfil=", 9)) { explicit_profile = true; g_profile_id = lower(argv[i] + 9); if (g_profile_id == L"default" || !profile_valid_id(g_profile_id)) g_profile_id.clear(); }
        else if (argv[i][0] != L'-' || (argv[i][1] && argv[i][1] != L'-')) urls.push_back(arg_to_url(argv[i]));
    }
    LocalFree(argv);
    if (!urls.empty()) g_start_hidden = false;
    if (urls.empty()) g_open_asked = -1;   // the time to the first page is logged only when one was asked for

    wchar_t inst_name[64] = L"";
    std::wstring suffix;
    if (GetEnvironmentVariableW(L"PLUMA_INSTANCIA", inst_name, 64) > 0) { suffix = std::wstring(L".") + inst_name; g_test = true; }
    std::wstring state_suffix = suffix;
    // the incognito window: another Pluma of its own (own window, mutex and session), on the same engine and settings
    if (g_incognito) { suffix += L".incognito"; g_start_hidden = false; g_profile_id.clear(); }
    // another profile: a Pluma of its own too (window, mutex, files), on the same engine and settings; never hidden
    if (!g_profile_id.empty()) { suffix += L".perfil." + g_profile_id; g_start_hidden = false; }
    g_class += suffix; g_mutex += suffix;

    HANDLE mutex = CreateMutexW(NULL, FALSE, g_mutex.c_str());
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // one already in memory: hand over and leave. If it is closing for good (or still starting), wait: once it
        // is gone this one starts instead (opening Pluma right after quitting it used to do nothing)
        HWND other = NULL;
        bool primary = false;
        for (int i = 0; i < 250; i++) {
            other = FindWindowW(g_class.c_str(), NULL);
            if (other && !GetPropW(other, L"saliendo")) break;
            other = NULL;
            CloseHandle(mutex);
            mutex = CreateMutexW(NULL, FALSE, g_mutex.c_str());
            if (mutex && GetLastError() != ERROR_ALREADY_EXISTS) { primary = true; break; }
            Sleep(20);
        }
        if (!other && !primary) return 0;
        if (other) {
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            if (quit) PostMessageW(other, WM_APP_QUIT, 0, 0);
            else if (!urls.empty()) {
                std::wstring all;
                for (auto &u : urls) all += u + L"\n";
                COPYDATASTRUCT cds = { explicit_profile ? kCopyOpenHere : kCopyOpen, (DWORD)(all.size() * sizeof(wchar_t)), (PVOID)all.c_str() };
                DWORD_PTR res = 0;
                SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &res);
            } else if (!g_start_hidden) PostMessageW(other, WM_APP_SHOW, 0, 0);
            return 0;
        }
    }
    if (quit) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    g_root = exe;
    PWSTR local = NULL;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &local);
    g_state = std::wstring(local ? local : L".") + L"\\Pluma" + (state_suffix.empty() ? L"" : L"\\prueba" + state_suffix);
    CoTaskMemFree(local);
    SHCreateDirectoryExW(NULL, g_state.c_str(), NULL);
    if (!g_profile_id.empty()) {
        bool known = false;
        for (auto &p : profiles_list()) if (p.id == g_profile_id) known = true;
        if (!known) return 0;   // (removed meanwhile, or never made: nothing is created for it)
    }
    g_data = profile_data_dir(g_profile_id);
    SHCreateDirectoryExW(NULL, g_data.c_str(), NULL);
    wchar_t port[16] = L"";
    if (GetEnvironmentVariableW(L"PLUMA_DEPURAR", port, 16) > 0) g_dev = true;
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    g_motion = anim != FALSE;
    settings_load();
    // a link from another app, with nothing in memory: it goes to the profile used last (as in Edge)
    std::vector<std::wstring> for_profile;
    std::wstring last = !urls.empty() && !explicit_profile && g_profile_id.empty() && !g_incognito ? profile_last_used() : L"";
    if (!last.empty()) {
        if (!g_set.resident) { profile_launch_child(last, urls); return 0; }
        for_profile = urls; urls.clear(); g_start_hidden = true;   // this one waits in memory, as with --oculto
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = g_class.c_str();
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    g_win = CreateWindowExW(0, g_class.c_str(), L"Pluma", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            1200, 800, NULL, NULL, inst, NULL);
    if (!g_test) restore_placement();
    SetWindowPos(g_win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ui_init();
    video_init();
    SetTimer(g_win, kTimerTabs, 5000, NULL);
    if (!g_start_hidden) {
        if (!g_incognito) session_restore();   // the tabs are drawn at once; their pages come when the engine is up
        for (auto &u : urls) tab_new(u, true);
        if (g_tabs.empty()) tab_new(L"", true);
        show_window();
        if (active_tab() && active_tab()->url.empty()) ui_focus_address(false);
        log_line(L"ventana a la vista en " + std::to_wstring((int)now_ms()) + L" ms");
    }
    if (g_profile_id.empty() && !g_incognito) update_finish();   // (only the default one updates)
    if (!for_profile.empty()) profile_launch_child(last, for_profile);
    history_load();
    hidden_load();
    bookmarks_load();
    scripts_load();
    passwords_apply_pending();   // imported passwords waiting from last time: in before the engine opens Login Data
    ext_settings_apply_pending();   // and imported extensions' settings
    env_create([] {
        g_env_ready = true;
        log_line(L"motor listo en " + std::to_wstring((int)now_ms()) + L" ms");
        ext_sync([] { ui_layout(); });
        if (!g_tabs.empty()) tab_activate(g_active < 0 ? 0 : g_active);   // gives the one on screen its page
        if (!g_waiting_urls.empty()) { auto u = g_waiting_urls; g_waiting_urls.clear(); open_urls(u); }
        spare_prepare();
        update_init();
    });

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    // everything is saved by now: leave at once. Tearing the engine down politely made quitting take seconds, and a
    // Pluma opened meanwhile could find this one still there; the engine closes itself when its host is gone
    if (mutex) CloseHandle(mutex);
    ExitProcess((UINT)m.wParam);
}
