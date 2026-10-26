// Pluma: the Scroller's gate between a web page and mpv (scroller.cpp, video.cpp; tested by tools\scroller-seguridad.cpp).
// Every address the Scroller hands to mpv comes from a page, and pages are hostile: only plain http(s) addresses get
// through, never a local file, one of mpv's own protocols (edl://, lavfi://, av://, memory://...) or, from a page on
// the internet, a machine of the local network. A list the page reads for Pluma (an HLS master signed for it) is
// made whole and checked line by line before it goes to disk; and mpv opens what the Scroller gives it with its own
// demuxer only (lavf) and a protocol list without "file", so a list on the internet cannot point it at the disk either.
// A name can point anywhere, and a server can redirect anywhere: so every host is also resolved (vet_host: any of
// its addresses on the machine or the local network refuses it), and every redirect is followed by Pluma itself, hop by
// hop, each hop checked again (vet_chain). mpv gets the last address and follows no redirect of its own
// (max_redirects=0).
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace scroll {

inline std::string lower_ascii(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

// "https://Host:8080/x" -> "host" (empty if it is not an http(s) address)
inline std::string host_of(const std::string &u) {
    size_t p = u.rfind("https://", 0) == 0 ? 8 : u.rfind("http://", 0) == 0 ? 7 : std::string::npos;
    if (p == std::string::npos) return "";
    size_t e = u.find_first_of("/?#", p);
    std::string h = u.substr(p, e == std::string::npos ? std::string::npos : e - p);
    size_t at = h.rfind('@');
    if (at != std::string::npos) h = h.substr(at + 1);
    if (!h.empty() && h[0] == '[') { size_t b = h.find(']'); return lower_ascii(h.substr(0, b == std::string::npos ? std::string::npos : b + 1)); }
    size_t colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    return lower_ascii(h);
}

// the machine itself or its local network: loopback, private and link-local addresses, names that only mean
// something at home (.local, .lan, .home.arpa, a name without a dot)
inline bool private_host(const std::string &h) {
    if (h.empty()) return true;
    if (h[0] == '[') return true;   // (IPv6 literals: ::1, fc00::/7, fe80::/10... none is a video server)
    if (h == "localhost" || h.find('.') == std::string::npos) return true;
    auto ends = [&](const char *s) { std::string x(s); return h.size() >= x.size() && h.compare(h.size() - x.size(), x.size(), x) == 0; };
    if (ends(".localhost") || ends(".local") || ends(".lan") || ends(".home.arpa") || ends(".internal") || ends(".intranet")) return true;
    // a dotted IPv4 (decimal only: other spellings are refused below as unusual)
    int part[4], n = 0; size_t i = 0;
    while (n < 4 && i < h.size() && isdigit((unsigned char)h[i])) {
        int v = 0, digits = 0;
        while (i < h.size() && isdigit((unsigned char)h[i]) && digits < 4) { v = v * 10 + (h[i] - '0'); i++; digits++; }
        part[n++] = v;
        if (i < h.size() && h[i] == '.') i++; else break;
    }
    if (n == 4 && i == h.size()) {
        if (part[0] == 127 || part[0] == 10 || part[0] == 0) return true;
        if (part[0] == 169 && part[1] == 254) return true;
        if (part[0] == 172 && part[1] >= 16 && part[1] <= 31) return true;
        if (part[0] == 192 && part[1] == 168) return true;
        if (part[0] == 100 && part[1] >= 64 && part[1] <= 127) return true;   // (carrier-grade NAT, Tailscale)
        return false;
    }
    // all digits and dots but not a plain IPv4 (0x7f.1, 2130706433): refused
    if (h.find_first_not_of("0123456789.") == std::string::npos) return true;
    if (h.rfind("0x", 0) == 0) return true;
    return false;
}

// an address of the machine or of a local network: loopback, private (RFC 1918), link-local, carrier-grade NAT,
// unspecified, multicast, broadcast; IPv6 loopback, unique-local (fc00::/7), link-local (fe80::/10), and IPv4 inside IPv6
inline bool private_v4(const uint8_t a[4]) {
    return a[0] == 0 || a[0] == 10 || a[0] == 127 || (a[0] == 169 && a[1] == 254) || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) ||
           (a[0] == 192 && a[1] == 168) || (a[0] == 100 && a[1] >= 64 && a[1] <= 127) || a[0] >= 224 || (a[0] == 192 && a[1] == 0 && a[2] == 0);
}
inline bool private_v6(const uint8_t a[16]) {
    bool zero10 = true;
    for (int i = 0; i < 10; i++) if (a[i]) zero10 = false;
    if (zero10 && ((a[10] == 0xff && a[11] == 0xff) || (a[10] == 0 && a[11] == 0))) {   // ::ffff:a.b.c.d, ::a.b.c.d, ::, ::1
        bool all0 = true; for (int i = 10; i < 16; i++) if (a[i]) all0 = false;
        if (all0 || (a[10] == 0 && a[11] == 0 && a[12] == 0 && a[13] == 0 && a[14] == 0 && a[15] == 1)) return true;
        return private_v4(a + 12);
    }
    if ((a[0] & 0xfe) == 0xfc) return true;                  // fc00::/7
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return true;  // fe80::/10
    if (a[0] == 0xff) return true;                           // multicast
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xff && a[3] == 0x9b) return private_v4(a + 12);   // 64:ff9b::/96 (NAT64)
    return false;
}

// an address mpv may open for a page on page_host: http(s) only, nothing odd in it, and the local network only for a
// page that is itself there
// the page's own host is on the machine or a local network (given bare or with its port, as the address bar has it)
inline bool page_local(const std::string &page_host) {
    std::string h = lower_ascii(page_host);
    if (h.find("://") == std::string::npos) h = "http://" + h;
    return private_host(host_of(h));
}

inline bool url_ok(const std::string &u, const std::string &page_host) {
    if (u.size() < 10 || u.size() > 8192) return false;
    if (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0) return false;
    for (unsigned char c : u) if (c <= 0x20 || c == 0x7f || c == '\\' || c == '"' || c == '<' || c == '>' || c == '`') return false;
    std::string h = host_of(u);
    if (h.empty() || h.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.[]:") != std::string::npos) return false;
    if (private_host(h) && !page_local(page_host)) return false;
    return true;
}

#ifdef _WIN32
// the host's addresses (the system's resolver, as mpv's would): false if it has none, or any of them is the machine's
// or a local network's (unless the page itself is on one: a page on the local network may play its own videos)
inline bool vet_host(const std::string &host, const std::string &page_host) {
    if (page_local(page_host)) return true;
    std::string h = host;
    if (!h.empty() && h[0] == '[') h = h.substr(1, h.size() - 2);
    static bool started = false;
    if (!started) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); started = true; }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    bool ok = true;
    for (addrinfo *p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) { if (private_v4((const uint8_t *)&((sockaddr_in *)p->ai_addr)->sin_addr)) ok = false; }
        else if (p->ai_family == AF_INET6) { if (private_v6((const uint8_t *)&((sockaddr_in6 *)p->ai_addr)->sin6_addr)) ok = false; }
        else ok = false;
    }
    freeaddrinfo(res);
    return ok;
}
#endif

inline std::string resolve(const std::string &base, const std::string &u);
// one request, without following redirects: its status, and where a redirect goes (fetch gives them)
struct Hop { int status = 0; std::string location; };
// an address followed to where it ends, every hop checked (the address, and its host's addresses): the last one, or
// "" if a hop is refused, fails or there are more than 5. fetch(url, cookie) does one request without redirects, with
// the page's cookies only if cookie: while the hops stay on cookie_host (the host the cookies are for: the page's own),
// and never again once one left it. vet(host) is vet_host (a stand-in in the tests)
inline std::string vet_chain(std::string url, const std::string &page_host, const std::string &cookie_host,
                             const std::function<Hop(const std::string &, bool)> &fetch, const std::function<bool(const std::string &)> &vet) {
    bool cookie = !cookie_host.empty();
    for (int hop = 0; hop < 6; hop++) {
        if (!url_ok(url, page_host) || !vet(host_of(url))) return "";
        cookie = cookie && host_of(url) == lower_ascii(cookie_host);
        Hop h = fetch(url, cookie);
        if (h.status >= 300 && h.status < 400 && h.status != 304) {
            if (h.location.empty() || hop == 5) return "";
            url = resolve(url, h.location);
            continue;
        }
        return h.status >= 200 && h.status < 300 ? url : "";
    }
    return "";
}

// a header value from a page or the cookie store: one line, printable, short
inline bool header_ok(const std::string &v) {
    if (v.size() > 8192) return false;
    for (unsigned char c : v) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

// an address as a list writes it (relative to the list), made whole
inline std::string resolve(const std::string &base, const std::string &u) {
    // (a list's or a redirect's)
    if (u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0) return u;
    if (u.find(':') != std::string::npos && u.find(':') < u.find_first_of("/?#")) return u;   // (another scheme: left as it is, and refused)
    if (u.rfind("//", 0) == 0) { size_t s = base.find("://"); return s == std::string::npos ? u : base.substr(0, s + 1) + u; }
    size_t host_end = base.find('/', base.find("://") == std::string::npos ? 0 : base.find("://") + 3);
    if (host_end == std::string::npos) return u;
    if (!u.empty() && u[0] == '/') return base.substr(0, host_end) + u;
    std::string dir = base.substr(0, base.find_first_of("?#"));
    return dir.substr(0, dir.rfind('/') + 1) + u;
}

// an HLS list (a master or a media list) the page gave: every address in it (its lines and the URI="..." of its tags)
// made whole from base and checked; out is the list as mpv will read it from the disk. False: something in it is not
// an address mpv may open (the whole list is refused, not just that line)
inline bool safe_list(const std::string &text, const std::string &base, const std::string &page_host, std::string &out) {
    out.clear();
    if (text.rfind("#EXTM3U", 0) != 0 || text.size() > 512 * 1024) return false;
    size_t pos = 0;
    int addresses = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (unsigned char c : line) if (c < 0x20 && c != '\t') return false;
        if (line.empty()) { out += "\n"; continue; }
        if (line[0] != '#') {
            line = resolve(base, line);
            if (!url_ok(line, page_host)) return false;
            addresses++;
        } else {
            size_t from = 0;
            while ((from = line.find("URI=\"", from)) != std::string::npos) {
                size_t e = line.find('"', from + 5);
                if (e == std::string::npos) return false;
                std::string u = resolve(base, line.substr(from + 5, e - from - 5));
                if (!url_ok(u, page_host)) return false;
                line = line.substr(0, from + 5) + u + line.substr(e);
                from = from + 5 + u.size() + 1;
                addresses++;
            }
        }
        out += line + "\n";
    }
    return addresses > 0;
}

// every host a list names (its lines and the URI="..." of its tags, already whole: safe_list's output)
inline std::vector<std::string> list_hosts(const std::string &list) {
    std::vector<std::string> hosts;
    size_t p = 0;
    while ((p = list.find("http", p)) != std::string::npos) {
        if (list.compare(p, 7, "http://") && list.compare(p, 8, "https://")) { p += 4; continue; }
        size_t e = list.find_first_of("\r\n\"", p);
        std::string h = host_of(list.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (std::find(hosts.begin(), hosts.end(), h) == hosts.end()) hosts.push_back(h);
        p = e == std::string::npos ? list.size() : e;
    }
    return hosts;
}
// mpv sends its headers with every request of a file (a list's pieces and keys too): the page's cookies go to it only
// if everything it will ask for is on their host. what: the address it opens, or the lists it reads from the disk
inline bool cookie_for_mpv(const std::string &cookie_host, const std::vector<std::string> &what) {
    if (cookie_host.empty() || what.empty()) return false;
    for (auto &w : what) {
        if (w.rfind("http://", 0) == 0 || w.rfind("https://", 0) == 0) { if (host_of(w) != lower_ascii(cookie_host)) return false; continue; }
        auto hosts = list_hosts(w);
        if (hosts.empty()) return false;
        for (auto &h : hosts) if (h != lower_ascii(cookie_host)) return false;
    }
    return true;
}

// what goes with every file the Scroller loads (mpv's per-file options): its own demuxer only (no mpv playlists,
// EDL, CUE or archives that could name local files) and, for what lavf opens from inside a stream (HLS variants and
// pieces, DASH), only network protocols. Nothing of the page is in here: its addresses and headers go apart.
inline std::string loadfile_options(bool paused, int loops) {
    // (and no redirect: Pluma followed them already, each checked; one now could go to the local network)
    // (mpv reads http with its own libcurl, the pieces of a list too: curl-max-redirects; and FFmpeg's, if not)
    const std::string lavf = "seg_max_retry=3,protocol_whitelist=[http,https,tls,tcp,crypto,httpproxy]";
    const std::string stream = "max_redirects=0";
    std::string o = std::string("start=0,pause=") + (paused ? "yes" : "no") + ",demuxer=lavf,demuxer-lavf-o=%" + std::to_string(lavf.size()) + "%" + lavf +
                    ",curl-max-redirects=0,stream-lavf-o=%" + std::to_string(stream.size()) + "%" + stream;
    o += loops < 0 ? ",loop-file=inf" : loops > 0 ? ",loop-file=" + std::to_string(loops) : ",loop-file=no";
    return o;
}

}   // namespace scroll
