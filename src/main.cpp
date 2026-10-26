// Pluma: the window, its frame, settings, history, and starting up. See pluma.h.
#include "pluma.h"
#include <unordered_map>
#include <ctime>
#include <atomic>
#include <thread>

HINSTANCE g_inst;
std::vector<std::unique_ptr<Window>> g_windows;
static Window g_no_window;            // (before the first one is made: the current window is never null)
Window *g_cw = &g_no_window;
HWND g_app;
static std::vector<std::unique_ptr<Window>> g_gone;   // windows closed: freed at the next message, with nothing of theirs on the stack
static int g_next_window = 1;
std::wstring g_root, g_state, g_data, g_profile_id;
bool g_incognito, g_test, g_test_visible, g_dev, g_quitting, g_motion = true;
double g_open_asked = 0;
ComPtr<ICoreWebView2Environment> g_env;
Settings g_set;
std::vector<HistoryEntry> g_history;

static std::wstring g_class = L"PlumaVentana", g_mutex = L"PlumaNavegadorUnico";
static const UINT WM_APP_SHOW = WM_APP + 1, WM_APP_QUIT = WM_APP + 2, WM_APP_OPEN = WM_APP + 3, WM_APP_FRONT = WM_APP + 4;
static const ULONG_PTR kCopyOpen = 0x504C55;   // WM_COPYDATA from a second Pluma.exe: urls, one per line
static const ULONG_PTR kCopyProfile = 0x504C56;   // ... from another profile's window: "open this profile" (id, then urls)
static const ULONG_PTR kCopyOpenHere = 0x504C57;  // ... urls for this window itself (not routed to the last used profile)
static const UINT_PTR kTimerQuit = 1, kTimerHistory = 2, kTimerTabs = 3, kTimerSession = 4, kTimerBurst = 5, kTimerTransitions = 6;
static int g_burst_left, g_burst_n;   // tests: a burst of pictures
static double g_burst_t0;
static LARGE_INTEGER g_t0, g_freq;
static bool g_start_hidden, g_env_ready;
static std::vector<std::wstring> g_waiting_urls;   // asked for before the engine was ready

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
    // (never by default: a tab let go loses where it was and what was typed in it, and comes back loading; the user
    // found it "un coñazo", 26/10. Frozen, it keeps all; only a PC short of memory lets the oldest go. Saved before
    // ajustes_v 3, 60 s and 20 min were the old defaults, not a choice)
    bool v3 = _wtoi(ini_get(L"ajustes_v", L"1").c_str()) >= 3;
    g_set.discard_s = v3 ? std::max(0, _wtoi(ini_get(L"descargar_s", L"0").c_str())) : 0;
    if (g_set.discard_s > 0 && g_set.discard_s < 10) g_set.discard_s = 10;
    g_set.keep_recent = std::max(0, _wtoi(ini_get(L"recientes", L"2").c_str()));
    g_set.recent_discard_min = v3 ? std::max(0, _wtoi(ini_get(L"descargar_min", L"0").c_str())) : 0;
    g_set.spare = ini_get(L"reserva", L"0") == L"1";
    g_set.resident = ini_get(L"residente", L"1") != L"0";
    g_set.native_video = ini_get(L"video_propio", L"1") != L"0";
    g_set.youtube_lite = ini_get(L"youtube_ligero", L"1") != L"0";
    g_set.music_lite = ini_get(L"music_ligero", L"1") != L"0";
    g_set.reddit_lite = ini_get(L"reddit_ligero", L"1") != L"0";
    g_set.suggest_top = ini_get(L"sugerir_visitadas", L"1") != L"0";
    g_set.sound.preset = ini_get(L"efecto", L"no");
    sound_eq_parse(ini_get(L"eq", L""), g_set.sound.custom.eq);
    g_set.sound.custom.bass = _wtoi(ini_get(L"graves", L"0").c_str());
    g_set.sound.custom.reverb = _wtoi(ini_get(L"reverb", L"0").c_str());
    g_set.sound.pitch = _wtoi(ini_get(L"tono", L"0").c_str());
    sound_clamp(g_set.sound);
    g_set.suggest_passwords = ini_get(L"sugerir_contrasenas", L"1") != L"0";
    g_set.bookmarks_bar = ini_get(L"barra_marcadores", L"0") == L"1";
    g_set.reader_size = std::min(40, std::max(12, _wtoi(ini_get(L"lectura_letra", L"20").c_str())));
    // off unless turned on (26/10: "desactivado por defecto"); before ajustes_v 3 it was on by default, so a saved
    // "1" was never a choice
    g_set.tr_offer = _wtoi(ini_get(L"ajustes_v", L"1").c_str()) >= 3 && ini_get(L"traducir_ofrecer", L"0") == L"1";
    g_set.tr_target = trim(ini_get(L"traducir_a", L""));
    auto list = [](const std::wstring &v) {
        std::vector<std::wstring> out;
        size_t pos = 0;
        while (pos <= v.size()) {
            size_t c = v.find(L',', pos);
            std::wstring one = lower(trim(v.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos)));
            if (!one.empty() && std::find(out.begin(), out.end(), one) == out.end()) out.push_back(one);
            if (c == std::wstring::npos) break;
            pos = c + 1;
        }
        return out;
    };
    g_set.tr_always = list(ini_get(L"traducir_siempre", L""));
    g_set.tr_never = list(ini_get(L"traducir_nunca", L""));
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
    WritePrivateProfileStringW(L"pluma", L"ajustes_v", L"3", f.c_str());
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
    WritePrivateProfileStringW(L"pluma", L"reddit_ligero", g_set.reddit_lite ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"sugerir_visitadas", g_set.suggest_top ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"sugerir_contrasenas", g_set.suggest_passwords ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"lectura_letra", std::to_wstring(g_set.reader_size).c_str(), f.c_str());
    auto joined = [](const std::vector<std::wstring> &v) { std::wstring o; for (auto &x : v) o += (o.empty() ? L"" : L",") + x; return o; };
    WritePrivateProfileStringW(L"pluma", L"traducir_ofrecer", g_set.tr_offer ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"pluma", L"traducir_a", g_set.tr_target.c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"traducir_siempre", joined(g_set.tr_always).c_str(), f.c_str());
    WritePrivateProfileStringW(L"pluma", L"traducir_nunca", joined(g_set.tr_never).c_str(), f.c_str());
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
    SetTimer(g_app, kTimerHistory, 4000, NULL);
}
void history_title(const std::wstring &url, const std::wstring &title) {
    auto it = g_hist_index.find(url);
    if (it == g_hist_index.end() || title.empty() || g_history[it->second].title == title) return;
    g_history[it->second].title = title;
    g_hist_dirty = true;
    SetTimer(g_app, kTimerHistory, 4000, NULL);
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

// ---------------------------------------------------------------- the windows
WinScope::~WinScope() {
    // back to the one before; if it went meanwhile (closed during the scope), to the one used last
    g_cw = win_alive(saved) ? saved : !g_windows.empty() ? win_last_used() : g_cw;
}
bool win_alive(const Window *w) {
    for (auto &x : g_windows) if (x.get() == w) return true;
    return false;
}
Window *win_of_hwnd(HWND h) {
    // the window itself, or one of its children (the bar, the address, the views' own), or a popup it owns
    for (int depth = 0; h && depth < 8; depth++) {
        for (auto &w : g_windows) if (w->hwnd == h) return w.get();
        h = GetParent(h);   // (the parent of a child, the owner of a popup)
    }
    return nullptr;
}
Window *win_of_tab(const Tab *t) {
    if (!t) return nullptr;
    for (auto &w : g_windows) for (auto &x : w->tabs) if (x.get() == t) return w.get();
    return nullptr;
}
Window *win_by_id(int id) {
    for (auto &w : g_windows) if (w->id == id) return w.get();
    return nullptr;
}
Window *win_last_used() {
    Window *best = nullptr;
    for (auto &w : g_windows) {
        bool vis = IsWindowVisible(w->hwnd) != FALSE;
        bool best_vis = best && IsWindowVisible(best->hwnd);
        if (!best || (vis && !best_vis) || (vis == best_vis && w->used > best->used)) best = w.get();
    }
    return best ? best : g_cw;
}
bool any_window_visible() {
    for (auto &w : g_windows) if (IsWindowVisible(w->hwnd) && !IsIconic(w->hwnd)) return true;
    return false;
}
std::vector<Tab *> all_tabs() {
    std::vector<Tab *> out;
    for (auto &w : g_windows) for (auto &t : w->tabs) out.push_back(t.get());
    return out;
}
std::wstring win_name(const Window *w) {
    // as Edge's "Move tab to": the window's tab on screen says which one it is
    std::wstring title;
    if (w && w->active >= 0 && w->active < (int)w->tabs.size()) {
        const Tab *t = w->tabs[w->active].get();
        title = !t->title.empty() ? t->title : t->url.empty() ? T(L"Pestaña nueva") : url_host(t->url);
    }
    if (title.size() > 40) title = title.substr(0, 39) + L"…";
    int k = 1;   // (its place among the open ones, not its id: "Ventana 2" while there are two)
    for (auto &o : g_windows) { if (o.get() == w) break; k++; }
    std::wstring n = std::wstring(T(L"Ventana")) + L" " + std::to_wstring(k);
    return title.empty() ? n : n + L": " + title;
}
static void save_placement() {
    if (g_incognito) return;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (g_fullscreen) wp = g_cw->before_fs;
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
        g_cw->before_fs.length = sizeof(g_cw->before_fs);
        GetWindowPlacement(g_win, &g_cw->before_fs);
        GetWindowRect(g_win, &g_cw->test_rect);
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
        RECT &tr = g_cw->test_rect;
        if (g_test) SetWindowPos(g_win, HWND_BOTTOM, tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top, SWP_NOACTIVATE | (g_test_visible ? SWP_NOZORDER : 0));
        else SetWindowPlacement(g_win, &g_cw->before_fs);
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
static void test_place(Window *w) {
    // a test instance draws for real but off the screen, without taking the focus from the user; its windows one
    // under the other (a window placed already off the screen, as one dragged out by a test, stays where it is)
    int ww = 1400, h = 900, x = -7000, y = 100;
    wchar_t size[32] = L"", at[32] = L"";
    if (GetEnvironmentVariableW(L"PLUMA_TAMANO", size, 32)) swscanf_s(size, L"%dx%d", &ww, &h);   // e.g. 1920x1040
    // PLUMA_A_LA_VISTA=x,y: on the screen, over the rest (to record a transition as the user sees it)
    if (GetEnvironmentVariableW(L"PLUMA_A_LA_VISTA", at, 32) && swscanf_s(at, L"%d,%d", &x, &y) == 2) g_test_visible = true;
    RECT r; GetWindowRect(w->hwnd, &r);
    if (!g_test_visible && r.left < -3000 && IsWindowVisible(w->hwnd)) return;
    if (!g_test_visible && r.left < -3000 && GetPropW(w->hwnd, L"colocada")) { SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); return; }
    int k = 0;
    for (auto &o : g_windows) { if (o.get() == w) break; k++; }
    if (g_test_visible) { x += k * 60; y += k * 60; } else y += k * (h + 60);
    SetWindowPos(w->hwnd, g_test_visible ? HWND_TOPMOST : HWND_BOTTOM, x, y, ww, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void win_show(Window *w, bool activate) {
    if (!w) return;
    WinScope ws(w);
    if (g_test) { test_place(w); return; }
    if (!IsWindowVisible(g_win)) ShowWindow(g_win, GetPropW(g_win, L"max") ? SW_SHOWMAXIMIZED : activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    else if (IsIconic(g_win)) ShowWindow(g_win, SW_RESTORE);
    if (activate) SetForegroundWindow(g_win);
}
static void show_window() { g_start_hidden = false; win_show(g_cw, true); }
static void show_all_windows() {
    // (the session's windows: the first one in front)
    g_start_hidden = false;
    for (size_t i = g_windows.size(); i-- > 1;) win_show(g_windows[i].get(), false);
    if (!g_windows.empty()) win_show(g_windows[0].get(), true);
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
    if (!g_incognito) sync_on_close();   // back up now if due (the session and history are already saved)
}
static void open_urls(const std::vector<std::wstring> &urls) {
    // what comes from outside (a link, Pluma opened again) goes to the window used last
    WinScope ws(win_last_used());
    if (!g_env_ready) { for (auto &u : urls) g_waiting_urls.push_back(u); show_window(); return; }
    bool was_hidden = !any_window_visible();
    if (was_hidden) g_open_asked = now_ms();
    if (was_hidden && g_tabs.empty() && !g_incognito) session_restore();
    for (auto &u : urls) tab_new(u, true);
    if (urls.empty() && g_tabs.empty()) tab_new(L"", true);
    if (urls.empty() && was_hidden) { Tab *t = active_tab(); if (t) tab_activate(g_active); }
    if (was_hidden)   // (started hidden with more windows in the session: they all come)
        for (auto &w : g_windows)
            if (w.get() != g_cw && !IsWindowVisible(w->hwnd) && !w->tabs.empty()) { win_show(w.get(), false); WinScope ws2(w.get()); tab_activate(g_active); }
    show_window();
    if (urls.empty() && active_tab() && active_tab()->url.empty()) ui_focus_address();
}
static void quit_now() {
    log_line(L"saliendo");
    g_quitting = true;
    for (auto &w : g_windows) SetPropW(w->hwnd, L"saliendo", (HANDLE)1);   // a Pluma.exe started now waits for this one to go, then starts
    ui_close_popups();
    if (any_window_visible()) {
        WinScope ws(g_windows.empty() ? nullptr : g_windows[0].get());
        save_placement();
        session_save();
    }
    history_flush();
    update_on_quit();
    if (!g_incognito) sync_on_close();   // a last backup if due; wWinMain waits for it before leaving
    SetTimer(g_app, kTimerQuit, 150, NULL);
}
void win_close(Window *w) {
    // one of several windows: its tabs go to the recently closed (as Edge's), its views and players with them
    if (!win_alive(w) || g_windows.size() < 2) return;
    {
        WinScope ws(w);
        ui_close_popups();
        zoom_stop();
        tabs_close_window();
        views_leave_window(g_win);
        ui_window_free();
    }
    HWND h = w->hwnd;
    for (auto it = g_windows.begin(); it != g_windows.end(); ++it)
        if (it->get() == w) { g_gone.push_back(std::move(*it)); g_windows.erase(it); break; }   // (freed at the next message)
    if (g_cw == w) g_cw = win_last_used();
    DestroyWindow(h);
    log_line(L"ventana cerrada; quedan " + std::to_wstring(g_windows.size()));
    session_save_soon();
}
static Window *make_window(const RECT *where) {
    auto up = std::make_unique<Window>();
    Window *w = up.get();
    w->id = g_next_window++;
    w->used = now_ms();
    g_windows.push_back(std::move(up));
    WinScope ws(w);
    RECT r = where ? *where : RECT{ CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 1200, CW_USEDEFAULT + 800 };
    CreateWindowExW(0, g_class.c_str(), app_name().c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, r.left, r.top,
                    where ? r.right - r.left : 1200, where ? r.bottom - r.top : 800, NULL, NULL, g_inst, w);
    return w;
}
Window *win_new(const RECT *where) {
    // a new window (Ctrl+N, a tab taken out): by default as big as the current one and a step down and right (as Edge)
    RECT r{};
    bool max = false;
    if (where) r = *where;
    else if (g_win) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(g_win, &wp);
        max = IsZoomed(g_win) != FALSE;
        if (IsZoomed(g_win) || IsIconic(g_win)) r = wp.rcNormalPosition; else GetWindowRect(g_win, &r);
        int step = (int)(30 * GetDpiForWindow(g_win) / 96);
        OffsetRect(&r, step, step);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
        if (r.bottom > mi.rcWork.bottom || r.right > mi.rcWork.right) OffsetRect(&r, mi.rcWork.left - r.left + step, mi.rcWork.top - r.top + step);
    } else SetRect(&r, 100, 100, 1300, 900);
    Window *w = make_window(&r);
    WinScope ws(w);
    SetWindowPos(g_win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (max) SetPropW(g_win, L"max", (HANDLE)1);
    ui_window_init();
    log_line(L"ventana nueva (" + std::to_wstring(g_windows.size()) + L" abiertas)");
    return w;
}
void win_open_new(const std::wstring &url) {
    // the incognito window's "new window" is one more of its own (all its tabs are incognito anyway)
    Window *w = win_new(nullptr);
    {
        WinScope ws(w);
        tab_new(url, true);
        win_show(w, true);
        if (url.empty()) ui_focus_address(false);
    }
    session_save_soon();
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

// tests: "Pluma.exe pluma-prueba:<what>" drives Pluma without a mouse (tools\traducir-prueba.ps1, tools\ventanas.ps1)
static int g_test_window;   // ventana=<n>: the window the next ones work on (0: the one used last)
static void test_command(const std::wstring &what) {
    Window *tw = win_by_id(g_test_window);
    WinScope ws(tw ? tw : win_last_used());
    Tab *t = active_tab();
    if (what == L"traducir") translate_page(t);
    else if (what == L"original") translate_original(t);
    else if (what == L"seleccion") translate_selection(t, L"", false, POINT{});
    else if (what == L"menu") ui_open_menu_quietly(true);
    else if (what == L"menu-principal") ui_open_menu_quietly(false);
    else if (what == L"menu-pestana") ui_open_tab_menu_quietly();
    else if (what == L"sonido") ui_open_sound_panel();   // ⋯ > Sonido
    else if (what == L"perfil") ui_open_profile_menu(true);
    else if (what == L"oferta") ui_translate_offer(t);
    else if (starts_with(what, L"tarjeta=")) ui_translate_card_test(what.substr(8));
    else if (starts_with(what, L"automatica=")) { g_set.tr_offer = what.substr(11) == L"1"; settings_save(); ui_invalidate(); }
    else if (what == L"clic-derecho") translate_test_pick_menu();   // the next page menu: logged, Pluma's row chosen
    else if (starts_with(what, L"destino=")) translate_set_target(what.substr(8));
    else if (what == L"descargas") ui_open_downloads(true);
    else if (starts_with(what, L"clave") || what == L"sembrar" || what == L"cancelar" || what == L"lista") vault_test(what);   // saved passwords (vault.cpp)
    else if (starts_with(what, L"descarga-")) {   // descarga-abrir|carpeta|cancelar=<n>: the n-th row of the downloads panel
        size_t eq = what.find(L'=');
        auto rows = downloads_rows();
        int n = eq == std::wstring::npos ? 0 : _wtoi(what.c_str() + eq + 1);
        if (n >= 1 && n <= (int)rows.size()) {
            std::wstring verb = what.substr(9, eq - 9);
            if (verb == L"abrir") download_open(rows[n - 1].id);
            else if (verb == L"carpeta") download_show(rows[n - 1].id);
            else if (verb == L"cancelar") download_cancel(rows[n - 1].id);
        }
    }
    else if (starts_with(what, L"ventana=")) g_test_window = _wtoi(what.c_str() + 8);
    else if (what == L"ventana-nueva") { win_open_new(); g_test_window = g_windows.back()->id; }
    else if (what == L"mover-nueva") { Window *n = t ? tab_to_new_window(t, nullptr) : nullptr; if (n) g_test_window = n->id; }
    else if (starts_with(what, L"mover=")) { Window *to = win_by_id(_wtoi(what.c_str() + 6)); if (t && to) tab_move(t, to, (int)to->tabs.size(), true); }
    else if (starts_with(what, L"arrastre-empieza=")) ui_test_drag(0, _wtoi(what.c_str() + 17), 0, 0);
    else if (starts_with(what, L"arrastre=")) { int dx = 0, dy = 0; swscanf_s(what.c_str() + 9, L"%d,%d", &dx, &dy); ui_test_drag(1, 0, dx, dy); }
    else if (starts_with(what, L"arrastre-a=")) { int n = 0, x = 0, y = 0; swscanf_s(what.c_str() + 11, L"%d,%d,%d", &n, &x, &y); ui_test_drag(2, n, x, y); }
    else if (what == L"arrastre-suelta") ui_test_drag(3, 0, 0, 0);
    else if (what == L"cerrar") PostMessageW(g_win, WM_CLOSE, 0, 0);   // its X
    else if (what == L"pos") log_line(L"prueba: vídeo en " + std::to_wstring(video_position(t)) + L" s" + (video_playing(t) ? L", sonando" : L""));
    else if (what == L"ventanas") {
        for (auto &w : g_windows) {
            std::wstring line = L"prueba: ventana " + std::to_wstring(w->id) + (IsWindowVisible(w->hwnd) ? L" visible" : L" oculta") +
                                L", activa " + std::to_wstring(w->active) + L":";
            for (auto &x : w->tabs) line += L" [" + x->url.substr(0, 60) + L"]";
            log_line(line);
        }
    } else if (starts_with(what, L"foto=")) {   // a picture with Pluma's panels as the screen shows them
        std::wstring name = what.substr(5);
        bool safe = !name.empty() && name.size() < 40;
        for (wchar_t c : name) if (!iswalnum(c) && c != L'-') safe = false;
        if (safe) video_debug_shot(g_state + L"\\" + name + L".png");
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (msg == WM_NCCREATE) {
        Window *nw = (Window *)((CREATESTRUCTW *)lp)->lpCreateParams;
        if (nw) nw->hwnd = h;
    }
    Window *self = win_of_hwnd(h);
    if (!self) return DefWindowProcW(h, msg, wp, lp);   // (before it is made, or once it is closing)
    WinScope ws(self);
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
        if (caption_code((int)wp)) { g_cw->caption_down = (int)wp; ui_caption_press((int)wp, true); return 0; }
        ui_close_popups();
        break;
    case WM_NCLBUTTONUP:
        if (caption_code((int)wp) || g_cw->caption_down) {
            int down = g_cw->caption_down;
            g_cw->caption_down = 0;
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
        // (a window following a dragged tab keeps where the drag puts it: only its size changes)
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE | (g_cw->dragged ? SWP_NOMOVE : 0));
        ui_dpi_changed();
        return 0;
    }
    case WM_ACTIVATE:
        ui_window_activated(LOWORD(wp) != WA_INACTIVE);
        if (LOWORD(wp) != WA_INACTIVE) { g_cw->used = now_ms(); if (!g_incognito) profile_touch(); }
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
        // (every window gets it: the theme changes once, with the first)
        if (lp && !wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") && g_set.theme == 2 && self == g_windows[0].get()) { ui_theme_changed(); tabs_apply_theme(); }
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
    case WM_CLOSE: {
        if (g_quitting) break;
        // one of several windows: it goes (its tabs to the recently closed, as in Edge)
        int open = 0;
        for (auto &w : g_windows) if (IsWindowVisible(w->hwnd)) open++;
        if (open > 1 || (g_windows.size() > 1 && !IsWindowVisible(h))) { win_close(self); return 0; }
        // the last one: (the incognito window and other profiles' windows go for good: only the default one stays warm)
        if (g_set.resident && !g_incognito && g_profile_id.empty()) {
            // (hidden windows left by nobody: only this one stays, to come back)
            for (size_t i = g_windows.size(); i-- > 0;) if (g_windows[i].get() != self) win_close(g_windows[i].get());
            hide_window();
            return 0;
        }
        quit_now();
        return 0;
    }
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
        // test instances: "Pluma.exe pluma-prueba:<what>" drives Pluma without a mouse
        if (g_test && !urls.empty() && starts_with(urls[0], L"pluma-prueba:")) { test_command(urls[0].substr(13)); return TRUE; }
        // links from other apps go to the profile used last (as in Edge)
        std::wstring last = cds->dwData == kCopyOpen && !urls.empty() && g_profile_id.empty() && !g_incognito ? profile_last_used() : L"";
        if (!last.empty()) { profile_open(last, urls); return TRUE; }
        open_urls(urls);
        return TRUE;
    }
    case WM_APP_SHOW:
        // Pluma opened again (its icon, the taskbar's middle click) with a window on screen: a new window, as Edge;
        // else the window comes back
        if (any_window_visible() && g_env_ready) win_open_new(); else open_urls({});
        return 0;
    case WM_APP_FRONT: open_urls({}); return 0;   // another profile asks for this one's window: to the front
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
        if (wp == kTimerTransitions) {
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
        // (every window gets it: the session, with all of them, is saved once)
        if (wp && self == g_windows[0].get()) { if (any_window_visible()) { save_placement(); session_save(); } history_flush(); }
        return 0;
    case WM_DESTROY:
        if (g_quitting) {
            for (auto &t : g_tabs) if (t->ctrl) t->ctrl->Close();
            g_tabs.clear();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
// Pluma's own window, never shown: what is the app's and not a window's (timers, posts from other threads)
static LRESULT CALLBACK app_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == kTimerQuit) {
            KillTimer(h, kTimerQuit);
            log_line(L"ventanas destruidas");
            std::vector<HWND> all;
            for (auto &w : g_windows) all.push_back(w->hwnd);
            for (HWND x : all) DestroyWindow(x);
            PostQuitMessage(0);
        }
        else if (wp == kTimerHistory) { KillTimer(h, kTimerHistory); history_flush(); }
        else if (wp == kTimerTabs) tabs_tick();
        else if (wp == kTimerSession) { KillTimer(h, kTimerSession); session_save(); }
        return 0;
    case WM_APP + 21: {   // ui_post: something from another thread, on this one (in the window used last)
        auto f = (std::function<void()> *)lp;
        { WinScope ws(win_last_used()); (*f)(); }
        delete f;
        return 0;
    }
    case WM_APP + 22: ui_frame(); return 0;   // one animation frame for every window that moves (ui.cpp)
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void session_save_soon() { if (g_app) SetTimer(g_app, kTimerSession, 1500, NULL); }
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
    // (another profile's window comes back as that profile). Marked as leaving before it starts: a new one quicker
    // than quit_now found the window unmarked, handed itself over to it and ended, and nothing came back (a restore
    // was then applied only at the next start)
    for (auto &w : g_windows) SetPropW(w->hwnd, L"saliendo", (HANDLE)1);
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

// the file types Pluma opens: listed in "Open with" for each, so Windows offers Pluma for them (instalar.ps1 registers
// Pluma as a browser, but an update doesn't run it again)
static const wchar_t *kOpenTypes[] = { L".htm", L".html", L".shtml", L".xht", L".xhtml", L".svg", L".webp", L".pdf" };
static void register_open_with() {
    for (auto e : kOpenTypes) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + e + L"\\OpenWithProgids").c_str(), 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) continue;
        RegSetValueExW(k, L"PlumaHTML", 0, REG_NONE, NULL, 0);
        RegCloseKey(k);
    }
}
// a type Windows hands to a program that is no longer there (Edge uninstalled leaves http, .html and .pdf on its
// MSEdgeHTM / MSEdgePDF): opening a PDF or a link from another app then does nothing at all
static bool handler_missing(const wchar_t *choice_key) {
    // Windows 11 keeps the choice in UserChoiceLatest\ProgId; UserChoice can still name the old program
    wchar_t prog[128] = L""; DWORD n = sizeof(prog);
    std::wstring latest = std::wstring(choice_key) + L"Latest\\ProgId";
    if (RegGetValueW(HKEY_CURRENT_USER, latest.c_str(), L"ProgId", RRF_RT_REG_SZ, NULL, prog, &n) != ERROR_SUCCESS || !*prog) {
        n = sizeof(prog);
        if (RegGetValueW(HKEY_CURRENT_USER, choice_key, L"ProgId", RRF_RT_REG_SZ, NULL, prog, &n) != ERROR_SUCCESS || !*prog) return false;
    }
    if (!wcscmp(prog, L"PlumaHTML")) return false;
    wchar_t cmd[2048] = L""; n = sizeof(cmd);
    if (RegGetValueW(HKEY_CLASSES_ROOT, (std::wstring(prog) + L"\\shell\\open\\command").c_str(), NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, cmd, &n) != ERROR_SUCCESS)
        return true;
    // the program is the first token of the command, quoted or not
    std::wstring c = cmd, exe;
    if (!c.empty() && c[0] == L'"') { size_t e = c.find(L'"', 1); exe = c.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1); }
    else { size_t e = c.find(L".exe"); exe = e == std::wstring::npos ? c.substr(0, c.find(L' ')) : c.substr(0, e + 4); }
    wchar_t full[MAX_PATH] = L"";
    ExpandEnvironmentStringsW(exe.c_str(), full, MAX_PATH);
    return *full && GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES;
}
static void check_default_handlers() {
    static const wchar_t *keys[] = {
        L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice",
        L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.html\\UserChoice",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.htm\\UserChoice" };
    for (auto k : keys) if (handler_missing(k)) {
        log_line(L"tipos sin programa: los PDF o los enlaces apuntan a uno que ya no está");
        // from Pluma, on a click: the settings window comes to the front (opened from elsewhere it stayed behind)
        ui_notice(T(L"Los PDF o los enlaces se abren con un programa que ya no está instalado."), T(L"Abrir con Pluma"),
                  [] { ShellExecuteW(NULL, L"open", L"ms-settings:defaultapps?registeredAppUser=Pluma", NULL, NULL, SW_SHOWNORMAL); }, 0);
        return;
    }
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
    if (GetEnvironmentVariableW(L"PLUMA_INSTANCIA", inst_name, 64) > 0) {
        // a name only (1-32 letters, digits, - or _): its data folder can never be another one, the user's included
        // (test-only shortcuts, such as the saved passwords' verification answered without asking, rely on it)
        size_t n = wcslen(inst_name);
        bool ok = n >= 1 && n <= 32;
        for (size_t i = 0; i < n; i++) if (!iswalnum(inst_name[i]) && inst_name[i] != L'-' && inst_name[i] != L'_') ok = false;
        if (ok) { suffix = std::wstring(L".") + inst_name; g_test = true; }
    }
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
    bool restored = !g_incognito && sync_apply_restore();   // a staged restore: the old data aside, the new in, before anything is read
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
    bool first_run = !restored && !PathFileExistsW((g_state + L"\\ajustes.ini").c_str());   // before settings_load: offer to restore
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
    WNDCLASSEXW ac = { sizeof(ac) };
    ac.lpfnWndProc = app_proc;
    ac.hInstance = inst;
    ac.lpszClassName = L"PlumaApp";
    RegisterClassExW(&ac);
    g_app = CreateWindowExW(0, L"PlumaApp", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    make_window(nullptr);
    if (!g_test) restore_placement();
    SetWindowPos(g_win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ui_init();
    ui_window_init();
    video_init();
    SetTimer(g_app, kTimerTabs, 5000, NULL);
    if (!g_start_hidden) {
        if (!g_incognito) session_restore();   // the tabs are drawn at once; their pages come when the engine is up
        WinScope first(g_windows[0].get());
        for (auto &u : urls) tab_new(u, true);
        if (g_tabs.empty()) tab_new(L"", true);
        show_all_windows();
        if (active_tab() && active_tab()->url.empty()) ui_focus_address(false);
        log_line(L"ventana a la vista en " + std::to_wstring((int)now_ms()) + L" ms" +
                 (g_windows.size() > 1 ? L" (" + std::to_wstring(g_windows.size()) + L" ventanas)" : L""));
    }
    if (g_profile_id.empty() && !g_incognito) update_finish();   // (only the default one updates)
    if (!for_profile.empty()) profile_launch_child(last, for_profile);
    history_load();
    hidden_load();
    bookmarks_load();
    scripts_load();
    passwords_apply_pending();   // imported passwords waiting from last time: in before the engine opens Login Data
    // the engine has not opened its cookies yet: the backup (and the saved passwords' list, if the live file is locked)
    // reads this copy; after the pending passwords, so it has them
    if (!g_incognito) sync_snapshot_engine();
    if (!g_incognito && g_profile_id.empty()) sync_init();   // one worker, in the default profile's process: it backs up every profile   // the backup worker (hourly if changed, and on close)
    // first time: offer to bring a backup back (test instances only with PLUMA_SYNC_PRIMERA=1)
    if (first_run && !g_incognito && !g_start_hidden && (!g_test || GetEnvironmentVariableW(L"PLUMA_SYNC_PRIMERA", NULL, 0) > 0))
        ui_notice(T(L"¿Tienes una copia de Pluma? Puedes restaurarla."), T(L"Restaurar"), [] { settings_open_sync(); }, 0);
    if (!g_incognito && !g_test && g_profile_id.empty()) {
        register_open_with();
        if (!g_start_hidden) check_default_handlers();
    }
    ext_settings_apply_pending();   // and imported extensions' settings
    env_create([] {
        g_env_ready = true;
        log_line(L"motor listo en " + std::to_wstring((int)now_ms()) + L" ms");
        ext_sync([] { ui_layout(); });
        for (auto &w : g_windows) { WinScope ws(w.get()); if (!g_tabs.empty()) tab_activate(g_active < 0 ? 0 : g_active); }   // gives the ones on screen their page
        if (!g_waiting_urls.empty()) { auto u = g_waiting_urls; g_waiting_urls.clear(); open_urls(u); }
        spare_prepare();
        update_init();
    });

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (!g_gone.empty()) g_gone.clear();   // (nothing of a closed window is on the stack here)
        // the current window: the one this message is for (a TIMERPROC of a window gets its window too)
        if (Window *w = win_of_hwnd(m.hwnd)) g_cw = w;
        else if (!win_alive(g_cw) && !g_windows.empty()) g_cw = win_last_used();
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // everything is saved by now: leave at once. Tearing the engine down politely made quitting take seconds, and a
    // Pluma opened meanwhile could find this one still there; the engine closes itself when its host is gone
    if (mutex) CloseHandle(mutex);
    if (!g_incognito) { sync_wait_idle(120000); sync_shutdown(); }   // let a backup in progress finish before leaving (the mutex is gone: a new Pluma can start meanwhile)
    ExitProcess((UINT)m.wParam);
}
