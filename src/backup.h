// Pluma: the backup file (sync.cpp writes and reads it; tools\sync-test.cpp checks it). Standalone: Windows, the C++
// library, json.h and Monocypher, nothing of Pluma, so the test builds it alone.
//
//   "PLUMABK1"                     8 bytes
//   header length                  uint32, little endian
//   header                         JSON, UTF-8, in clear: format, the key derivation and its salt, when, which
//                                  Windows, the password's hint. Nothing else (no sites, no counts).
//   nonce                          12 bytes, fresh for every file
//   ciphertext + tag               AES-256-GCM (BCrypt) of the compressed archive; the tag (16 bytes) last
// The magic, the length and the header are the GCM's additional data: changing any of them fails like a wrong password.
// Key: Argon2id (Monocypher) of the password with the header's salt; memory, passes and lanes are in the header too.
// Archive: "PLAR", uint32 count, then per entry uint32 name length, name (UTF-8, '/' separated), uint64 size, the
// bytes. Names are checked before anything is written: only Pluma's own files and the engine's profiles (not their
// caches), never "..", drive letters or absolute paths (allowed_target).
// Compression (Windows's XPRESS+Huffman): formato 1, the whole archive in one piece; formato 2 (since the whole
// browser goes in), blocks of up to 4 MB of the archive as it is filled, each "uint32 raw length, uint32 compressed
// length, bytes", and a raw length of 0 at the end; its count is 0xFFFFFFFF (entries up to the end). So a backup
// holds in memory the compressed copy and one block, not the profile two or three times over.
#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10: the compression API and the BCrypt calls below
#endif
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include "json.h"
#include "monocypher/monocypher.h"

namespace bk {

static const char kMagic[8] = { 'P', 'L', 'U', 'M', 'A', 'B', 'K', '1' };
static const size_t kMaxArchive = (size_t)2 << 30;   // 2 GB: a bigger one is not ours

struct Kdf {
    uint32_t m_kib = 65536, t = 3, p = 4;   // 64 MB, 3 passes, 4 lanes (Bitwarden's defaults for Argon2id)
    std::string salt;                       // 16 bytes
};

inline bool random_bytes(void *p, size_t n) {
    return BCryptGenRandom(NULL, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
inline std::string hex(const std::string &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
inline bool unhex(const std::string &s, std::string &out) {
    out.clear();
    if (s.size() % 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out += (char)((a << 4) | b);
    }
    return true;
}
inline void wipe(std::string &s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size()); s.clear(); }

// ---------------------------------------------------------------- the key from the password
inline bool kdf_sane(const Kdf &k) {
    // a header could ask for anything: keep it to what Pluma itself would write (up to 1 GB, 10 passes, 16 lanes)
    return k.salt.size() >= 16 && k.salt.size() <= 64 && k.p >= 1 && k.p <= 16 && k.t >= 1 && k.t <= 10 &&
           k.m_kib >= 8 * k.p && k.m_kib <= (1u << 20);
}
inline bool derive(const std::string &password, const Kdf &k, uint8_t key[32]) {
    if (!kdf_sane(k)) return false;
    void *work = VirtualAlloc(NULL, (SIZE_T)k.m_kib * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!work) return false;
    crypto_argon2_config cfg = { CRYPTO_ARGON2_ID, k.m_kib, k.t, k.p };
    crypto_argon2_inputs in = { (const uint8_t *)password.data(), (const uint8_t *)k.salt.data(), (uint32_t)password.size(), (uint32_t)k.salt.size() };
    crypto_argon2(key, 32, work, cfg, in, crypto_argon2_no_extras);
    SecureZeroMemory(work, (SIZE_T)k.m_kib * 1024);
    VirtualFree(work, 0, MEM_RELEASE);
    return true;
}

// ---------------------------------------------------------------- AES-256-GCM (one shot)
inline bool gcm(bool encrypt, const uint8_t key[32], const uint8_t nonce[12], const std::string &aad,
                const uint8_t *in, size_t n, uint8_t *out, uint8_t tag[16]) {
    if (n > 0xFFFFFFF0u) return false;
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_KEY_HANDLE k = NULL;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) return false;
    do {
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) break;
        if (BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key, 32, 0) != 0) break;
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)nonce; info.cbNonce = 12; info.pbTag = tag; info.cbTag = 16;
        info.pbAuthData = (PUCHAR)aad.data(); info.cbAuthData = (ULONG)aad.size();
        ULONG done = 0;
        NTSTATUS st = encrypt ? BCryptEncrypt(k, (PUCHAR)in, (ULONG)n, &info, NULL, 0, out, (ULONG)n, &done, 0)
                              : BCryptDecrypt(k, (PUCHAR)in, (ULONG)n, &info, NULL, 0, out, (ULONG)n, &done, 0);
        ok = st == 0 && done == n;
    } while (0);
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---------------------------------------------------------------- compression (Windows's own, cabinet.dll)
inline bool compress(const std::string &in, std::string &out) {
    COMPRESSOR_HANDLE c = NULL;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &c)) return false;
    SIZE_T need = 0;
    Compress(c, (PVOID)in.data(), in.size(), NULL, 0, &need);
    bool ok = false;
    if (need) {
        out.resize(need);
        SIZE_T got = 0;
        ok = Compress(c, (PVOID)in.data(), in.size(), &out[0], out.size(), &got) != FALSE;
        out.resize(ok ? got : 0);
    }
    CloseCompressor(c);
    return ok;
}
inline bool decompress(const std::string &in, std::string &out) {
    DECOMPRESSOR_HANDLE d = NULL;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &d)) return false;
    SIZE_T need = 0;
    Decompress(d, (PVOID)in.data(), in.size(), NULL, 0, &need);
    bool ok = false;
    if (need && need <= kMaxArchive) {
        out.resize(need);
        SIZE_T got = 0;
        ok = Decompress(d, (PVOID)in.data(), in.size(), &out[0], out.size(), &got) != FALSE && got == need;
        if (!ok) out.clear();
    }
    CloseDecompressor(d);
    return ok;
}

// ---------------------------------------------------------------- the archive
struct Entry { std::string name, data; };

// what a restore may write: Pluma's own files and folders, and the engine's profiles (all relative to the data folder)
inline const std::vector<std::string> &pluma_files() {
    static const std::vector<std::string> v = { "ajustes.ini", "sesion.tsv", "historial.tsv", "marcadores.json", "paginas.tsv",
                                                "ocultas.tsv", "lectura.tsv", "extensiones.tsv", "perfiles.tsv", "traducir-nunca.tsv",
                                                "passwords-pending.tsv", "perfil-ultimo.txt", "avatar.png" };
    return v;
}
inline const std::vector<std::string> &pluma_dirs() {
    static const std::vector<std::string> v = { "scripts", "extensiones", "iconos", "perfiles", "extensiones-ajustes" };
    return v;
}
// inside Pluma's folders: what is made again by itself (a profile's video cache, an import's scratch folder)
inline bool pluma_skip_dir(const std::string &seg) { return seg == "cache-video" || seg == "imp_tmp"; }

// The engine (webview2\EBWebView\<profile>\...): the whole profile, as Firefox keeps its whole profile folder and
// leaves its caches apart: passwords, cookies, every site's storage (Local Storage, Session Storage, IndexedDB,
// Service Worker with its scripts and Cache Storage, File System, WebStorage), Web Data (autofill, cards, addresses),
// History, Favicons, Top Sites, Preferences and Secure Preferences (site permissions, zoom), the extensions and all
// their state. Left out, at any depth, what the engine makes again by itself (bleachbit's "cache" list for Chrome,
// Chromium's own cache folders): HTTP and code caches, GPU shader caches, crash dumps, leveldb's lock and text logs,
// SQLite's shared-memory index (rebuilt from the -wal), temp files. Passwords and cookies are encrypted with the
// engine's key, which the backup carries apart (meta/clave) and a restore guards again for this Windows account.
inline bool engine_skip_dir(const std::string &seg) {
    static const char *const v[] = { "Cache", "Code Cache", "GPUCache", "DawnCache", "DawnGraphiteCache", "DawnWebGPUCache",
                                     "GraphiteDawnCache", "GrShaderCache", "ShaderCache", "GPUPersistentCache", "Media Cache",
                                     "Application Cache", "blob_storage", "Crashpad", "component_crx_cache",
                                     "extensions_crx_cache", "Safe Browsing Network", "BrowserMetrics", "Shared Dictionary",
                                     "optimization_guide_hint_cache_store", "optimization_guide_model_store" };
    for (auto *d : v) if (seg == d) return true;
    return seg.compare(0, 12, "JumpListIcon") == 0;
}
inline bool engine_skip_file(const std::string &leaf) {
    if (leaf == "LOCK" || leaf == "LOG" || leaf == "LOG.old") return true;   // leveldb's lock (held by the engine) and text logs
    auto ends = [&](const char *x) { size_t n = strlen(x); if (leaf.size() < n) return false;
                                     for (size_t i = 0; i < n; i++) if (tolower((unsigned char)leaf[leaf.size() - n + i]) != x[i]) return false;
                                     return true; };
    return ends("-shm") || ends(".tmp") || ends(".dbtmp");
}
// a path inside an engine profile ('/' separated) that a backup keeps (every folder on the way and the file itself)
inline bool engine_kept(const std::string &rel) {
    if (rel.empty()) return false;
    size_t a = 0;
    for (;;) {
        size_t b = rel.find('/', a);
        if (b == std::string::npos) return !engine_skip_file(rel.substr(a));
        if (engine_skip_dir(rel.substr(a, b - a))) return false;
        a = b + 1;
    }
}
// what a backup made before the whole profile went in (26/10) holds of each engine profile: a restore of one of those
// puts back just these, leaving the rest of the profile as it is
inline const std::vector<std::string> &engine_paths_v1() {
    static const std::vector<std::string> v = {
        "Local Extension Settings", "Sync Extension Settings", "Managed Extension Settings",
        "Extension State", "Extension Rules", "Extension Scripts",
        "Login Data", "Login Data-wal", "Login Data For Account", "Login Data For Account-wal",
        "Network/Cookies", "Network/Cookies-wal" };
    return v;
}
// the engine databases it keeps locked for itself while it runs (nobody else can even read them): each start copies
// them (with their journals) before the engine opens them, and the backup takes that copy
inline const std::vector<std::string> &engine_locked_dbs() {
    static const std::vector<std::string> v = { "Network/Cookies", "Login Data", "Login Data For Account" };
    return v;
}
// an engine profile folder: "Default", or "WV2Profile_pluma-<id>" (a Pluma profile: lowercase letters and digits)
inline bool engine_profile(const std::string &s) {
    if (s == "Default") return true;
    static const std::string pre = "WV2Profile_pluma-";
    if (s.size() <= pre.size() || s.size() > pre.size() + 16 || s.compare(0, pre.size(), pre) != 0) return false;
    for (size_t i = pre.size(); i < s.size(); i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return false;
    return true;
}
inline bool safe_name(const std::string &n) {
    if (n.empty() || n.size() > 400 || n[0] == '/' || n.back() == '/') return false;
    size_t a = 0;
    while (a <= n.size()) {
        size_t b = n.find('/', a);
        std::string seg = n.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ') return false;
        for (unsigned char c : seg) if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        // Windows's device names (con, nul, com1...) are not files, whatever comes after the dot
        std::string base = seg.substr(0, seg.find('.'));
        for (auto &ch : base) ch = (char)tolower((unsigned char)ch);
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
            (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '0' && base[3] <= '9'))
            return false;
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return true;
}
inline bool under(const std::string &n, const std::string &root) {   // n is root itself or inside it
    return n == root || (n.size() > root.size() && n.compare(0, root.size(), root) == 0 && n[root.size()] == '/');
}
// "pluma/<file or dir/...>", "motor/<engine profile>/<path kept>", or one of the "meta/..." entries (never written as
// files). meta/clave is the engine's key (it opens the passwords and cookies): sync.cpp stores it and puts it back.
inline bool allowed_target(const std::string &n) {
    if (!safe_name(n)) return false;
    if (n == "meta/info.json" || n == "meta/clave" || n == "meta/cuenta.json") return true;
    if (n.compare(0, 6, "pluma/") == 0) {
        std::string r = n.substr(6);
        for (auto &f : pluma_files()) if (r == f) return true;
        for (auto &d : pluma_dirs()) if (r.size() > d.size() && under(r, d)) {
            for (size_t a = 0, b; (b = r.find('/', a)) != std::string::npos; a = b + 1)
                if (pluma_skip_dir(r.substr(a, b - a))) return false;
            return true;
        }
        return false;
    }
    if (n.compare(0, 6, "motor/") == 0) {
        size_t cut = n.find('/', 6);
        if (cut == std::string::npos || !engine_profile(n.substr(6, cut - 6))) return false;
        return engine_kept(n.substr(cut + 1));
    }
    return false;
}

inline void put32(std::string &o, uint32_t v) { for (int i = 0; i < 4; i++) o += (char)((v >> (8 * i)) & 255); }
inline void put64(std::string &o, uint64_t v) { for (int i = 0; i < 8; i++) o += (char)((v >> (8 * i)) & 255); }
inline uint64_t get(const std::string &s, size_t at, int bytes) {
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | (unsigned char)s[at + i];
    return v;
}
// builds the archive one entry at a time (the whole profile does not have to sit in memory twice)
struct Packer {
    std::string out;
    uint32_t count = 0;
    Packer() { out.append("PLAR", 4); put32(out, 0); }
    void add(const std::string &name, const char *data, size_t n) {
        put32(out, (uint32_t)name.size()); out += name; put64(out, n); out.append(data, n); count++;
    }
    void add(const std::string &name, const std::string &data) { add(name, data.data(), data.size()); }
    std::string &finish() { for (int i = 0; i < 4; i++) out[4 + i] = (char)((count >> (8 * i)) & 255); return out; }
};
inline std::string pack(const std::vector<Entry> &es) {
    Packer p;
    size_t total = 8;
    for (auto &e : es) total += 12 + e.name.size() + e.data.size();
    p.out.reserve(total);
    for (auto &e : es) p.add(e.name, e.data);
    return std::move(p.finish());
}
// every bound is checked and every name must be allowed: one bad entry and the whole archive is refused
inline bool unpack_raw(const std::string &a, std::vector<Entry> &out) {
    out.clear();
    if (a.size() < 8 || a.compare(0, 4, "PLAR") != 0) return false;
    uint64_t n = get(a, 4, 4);
    size_t at = 8;
    bool to_end = n == 0xFFFFFFFFu;   // formato 2: entries up to the end
    if (to_end) n = 1000000;
    else if (n > 1000000) return false;
    for (uint64_t i = 0; i < n && !(to_end && at == a.size()); i++) {
        if (a.size() - at < 4) return false;
        uint64_t nl = get(a, at, 4); at += 4;
        if (nl == 0 || nl > 400 || a.size() - at < nl + 8) return false;
        Entry e;
        e.name = a.substr(at, (size_t)nl); at += (size_t)nl;
        uint64_t dl = get(a, at, 8); at += 8;
        if (dl > a.size() - at) return false;
        if (!allowed_target(e.name)) return false;
        e.data = a.substr(at, (size_t)dl); at += (size_t)dl;
        out.push_back(std::move(e));
    }
    return at == a.size();
}
inline bool unpack(const std::string &a, std::vector<Entry> &out) {
    if (unpack_raw(a, out)) return true;
    for (auto &e : out) wipe(e.data);   // nothing half-read survives a refused archive
    out.clear();
    return false;
}

// ---------------------------------------------------------------- the file
inline std::string header_json(const Kdf &k, const std::string &extra_fields /* ,"a":... already JSON, UTF-8 */, int format = 1) {
    return "{\"formato\":" + std::to_string(format) + ",\"kdf\":\"argon2id\",\"m\":" + std::to_string(k.m_kib) + ",\"t\":" + std::to_string(k.t) +
           ",\"p\":" + std::to_string(k.p) + ",\"sal\":\"" + hex(k.salt) + "\"" + extra_fields + "}";
}
// packed is wiped as soon as it is compressed
inline bool seal_packed(const uint8_t key[32], const std::string &header, std::string &packed, std::string &file) {
    std::string z;
    bool ok = packed.size() <= kMaxArchive && compress(packed, z);
    wipe(packed);
    packed.shrink_to_fit();
    if (!ok) return false;
    file.assign(kMagic, 8);
    put32(file, (uint32_t)header.size());
    file += header;
    std::string aad = file;
    uint8_t nonce[12], tag[16];
    if (!random_bytes(nonce, 12)) { wipe(z); return false; }
    size_t body = file.size() + 12;
    file.append((const char *)nonce, 12);
    file.resize(body + z.size());
    ok = gcm(true, key, nonce, aad, (const uint8_t *)z.data(), z.size(), (uint8_t *)&file[body], tag);
    wipe(z);
    if (!ok) { file.clear(); return false; }
    file.append((const char *)tag, 16);
    return true;
}
inline bool seal(const uint8_t key[32], const std::string &header, const std::vector<Entry> &entries, std::string &file) {
    std::string packed = pack(entries);
    return seal_packed(key, header, packed, file);
}
// formato 2: the archive compressed block by block as it is filled, straight into the file, then encrypted in place
static const size_t kBlock = (size_t)4 << 20;
struct Sealer {
    std::string file, raw, z;
    size_t body = 0;
    uint64_t raw_total = 0;
    bool ok = true;
    COMPRESSOR_HANDLE c = NULL;
    uint8_t nonce[12];
    Sealer(const std::string &header) {
        file.assign(kMagic, 8);
        put32(file, (uint32_t)header.size());
        file += header;
        ok = random_bytes(nonce, 12) && CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &c);
        file.append((const char *)nonce, 12);
        body = file.size();
        raw.reserve(kBlock + (1 << 16));
        raw.append("PLAR", 4); put32(raw, 0xFFFFFFFFu);
    }
    ~Sealer() { if (c) CloseCompressor(c); wipe(raw); wipe(z); }
    void reserve(size_t expected) { file.reserve(body + expected + 16); }   // growing it would hold two copies a moment
    void block(const char *p, size_t n) {
        if (!ok) return;
        SIZE_T need = 0;
        Compress(c, (PVOID)p, n, NULL, 0, &need);
        if (!need) { ok = false; return; }
        if (z.size() < need) z.resize(need);
        SIZE_T got = 0;
        if (!Compress(c, (PVOID)p, n, &z[0], z.size(), &got)) { ok = false; return; }
        put32(file, (uint32_t)n); put32(file, (uint32_t)got);
        file.append(z.data(), got);
        raw_total += n;
        if (raw_total > kMaxArchive) ok = false;
    }
    void flush(bool all) {
        size_t off = 0;
        while (ok && (raw.size() - off >= kBlock || (all && off < raw.size()))) {
            size_t n = (std::min)(kBlock, raw.size() - off);
            block(raw.data() + off, n);
            off += n;
        }
        if (off) { SecureZeroMemory(&raw[0], off); raw.erase(0, off); }
    }
    void add(const std::string &name, const char *data, size_t n) {
        put32(raw, (uint32_t)name.size()); raw += name; put64(raw, n);
        while (n) {   // a big file goes in by pieces: never a second whole copy of it
            size_t k = (std::min)(n, kBlock);
            raw.append(data, k); data += k; n -= k;
            flush(false);
        }
        flush(false);
    }
    void add(const std::string &name, const std::string &data) { add(name, data.data(), data.size()); }
    // the finished file in out (this one is left empty); false if anything failed
    bool finish(const uint8_t key[32], std::string &out) {
        flush(true);
        put32(file, 0);
        if (!ok) { wipe(file); return false; }
        uint8_t tag[16];
        std::string aad = file.substr(0, body - 12);
        if (!gcm(true, key, nonce, aad, (const uint8_t *)file.data() + body, file.size() - body, (uint8_t *)&file[body], tag)) { wipe(file); return false; }
        file.append((const char *)tag, 16);
        out.swap(file);
        return true;
    }
};
// formato 2's blocks back into the archive: every length is checked, the end mark is required and nothing follows it
inline bool unblock(const std::string &in, std::string &out) {
    out.clear();
    DECOMPRESSOR_HANDLE d = NULL;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &d)) return false;
    bool ok = false;
    size_t at = 0;
    for (;;) {
        if (in.size() - at < 4) break;
        uint64_t rl = get(in, at, 4); at += 4;
        if (rl == 0) { ok = at == in.size(); break; }
        if (rl > kBlock || in.size() - at < 4) break;
        uint64_t zl = get(in, at, 4); at += 4;
        if (zl == 0 || zl > in.size() - at || out.size() + rl > kMaxArchive) break;
        size_t was = out.size();
        out.resize(was + (size_t)rl);
        SIZE_T got = 0;
        if (!Decompress(d, (PVOID)(in.data() + at), (SIZE_T)zl, &out[was], (SIZE_T)rl, &got) || got != rl) break;
        at += (size_t)zl;
    }
    CloseDecompressor(d);
    if (!ok) wipe(out);
    return ok;
}
// the header alone (no password needed): what it is, its key derivation, its hint
struct Header { std::string text; Json j; Kdf kdf; size_t body = 0; int format = 1; };
inline bool read_header(const std::string &file, Header &h) {
    if (file.size() < 12 || memcmp(file.data(), kMagic, 8) != 0) return false;
    uint64_t hl = get(file, 8, 4);
    if (hl == 0 || hl > 64 * 1024 || file.size() < 12 + hl + 12 + 16) return false;
    h.text = file.substr(12, (size_t)hl);
    h.body = 12 + (size_t)hl;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, h.text.data(), (int)h.text.size(), NULL, 0);
    if (n <= 0) return false;
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, h.text.data(), (int)h.text.size(), &w[0], n);
    h.j = json_parse(w);
    if (h.j.kind != Json::OBJ || (h.j[L"formato"].n != 1 && h.j[L"formato"].n != 2) || h.j[L"kdf"].s != L"argon2id") return false;
    h.format = (int)h.j[L"formato"].n;
    auto num = [&](const wchar_t *k) { const Json &v = h.j[k]; return v.kind == Json::NUM && v.n >= 0 && v.n < 4294967296.0 ? (uint32_t)v.n : 0u; };
    h.kdf.m_kib = num(L"m"); h.kdf.t = num(L"t"); h.kdf.p = num(L"p");
    std::string salt_hex;
    for (wchar_t c : h.j[L"sal"].s) salt_hex += (char)(c < 128 ? c : '?');
    return unhex(salt_hex, h.kdf.salt) && kdf_sane(h.kdf);
}
enum OpenResult { OPEN_OK, OPEN_NOT_A_BACKUP, OPEN_WRONG_PASSWORD, OPEN_DAMAGED };
// a wrong password and a changed byte look the same to GCM: both say "wrong password" (or damaged), and nothing is out
inline OpenResult open(const std::string &file, const Header &h, const uint8_t key[32], std::vector<Entry> &out) {
    out.clear();
    if (file.size() < h.body + 12 + 16) return OPEN_NOT_A_BACKUP;
    std::string aad = file.substr(0, h.body);
    const uint8_t *nonce = (const uint8_t *)file.data() + h.body;
    size_t n = file.size() - h.body - 12 - 16;
    uint8_t tag[16];
    memcpy(tag, file.data() + file.size() - 16, 16);
    std::string z(n, 0);
    if (!gcm(false, key, nonce, aad, (const uint8_t *)file.data() + h.body + 12, n, (uint8_t *)&z[0], tag)) { wipe(z); return OPEN_WRONG_PASSWORD; }
    std::string packed;
    bool ok = h.format == 2 ? unblock(z, packed) : decompress(z, packed);
    wipe(z);
    if (!ok) return OPEN_DAMAGED;
    ok = unpack(packed, out);
    wipe(packed);
    if (!ok) { for (auto &e : out) wipe(e.data); out.clear(); return OPEN_DAMAGED; }
    return OPEN_OK;
}

}  // namespace bk
