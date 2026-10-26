// node tools/music-test.js
// The light YouTube Music page's parsers (src/music.html, between "// PARSER" and "// PARSER_FIN") against real
// answers of YouTube Music's API, trimmed (tools/ytm-respuestas). The risk they cover: YouTube Music changes its
// answers, and a page that shows nothing (or plays nothing). Run by hand after touching the parsers.
const fs = require('fs'), path = require('path'), assert = require('assert');
const root = path.join(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'src', 'music.html'), 'utf8');
const a = html.indexOf('// PARSER:'), b = html.indexOf('// PARSER_FIN');
assert(a > 0 && b > a, 'no encuentro el bloque PARSER en music.html');
const mod = { exports: {} };
new Function('module', 't', html.slice(a, b) + '\nmodule.exports = { items, shelves, parseHome, parseMore, parseSearch, parseBrowse, parseQueue, parseSuggest };')(mod, (s) => s);
const Y = mod.exports;
const read = (n) => JSON.parse(fs.readFileSync(path.join(__dirname, 'ytm-respuestas', n), 'utf8'));
let n = 0;
const test = (name, f) => { f(); n++; console.log('ok', name); };
const playable = (x) => x.videoId && /^[\w-]{11}$/.test(x.videoId) && x.title;

test('inicio: secciones con tarjetas o filas, y token para más', () => {
  const r = Y.parseHome(read('home.json'));
  assert(r.shelves.length >= 3, 'secciones: ' + r.shelves.length);
  for (const s of r.shelves) {
    assert(s.title, 'sección sin título');
    for (const it of s.items) assert(it.title && it.thumb.startsWith('https://') && (it.videoId || it.url || it.playlistId), JSON.stringify(it));
  }
  assert(r.shelves.some((s) => s.items.some(playable)), 'ninguna canción que se pueda reproducir');
  assert(r.cont && r.cont.ctoken, 'falta el token para más secciones');
});
test('búsqueda: canciones con duración, álbumes y artistas con su dirección', () => {
  const r = Y.parseSearch(read('search.json'));
  const all = [].concat(...r.shelves.map((s) => s.items));
  const songs = all.filter((x) => x.kind === 'song' || x.kind === 'video');
  assert(songs.length >= 3, 'canciones: ' + songs.length);
  assert(songs.every(playable), JSON.stringify(songs[0]));
  assert(songs.every((x) => x.sub), 'canciones sin subtítulo');
  assert(songs.some((x) => /^\d+:\d\d$/.test(x.duration)), 'canciones sin duración');
  assert(all.some((x) => x.kind === 'album' && x.url.startsWith('/browse/MPRE')), 'sin álbumes');
  assert(all.some((x) => x.kind === 'artist' && x.url.startsWith('/channel/UC')), 'sin artistas');
});
test('álbum: cabecera (título, artista, para reproducir) y sus canciones en orden', () => {
  const r = Y.parseBrowse(read('album.json'));
  assert.strictEqual(r.header.title, 'Discovery');
  assert(r.header.artists[0] && r.header.artists[0].name === 'Daft Punk', JSON.stringify(r.header.artists));
  assert(r.header.thumb.startsWith('https://') && r.header.playlistId, JSON.stringify(r.header));
  const list = r.shelves.find((s) => s.kind === 'list');
  assert(list && list.items.length >= 5 && list.items.every(playable), JSON.stringify(list && list.items[0]));
  assert(list.items.every((x) => /^\d+:\d\d$/.test(x.duration)), 'canciones sin duración');
});
test('lista: canciones con artista y token para más', () => {
  const r = Y.parseBrowse(read('playlist.json'));
  assert(r.header.title && r.header.second, JSON.stringify(r.header));
  const list = r.shelves.find((s) => s.kind === 'list');
  assert(list.items.length >= 5 && list.items.every(playable));
  assert(list.items[0].artists.length, 'sin artista');
  assert(list.cont && list.cont.body, 'falta el token para más canciones');
});
test('artista: cabecera de artista y sus canciones', () => {
  const r = Y.parseBrowse(read('artist.json'));
  assert(r.header && r.header.title && r.header.artist, JSON.stringify(r.header));
  const list = r.shelves.find((s) => s.kind === 'list');
  assert(list && list.items.some(playable), 'sin canciones');
  assert(r.shelves.some((s) => s.kind === 'cards' && s.items.some((x) => x.kind === 'album')), 'sin álbumes');
});
test('cola (next): las canciones que siguen, con su lista y token de la radio', () => {
  const r = Y.parseQueue(read('next.json'));
  assert(r.items.length >= 5 && r.items.every(playable), JSON.stringify(r.items[0]));
  assert.strictEqual(r.items[0].videoId, 'ARfJ55bUAw0');
  assert(r.items.every((x) => x.sub && x.duration), JSON.stringify(r.items[1]));
  assert(r.playlistId.startsWith('RDAMVM'));
});
test('sugerencias de búsqueda', () => {
  const s = Y.parseSuggest(read('suggest.json'));
  assert(s.length >= 3 && s.every((x) => typeof x === 'string' && x), JSON.stringify(s));
});
console.log(n + ' pruebas bien');
