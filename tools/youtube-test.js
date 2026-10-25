// node tools/youtube-test.js
// The light YouTube page's parsers (src/youtube.html, between "// PARSER" and "// PARSER_FIN") against real answers
// of YouTube's API, trimmed (tools/yt-respuestas). The risk they cover: YouTube changes its answers, and a page that
// shows nothing. Run by hand after touching the parsers or when YouTube changes something.
const fs = require('fs'), path = require('path'), assert = require('assert');
const root = path.join(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'src', 'youtube.html'), 'utf8');
const a = html.indexOf('// PARSER:'), b = html.indexOf('// PARSER_FIN');
assert(a > 0 && b > a, 'no encuentro el bloque PARSER en youtube.html');
const mod = { exports: {} };
new Function('module', 't', html.slice(a, b) + '\nmodule.exports = { collect, appended, parseBrowse, parseSearch, parseNext, parseComments, parseSuggest, attributed, startSeconds };')(mod, (s) => s);
const Y = mod.exports;
const read = (n) => JSON.parse(fs.readFileSync(path.join(__dirname, 'yt-respuestas', n), 'utf8'));
let n = 0;
const test = (name, f) => { f(); n++; console.log('ok', name); };

test('portada sin sesión: sin vídeos y con su aviso', () => {
  const r = Y.parseBrowse(read('home-logged-out.json'));
  assert.strictEqual(r.items.length, 0);
  assert(r.note && r.note.title, 'falta el aviso de la portada vacía');
});
test('búsqueda: vídeos y canales con título, enlace y miniatura', () => {
  const r = Y.parseSearch(read('search.json'));
  const v = r.items.filter((x) => x.kind === 'video');
  assert(v.length >= 3, 'pocos vídeos: ' + v.length);
  for (const x of v) {
    assert(x.title && x.thumb.startsWith('https://') && /^\/watch\?v=/.test(x.url), JSON.stringify(x));
    assert(x.channel && x.channelUrl.startsWith('/'), 'canal: ' + JSON.stringify(x));
  }
  // the lives go to YouTube's own page
  for (const x of v.filter((x) => x.live)) assert(x.url.includes('pluma_normal=1'));
  assert(r.cont, 'falta el token para más resultados');
});
test('canal: cabecera y vídeos (lockupViewModel)', () => {
  const r = Y.parseBrowse(read('channel-videos.json'));
  assert(r.header && r.header.title === 'MrBeast', JSON.stringify(r.header));
  assert(r.header.avatar.startsWith('https://'));
  assert(r.items.length === 4, 'vídeos: ' + r.items.length);
  for (const x of r.items) assert(x.kind === 'video' && x.title && x.thumb && x.url.startsWith('/watch?v=') && x.duration && x.meta.length, JSON.stringify(x));
  assert(r.cont);
});
test('vídeo: título, canal, me gusta, suscribirse, descripción con enlaces, recomendados, comentarios', () => {
  const r = Y.parseNext(read('next.json'), 'dQw4w9WgXcQ');
  assert(/Never Gonna Give You Up/.test(r.title));
  assert.strictEqual(r.channel, 'Rick Astley');
  assert(r.channelUrl.startsWith('/') && r.channelId.startsWith('UC'));
  assert(r.likes && r.likeParams && r.removeLikeParams, 'me gusta: ' + JSON.stringify([r.likes, r.likeParams, r.removeLikeParams]));
  assert(r.subParams && r.unsubParams);
  const links = r.desc.filter((s) => s.link);
  assert(links.length > 0, 'la descripción sin enlaces');
  assert(r.desc.map((s) => s.text).join('').length > 100);
  // YouTube's /redirect links come out unwrapped, and go out of YouTube
  const ext = links.filter((s) => s.link.external);
  assert(ext.length && ext.every((s) => !s.link.url.includes('youtube.com/redirect')), JSON.stringify(ext.slice(0, 2)));
  assert(r.related.length === 4 && r.related.every((x) => x.title && x.url && x.thumb), JSON.stringify(r.related[0]));
  assert(r.relatedCont && r.commentsToken);
});
test('comentarios: unidos a sus datos por su clave', () => {
  const r = Y.parseComments(read('comments.json'), 'dQw4w9WgXcQ');
  assert.strictEqual(r.items.length, 3);
  for (const c of r.items) assert(c.author && c.text.length && c.avatar.startsWith('https://') && c.time, JSON.stringify(c));
  assert(r.items.some((c) => c.repliesToken), 'sin respuestas');
  assert(r.cont, 'sin token para más');
});
test('texto con marcas de tiempo: los segundos de este vídeo se saltan en el reproductor', () => {
  const s = 'Intro 0:00\nCoro 1:05 y https://example.com';
  const segs = Y.attributed({ content: s, commandRuns: [
    { startIndex: 6, length: 4, onTap: { innertubeCommand: { watchEndpoint: { videoId: 'abc', startTimeSeconds: 0 } } } },
    { startIndex: 16, length: 4, onTap: { innertubeCommand: { commandMetadata: { webCommandMetadata: { url: '/watch?v=abc&t=65s' } }, watchEndpoint: { videoId: 'abc', startTimeSeconds: 65 } } } },
    { startIndex: 23, length: 19, onTap: { innertubeCommand: { urlEndpoint: { url: 'https://www.youtube.com/redirect?event=x&q=https%3A%2F%2Fexample.com' } } } },
  ] }, 'abc');
  assert.strictEqual(segs.map((x) => x.text).join(''), s);
  assert.strictEqual(segs[1].link.seek, 0);
  assert.strictEqual(segs[3].link.seek, 65);
  assert.deepStrictEqual(segs[5].link, { url: 'https://example.com', external: true });
  // UTF-16: an emoji before a link does not move it
  const e = Y.attributed({ content: '😀 ver', commandRuns: [{ startIndex: 3, length: 3, onTap: { innertubeCommand: { watchEndpoint: { videoId: 'x', startTimeSeconds: 3 } } } }] }, 'x');
  assert.strictEqual(e[1].text, 'ver');
});
test('&t= de YouTube', () => {
  assert.strictEqual(Y.startSeconds('90'), 90);
  assert.strictEqual(Y.startSeconds('90s'), 90);
  assert.strictEqual(Y.startSeconds('1h2m3s'), 3723);
  assert.strictEqual(Y.startSeconds(''), 0);
});
test('sugerencias', () => {
  assert.deepStrictEqual(Y.parseSuggest(['lofi', [['lofi', 0, [512]], ['lofi girl', 0, []]], {}]), ['lofi', 'lofi girl']);
});
console.log(n + ' pruebas bien');
