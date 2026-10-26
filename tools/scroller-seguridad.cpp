// Pluma: the Scroller's gate between a page and mpv (src/scroller_core.h), tested. A page is hostile: it must not get
// mpv to open a local file, one of mpv's own protocols, or a machine of the local network (by its name, or through a
// redirect). Then, with Pluma's own libmpv and the Scroller's options, the same through a server: an HLS list naming a
// local file, a plain playlist naming one, a redirect to a local file and a redirect at all (mpv follows none).
//   cl /nologo /EHsc /std:c++17 /utf-8 /I src tools\scroller-seguridad.cpp ws2_32.lib
//   scroller-seguridad.exe [<motor-video\libmpv-2.dll> <a small .mp4>]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include "scroller_core.h"

static int fails = 0;
#define CHECK(c, what) do { if (c) printf("ok    %s\n", what); else { printf("FALLA %s\n", what); fails++; } } while (0)

static void gate() {
    using namespace scroll;
    CHECK(url_ok("https://cdn.example.com/a.mp4", "example.com"), "https normal");
    CHECK(!url_ok("file:///C:/Windows/win.ini", "example.com"), "file:");
    CHECK(!url_ok("edl://%10%C:/x.mp4,0,1", "example.com"), "edl://");
    CHECK(!url_ok("lavfi://testsrc", "example.com"), "lavfi://");
    CHECK(!url_ok("av://dshow:video=x", "example.com"), "av://");
    CHECK(!url_ok("memory://abc", "example.com"), "memory://");
    CHECK(!url_ok("C:\\Windows\\win.ini", "example.com"), "ruta local");
    CHECK(!url_ok("\\\\server\\share\\a.mp4", "example.com"), "ruta de red");
    CHECK(!url_ok("https://cdn.example.com/a.mp4 ,stream-record=C:\\x", "example.com"), "espacio y opciones");
    CHECK(!url_ok("http://127.0.0.1/a.mp4", "example.com"), "127.0.0.1 desde internet");
    CHECK(!url_ok("http://192.168.1.1/a.mp4", "example.com"), "192.168 desde internet");
    CHECK(!url_ok("http://2130706433/a.mp4", "example.com"), "IPv4 en decimal");
    CHECK(!url_ok("http://0x7f.0.0.1/a.mp4", "example.com"), "IPv4 en hexadecimal");
    CHECK(!url_ok("http://[::1]/a.mp4", "example.com"), "IPv6 ::1");
    CHECK(!url_ok("http://router.local/a.mp4", "example.com"), ".local");
    CHECK(!url_ok("http://intranet/a.mp4", "example.com"), "nombre sin punto");
    CHECK(url_ok("http://127.0.0.1:8080/a.mp4", "127.0.0.1"), "una página local, sus vídeos locales");
    CHECK(url_ok("http://127.0.0.1:8080/a.mp4", "127.0.0.1:8080"), "una página local con su puerto");
    CHECK(!url_ok("http://127.0.0.1/a.mp4", "example.com:8443"), "127.0.0.1 desde internet con puerto");
    CHECK(!header_ok("a\r\nX-Evil: 1"), "cabecera con salto de línea");
    std::string out;
    CHECK(!safe_list("#EXTM3U\n#EXTINF:4,\nfile:///C:/x.ts\n", "https://cdn.example.com/l.m3u8", "example.com", out), "lista con file:");
    CHECK(!safe_list("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"file:///C:/k\"\n#EXTINF:4,\na.ts\n", "https://cdn.example.com/l.m3u8", "example.com", out), "clave con file:");
    CHECK(!safe_list("#EXTM3U\n#EXT-X-MAP:URI=\"http://10.0.0.1/i.mp4\"\n#EXTINF:4,\na.ts\n", "https://cdn.example.com/l.m3u8", "example.com", out), "red local en una etiqueta");
    CHECK(safe_list("#EXTM3U\n#EXTINF:4,\na.ts\n", "https://cdn.example.com/x/l.m3u8", "example.com", out) && out.find("https://cdn.example.com/x/a.ts") != std::string::npos,
          "lista normal, direcciones completas");
    std::string o = loadfile_options(false, 0);
    CHECK(o.find("demuxer=lavf") != std::string::npos && o.find("protocol_whitelist=[http,https,tls,tcp,crypto,httpproxy]") != std::string::npos &&
          o.find("curl-max-redirects=0") != std::string::npos, "opciones de mpv: lavf, sin file, sin redirecciones");
    // redirects and names, hop by hop (a stand-in network: names to addresses, addresses to answers)
    auto fake_vet = [](const std::string &h) { return h != "parece-publico.example" && h != "127.0.0.1"; };   // (that name is 127.0.0.1)
    auto fetch = [](const std::string &u, bool) {
        Hop h;
        if (u == "https://a.example/v.mp4") { h.status = 302; h.location = "https://b.example/v.mp4"; }
        else if (u == "https://b.example/v.mp4") h.status = 206;
        else if (u == "https://c.example/v.mp4") { h.status = 302; h.location = "http://127.0.0.1/v.mp4"; }
        else if (u == "https://d.example/v.mp4") { h.status = 301; h.location = "file:///C:/Windows/win.ini"; }
        else if (u == "https://e.example/v.mp4") { h.status = 302; h.location = "https://parece-publico.example/v.mp4"; }
        else if (u == "https://f.example/v.mp4") { h.status = 302; h.location = "https://f.example/v.mp4"; }
        else h.status = 200;
        return h;
    };
    CHECK(vet_chain("https://a.example/v.mp4", "example.com", "", fetch, fake_vet) == "https://b.example/v.mp4", "redirección normal, seguida");
    CHECK(vet_chain("https://c.example/v.mp4", "example.com", "", fetch, fake_vet).empty(), "redirección a 127.0.0.1");
    CHECK(vet_chain("https://d.example/v.mp4", "example.com", "", fetch, fake_vet).empty(), "redirección a file:");
    CHECK(vet_chain("https://e.example/v.mp4", "example.com", "", fetch, fake_vet).empty(), "redirección a un nombre que es 127.0.0.1");
    CHECK(vet_chain("https://parece-publico.example/v.mp4", "example.com", "", fetch, fake_vet).empty(), "un nombre que es 127.0.0.1");
    CHECK(vet_chain("https://f.example/v.mp4", "example.com", "", fetch, fake_vet).empty(), "redirecciones sin fin");
    // the page's cookies: only to their own host, never after a hop to another one (not even back), and to mpv only if
    // everything it will ask for is there
    {
        std::vector<std::pair<std::string, bool>> sent;
        auto rec = [&](const std::string &u, bool cookie) {
            sent.push_back({ u, cookie });
            Hop h;
            if (u == "https://site.example/v.mp4") { h.status = 302; h.location = "https://site.example/w.mp4"; }
            else if (u == "https://site.example/w.mp4") { h.status = 302; h.location = "https://other.example/w.mp4"; }
            else if (u == "https://other.example/w.mp4") { h.status = 302; h.location = "https://site.example/x.mp4"; }
            else h.status = 206;
            return h;
        };
        CHECK(vet_chain("https://site.example/v.mp4", "site.example", "site.example", rec, fake_vet) == "https://site.example/x.mp4", "cookies: la cadena llega");
        CHECK(sent.size() == 4 && sent[0].second && sent[1].second, "cookies: a su sitio, también tras una redirección en él");
        CHECK(sent.size() == 4 && !sent[2].second, "cookies: una redirección a otro sitio llega sin ellas");
        CHECK(sent.size() == 4 && !sent[3].second, "cookies: y no vuelven al regresar a su sitio");
        sent.clear();
        CHECK(!vet_chain("https://site.example/v.mp4", "site.example", "", rec, fake_vet).empty() && !sent[0].second, "cookies: sin cookies, ninguna");
        std::string same, other;
        CHECK(safe_list("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"/k.bin\"\n#EXTINF:4,\na.ts\n", "https://site.example/l.m3u8", "site.example", same) &&
              cookie_for_mpv("site.example", { same }), "cookies a mpv: una lista toda en su sitio");
        CHECK(safe_list("#EXTM3U\n#EXTINF:4,\na.ts\n#EXTINF:4,\nhttps://cdn.other.example/b.ts\n", "https://site.example/l.m3u8", "site.example", other) &&
              !cookie_for_mpv("site.example", { other }), "cookies a mpv: una lista con trozos de otro sitio, sin ellas");
        CHECK(safe_list("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"https://keys.other.example/k\"\n#EXTINF:4,\na.ts\n", "https://site.example/l.m3u8", "site.example", other) &&
              !cookie_for_mpv("site.example", { other }), "cookies a mpv: una clave en otro sitio, sin ellas");
        CHECK(!cookie_for_mpv("site.example", { same, "https://other.example/a.mp4" }) && cookie_for_mpv("site.example", { "https://site.example/a.mp4" }) &&
              !cookie_for_mpv("", { "https://site.example/a.mp4" }), "cookies a mpv: un fichero, solo en su sitio");
    }
    // the system's resolver: localtest.me is a public name for 127.0.0.1
    addrinfo *r = nullptr; WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    if (getaddrinfo("localtest.me", nullptr, nullptr, &r) == 0) { freeaddrinfo(r); CHECK(!vet_host("localtest.me", "example.com"), "localtest.me (127.0.0.1) desde internet"); }
    else printf("--    localtest.me no se resuelve aquí (sin probar con DNS)\n");
    CHECK(!vet_host("localhost", "example.com"), "localhost por DNS");
}

// ---------------------------------------------------------------- with mpv itself
typedef struct mpv_handle mpv_handle;
struct mpv_event { int event_id; int error; unsigned long long reply_userdata; void *data; };
static struct { mpv_handle *(*create)(); int (*initialize)(mpv_handle *); void (*terminate_destroy)(mpv_handle *); int (*set_option_string)(mpv_handle *, const char *, const char *);
                int (*command)(mpv_handle *, const char **); mpv_event *(*wait_event)(mpv_handle *, double); } M;
static std::string g_mp4, g_dir;
static int g_port;
static std::atomic<int> g_hits_ok{ 0 };
static void serve(SOCKET ls) {
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        std::thread([c] {
            char buf[4096]; int n = recv(c, buf, sizeof(buf) - 1, 0);
            if (n <= 0) { closesocket(c); return; }
            buf[n] = 0;
            std::string req(buf), path = req.substr(4, req.find(' ', 4) - 4);
            std::string canary = "file:///" + g_dir + "/canario.mp4", head, body;
            if (path == "/ok.mp4" || path == "/canario-por-red.mp4") {
                std::ifstream f(g_mp4, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); body = ss.str();
                if (path == "/ok.mp4") g_hits_ok++;
                head = "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n";
            } else if (path == "/mal.m3u8") { head = "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n"; body = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\n" + canary + "\n#EXT-X-ENDLIST\n"; }
            else if (path == "/mal.m3u") { head = "HTTP/1.1 200 OK\r\nContent-Type: audio/x-mpegurl\r\n"; body = "#EXTM3U\n" + g_dir + "\\canario.mp4\n"; }
            else if (path == "/redir-file.mp4") head = "HTTP/1.1 302 Found\r\nLocation: " + canary + "\r\n";
            else if (path == "/hls-redir.m3u8") { head = "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n";
                                                  body = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\nhttp://127.0.0.1:" + std::to_string(g_port) + "/redir.mp4\n#EXT-X-ENDLIST\n"; }
            else if (path == "/redir.mp4") head = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + std::to_string(g_port) + "/ok.mp4\r\n";
            else head = "HTTP/1.1 404 Not Found\r\n";
            head += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            std::string all = head + body;
            send(c, all.data(), (int)all.size(), 0);
            shutdown(c, SD_SEND); closesocket(c);
        }).detach();
    }
}
// true if mpv got as far as loading something from that address
static bool mpv_loads(const std::string &url) {
    mpv_handle *h = M.create();
    for (auto kv : { std::pair<const char *, const char *>{ "config", "no" }, { "load-scripts", "no" }, { "ytdl", "no" }, { "ao", "null" }, { "vo", "null" },
                     { "terminal", "no" }, { "idle", "yes" }, { "audio-file-auto", "no" }, { "sub-auto", "no" }, { "cover-art-auto", "no" } })
        M.set_option_string(h, kv.first, kv.second);
    if (getenv("SCROLLER_MPV_LOG")) { M.set_option_string(h, "log-file", getenv("SCROLLER_MPV_LOG")); M.set_option_string(h, "msg-level", "all=debug"); }
    M.initialize(h);
    std::string o = scroll::loadfile_options(false, 0);
    const char *cmd[] = { "loadfile", url.c_str(), "replace", "-1", o.c_str(), nullptr };
    M.command(h, cmd);
    bool loaded = false;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 6000) {
        mpv_event *e = M.wait_event(h, 0.5);
        if (!e) continue;
        if (e->event_id == 8) { loaded = true; break; }   // FILE_LOADED
        if (e->event_id == 7) break;                      // END_FILE (refused)
    }
    M.terminate_destroy(h);
    return loaded;
}
static void with_mpv(const char *dll, const char *mp4) {
    HMODULE m = LoadLibraryExA(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { printf("FALLA no se pudo cargar %s\n", dll); fails++; return; }
    *(FARPROC *)&M.create = GetProcAddress(m, "mpv_create"); *(FARPROC *)&M.initialize = GetProcAddress(m, "mpv_initialize");
    *(FARPROC *)&M.terminate_destroy = GetProcAddress(m, "mpv_terminate_destroy"); *(FARPROC *)&M.set_option_string = GetProcAddress(m, "mpv_set_option_string");
    *(FARPROC *)&M.command = GetProcAddress(m, "mpv_command"); *(FARPROC *)&M.wait_event = GetProcAddress(m, "mpv_wait_event");
    g_mp4 = mp4;
    if (getenv("SCROLLER_MPV_OPTS")) {
        auto gps = (char *(*)(mpv_handle *, const char *))GetProcAddress(m, "mpv_get_property_string");
        mpv_handle *h = M.create(); M.initialize(h);
        char *o = gps(h, "options");
        std::string all = o ? o : "";
        size_t p = 0;
        while (p < all.size()) { size_t e = all.find(',', p); std::string n = all.substr(p, e - p); if (n.find("curl") != std::string::npos || n.find("redirect") != std::string::npos || n.find("network") != std::string::npos) printf("opción: %s\n", n.c_str()); if (e == std::string::npos) break; p = e + 1; }
        M.terminate_destroy(h);
    }
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    g_dir = std::string(tmp) + "pluma-scroller-seguridad";
    CreateDirectoryA(g_dir.c_str(), NULL);
    CopyFileA(mp4, (g_dir + "\\canario.mp4").c_str(), FALSE);
    for (auto &c : g_dir) if (c == '\\') c = '/';
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (sockaddr *)&a, sizeof(a)); listen(ls, 16);
    int len = sizeof(a); getsockname(ls, (sockaddr *)&a, &len); g_port = ntohs(a.sin_port);
    std::thread(serve, ls).detach();
    std::string base = "http://127.0.0.1:" + std::to_string(g_port);
    CHECK(mpv_loads(base + "/ok.mp4"), "mpv abre un vídeo normal (control)");
    CHECK(!mpv_loads(base + "/mal.m3u8"), "mpv: lista HLS con un fichero local");
    CHECK(!mpv_loads(base + "/mal.m3u"), "mpv: lista simple con una ruta local");
    CHECK(!mpv_loads(base + "/redir-file.mp4"), "mpv: redirección a file:");
    int before = g_hits_ok;
    CHECK(!mpv_loads(base + "/redir.mp4") && g_hits_ok == before, "mpv no sigue redirecciones (curl-max-redirects=0)");
    mpv_loads(base + "/hls-redir.m3u8");
    CHECK(g_hits_ok == before, "mpv no sigue la redirección de un trozo de una lista");
    closesocket(ls);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    gate();
    if (argc >= 3) with_mpv(argv[1], argv[2]);
    printf(fails ? "\n%d FALLAS\n" : "\ntodo bien\n", fails);
    return fails ? 1 : 0;
}
