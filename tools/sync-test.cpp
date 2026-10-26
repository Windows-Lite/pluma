// Pluma: checks the backup file (src/backup.h): the one place where a slip loses someone's passwords, or lets a
// crafted file write outside Pluma's folder on restore.
//   cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX tools\sync-test.cpp src\monocypher\monocypher.c bcrypt.lib cabinet.lib
//   sync-test.exe
#include "../src/backup.h"
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char *what) { if (!ok) { fails++; printf("FALLA: %s\n", what); } }

int main() {
    // 1. Argon2id as RFC 9106 (5.3): Monocypher as vendored gives the reference tag
    {
        uint8_t pass[32], salt[16], secret[8], ad[12], tag[32];
        memset(pass, 1, 32); memset(salt, 2, 16); memset(secret, 3, 8); memset(ad, 4, 12);
        static const uint8_t want[32] = { 0x0d, 0x64, 0x0d, 0xf5, 0x8d, 0x78, 0x76, 0x6c, 0x08, 0xc0, 0x37, 0xa3, 0x4a, 0x8b, 0x53, 0xc9,
                                          0xd0, 0x1e, 0xf0, 0x45, 0x2d, 0x75, 0xb6, 0x5e, 0xb5, 0x25, 0x20, 0xe9, 0x6b, 0x01, 0xe6, 0x59 };
        std::vector<uint8_t> work(32 * 1024);
        crypto_argon2_config cfg = { CRYPTO_ARGON2_ID, 32, 3, 4 };
        crypto_argon2_inputs in = { pass, salt, 32, 16 };
        crypto_argon2_extras ex = { secret, ad, 8, 12 };
        crypto_argon2(tag, 32, work.data(), cfg, in, ex);
        check(!memcmp(tag, want, 32), "Argon2id: vector de RFC 9106");
    }
    // 2. a file round trip, with Pluma's real parameters (64 MB)
    bk::Kdf k;
    k.salt.resize(16);
    check(bk::random_bytes(&k.salt[0], 16), "sal aleatoria");
    uint8_t key[32], key2[32], wrong[32];
    DWORD t0 = GetTickCount();
    check(bk::derive("correct horse battery staple", k, key), "derivar la llave");
    printf("Argon2id (64 MB, 3 pasadas, 4 carriles): %u ms\n", (unsigned)(GetTickCount() - t0));
    check(bk::derive("correct horse battery staple", k, key2) && !memcmp(key, key2, 32), "misma contraseña, misma llave");
    check(bk::derive("correct horse battery stapl", k, wrong) && memcmp(key, wrong, 32), "otra contraseña, otra llave");
    std::vector<bk::Entry> in = { { "pluma/historial.tsv", "https://a.example/\tA\t3\t1\n" },
                                  { "pluma/scripts/x.user.js", std::string(100000, 'x') },
                                  { "motor/Default/Local Extension Settings/abc/000003.log", std::string("leveldb\0\1\2", 10) },
                                  { "motor/Default/Extension State/000005.ldb", "" },
                                  { "motor/Default/IndexedDB/https_a.example_0.indexeddb.leveldb/000003.log", "indexeddb" },
                                  { "meta/clave", std::string(32, '\x7f') } };
    std::string header = bk::header_json(k, ",\"equipo\":\"TEST\",\"pista\":\"la de siempre\"");
    std::string file;
    check(bk::seal(key, header, in, file), "cifrar");
    check(file.find("a.example") == std::string::npos && file.find("leveldb") == std::string::npos && file.find("IndexedDB") == std::string::npos, "nada en claro (tampoco los nombres)");
    bk::Header h;
    check(bk::read_header(file, h) && h.kdf.salt == k.salt && h.kdf.m_kib == 65536 && h.j[L"pista"].s == L"la de siempre", "leer la cabecera");
    std::vector<bk::Entry> out;
    check(bk::open(file, h, key, out) == bk::OPEN_OK && out.size() == in.size(), "abrir con la contraseña");
    bool same = out.size() == in.size();
    for (size_t i = 0; same && i < in.size(); i++) same = out[i].name == in[i].name && out[i].data == in[i].data;
    check(same, "lo mismo que entró");
    check(bk::open(file, h, wrong, out) == bk::OPEN_WRONG_PASSWORD && out.empty(), "contraseña mal: nada");
    // 3. any byte changed (the header too: it is the additional data) and it does not open
    for (size_t at : { (size_t)14, h.body + 3, file.size() / 2, file.size() - 1 }) {
        std::string bad = file;
        bad[at] ^= 1;
        bk::Header hb;
        bool refused = !bk::read_header(bad, hb) || bk::open(bad, hb, key, out) != bk::OPEN_OK;
        check(refused && out.empty(), "un byte cambiado no abre");
    }
    {
        std::string cut = file.substr(0, file.size() - 40);
        bk::Header hc;
        check(!bk::read_header(cut, hc) || bk::open(cut, hc, key, out) != bk::OPEN_OK, "cortado no abre");
        check(!bk::read_header("PK\3\4 not ours at all, not ours at all", hc), "otro fichero no es una copia");
    }
    // a header asking for 4 GB of memory is refused before deriving anything
    {
        bk::Kdf big = k; big.m_kib = 4u << 20;
        std::string f2 = file;
        std::string h2 = bk::header_json(big, "");
        bk::Header hb;
        std::string crafted(bk::kMagic, 8);
        bk::put32(crafted, (uint32_t)h2.size()); crafted += h2; crafted += std::string(64, 0);
        check(!bk::read_header(crafted, hb), "cabecera con 4 GB rechazada");
    }
    // 4. names: only what a restore may write. The whole engine profile goes in, but never its caches (at any depth),
    // leveldb's lock and logs, or anything outside a profile; and nothing climbs out of the data folder
    for (const char *ok : { "pluma/ajustes.ini", "pluma/scripts/a.user.js", "pluma/extensiones/abc/manifest.json",
                            "motor/Default/Local Extension Settings/ddkjiahejlhfcafbddmgiahcphecmpfh/000003.log", "motor/Default/Extension State/CURRENT", "meta/clave",
                            "motor/Default/Login Data", "motor/Default/Network/Cookies", "motor/WV2Profile_pluma-trabajo/Login Data",
                            "motor/Default/Web Data", "motor/Default/History-journal", "motor/Default/Preferences", "motor/Default/Secure Preferences",
                            "motor/Default/Local Storage/leveldb/000003.log", "motor/Default/IndexedDB/https_a.example_0.indexeddb.leveldb/CURRENT",
                            "motor/Default/Service Worker/ScriptCache/index", "motor/Default/Service Worker/CacheStorage/x/index",
                            "motor/Default/Network/Trust Tokens", "motor/Default/Extensions/abc/1.0_0/manifest.json",
                            "pluma/perfiles.tsv", "pluma/perfiles/trabajo/historial.tsv", "pluma/traducir-nunca.tsv" })
        check(bk::allowed_target(ok), ok);
    for (const char *no : { "../x", "pluma/../ajustes.ini", "pluma/./ajustes.ini", "C:/Windows/x", "/pluma/ajustes.ini", "pluma\\ajustes.ini",
                            "pluma/scripts", "pluma/scripts/", "pluma/otro.txt", "pluma/scripts/../../x",
                            "motor/Local State", "motor/Login Data", "motor/Default", "motor/Default/",
                            "motor/WV2Profile_otra/Login Data", "motor/WV2Profile_pluma-X/Login Data", "motor/WV2Profile_pluma-/Login Data",
                            "motor/../Default/Login Data", "motor/Default/../../ajustes.ini", "motor/Default/IndexedDB/../../../x",
                            "motor/Default/Cache/Cache_Data/data_0", "motor/Default/Code Cache/js/index", "motor/Default/GPUCache/data_1",
                            "motor/Default/Storage/ext/abc/def/GPUCache/data_1", "motor/Default/DawnWebGPUCache/data_0",
                            "motor/Default/Local Storage/leveldb/LOCK", "motor/Default/Local Storage/leveldb/LOG", "motor/Default/History-shm",
                            "motor/Default/Preferences.tmp", "motor/Default/C:x", "pluma/perfiles/trabajo/cache-video/a.m3u8",
                            "pluma/scripts/a.user.js.", "pluma/scripts/con:x", "pluma/scripts/NUL.js", "pluma/iconos/com1",
                            "meta/otra", "pluma/sesion.tsv/x", "" })
        check(!bk::allowed_target(no), no);
    // 5. an archive with a lying length, or one bad name among good ones, is refused whole
    {
        std::string a = bk::pack({ { "pluma/ajustes.ini", "x" } });
        std::string lie = a; lie[8 + 4 + 17] = (char)0xff;   // the data length's low byte
        check(!bk::unpack(lie, out), "longitud mentirosa");
        std::string evil = bk::pack({ { "pluma/ajustes.ini", "x" }, { "pluma/../../evil.exe", "MZ" } });
        check(!bk::unpack(evil, out) && out.empty(), "un nombre malo y se rechaza todo");
        check(!bk::unpack(a + "x", out), "bytes de más");
        check(bk::unpack(a, out) && out.size() == 1, "archivo bueno");
        // the packer that the backup fills one file at a time makes the same archive
        bk::Packer pk;
        pk.add("pluma/ajustes.ini", "x");
        pk.add("motor/Default/Web Data", std::string("SQLite format 3\0", 16));
        std::string b = bk::pack({ { "pluma/ajustes.ini", "x" }, { "motor/Default/Web Data", std::string("SQLite format 3\0", 16) } });
        check(pk.finish() == b && bk::unpack(b, out) && out.size() == 2, "empaquetar de uno en uno");
        std::string packed = b, f3;
        check(bk::seal_packed(key, header, packed, f3) && packed.empty(), "cifrar lo empaquetado (y borrarlo)");
        bk::Header h3;
        check(bk::read_header(f3, h3) && bk::open(f3, h3, key, out) == bk::OPEN_OK && out.size() == 2 && out[1].data.size() == 16, "y abrirlo");
    }
    // 6. formato 2 (what Pluma writes since the whole browser goes in): compressed block by block as it is filled,
    // encrypted in place; a big file crosses blocks. A formato 1 file (from before) still opens.
    {
        std::string big(9u << 20, 0);
        for (size_t i = 0; i < big.size(); i++) big[i] = (char)((i * 2654435761u) >> 13);
        bk::Sealer sl(bk::header_json(k, ",\"equipo\":\"TEST\"", 2));
        for (auto &e : in) sl.add(e.name, e.data);
        sl.add("motor/Default/Service Worker/CacheStorage/x/0", big);
        std::string f2;
        check(sl.finish(key, f2), "formato 2: cifrar");
        check(f2.find("a.example") == std::string::npos && f2.find("IndexedDB") == std::string::npos && f2.find("CacheStorage") == std::string::npos, "formato 2: nada en claro");
        bk::Header h2;
        check(bk::read_header(f2, h2) && h2.format == 2, "formato 2: cabecera");
        check(bk::open(f2, h2, key, out) == bk::OPEN_OK && out.size() == in.size() + 1, "formato 2: abrir");
        bool same2 = out.size() == in.size() + 1;
        for (size_t i = 0; same2 && i < in.size(); i++) same2 = out[i].name == in[i].name && out[i].data == in[i].data;
        check(same2 && out.back().data == big, "formato 2: lo mismo que entró (también lo que cruza bloques)");
        check(bk::open(f2, h2, wrong, out) == bk::OPEN_WRONG_PASSWORD && out.empty(), "formato 2: contraseña mal");
        for (size_t at : { (size_t)14, h2.body + 3, f2.size() / 2, f2.size() - 1 }) {
            std::string bad = f2; bad[at] ^= 1;
            bk::Header hb;
            check((!bk::read_header(bad, hb) || bk::open(bad, hb, key, out) != bk::OPEN_OK) && out.empty(), "formato 2: un byte cambiado no abre");
        }
        std::string cut = f2.substr(0, f2.size() - 40);
        bk::Header hc;
        check(!bk::read_header(cut, hc) || bk::open(cut, hc, key, out) != bk::OPEN_OK, "formato 2: cortado no abre");
        check(bk::read_header(file, h) && h.format == 1 && bk::open(file, h, key, out) == bk::OPEN_OK && out.size() == in.size(), "formato 1 sigue abriendo");
        // the blocks after decryption: lying lengths, no end mark or bytes after it are refused
        std::string blk, z, raw;
        bk::compress(std::string("PLAR\xff\xff\xff\xff", 8), z);
        bk::put32(blk, 8); bk::put32(blk, (uint32_t)z.size()); blk += z;
        check(bk::unblock(blk + std::string(4, 0), raw) && raw.size() == 8, "bloques: bien");
        check(!bk::unblock(blk, raw), "bloques: sin marca de fin");
        check(!bk::unblock(blk + std::string(4, 0) + "x", raw), "bloques: bytes tras el fin");
        std::string lie = blk; lie[4] = (char)0x7f;   // the compressed length runs past the end
        check(!bk::unblock(lie + std::string(4, 0), raw), "bloques: longitud mentirosa");
        std::string huge; bk::put32(huge, (uint32_t)(bk::kBlock + 1)); bk::put32(huge, 1); huge += "x"; bk::put32(huge, 0);
        check(!bk::unblock(huge, raw), "bloques: bloque demasiado grande");
    }
    printf(fails ? "%d fallos\n" : "todo bien\n", fails);
    return fails ? 1 : 0;
}
