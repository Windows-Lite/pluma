// node tools/reddit-test.js
// The light Reddit page's readers (src/reddit.html, between "// PARSER" and "// PARSER_FIN") against Reddit's real
// HTML, trimmed (tools/reddit-respuestas). The risks they cover: Reddit changes its pages and the light one shows
// nothing; a check page taken for content; a post's or comment's HTML that runs something at www.reddit.com with the
// session. Run by hand after touching the readers. Needs linkedom (a DOM for node), kept outside the repo:
//   cd %LOCALAPPDATA%\fp-tools\linkedom && npm install linkedom   (or LINKEDOM=<its folder>)
const fs = require('fs'), path = require('path'), assert = require('assert');
const where = process.env.LINKEDOM || path.join(process.env.LOCALAPPDATA || '', 'fp-tools', 'linkedom', 'node_modules', 'linkedom');
const { DOMParser } = require(where);
const root = path.join(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'src', 'reddit.html'), 'utf8');
const a = html.indexOf('// PARSER:'), b = html.indexOf('// PARSER_FIN');
assert(a > 0 && b > a, 'no encuentro el bloque PARSER en reddit.html');
const mod = { exports: {} };
new Function('module', html.slice(a, b) + '\nmodule.exports = { unfold, blocked, parseListing, parseSearch, parseComments, parsePost, cleanBody };')(mod);
const R = mod.exports;
const doc = (n) => new DOMParser().parseFromString(fs.readFileSync(path.join(__dirname, 'reddit-respuestas', n), 'utf8'), 'text/html');
let n = 0;
const test = (name, f) => { f(); n++; console.log('ok', name); };

test('subreddit: publicaciones con título, dirección, votos, imágenes, y la página siguiente', () => {
  const d = doc('sub.html');
  assert(!R.blocked(d), 'la tomó por la página de comprobación');
  const r = R.parseListing(d);
  assert(r.items.length >= 5, 'publicaciones: ' + r.items.length);
  for (const x of r.items) {
    assert(x.kind === 'post' && /^\w+$/.test(x.id) && x.title, JSON.stringify(x));
    assert(x.permalink.startsWith('/r/pics/comments/' + x.id + '/'), x.permalink);
    assert(x.sub === 'r/pics' && x.author && x.created && Number.isFinite(x.score), JSON.stringify(x));
  }
  assert(r.items.some((x) => x.images.length && /^https:\/\/(preview|i)\.redd\.it\//.test(x.images[0].src)), 'ninguna imagen');
  assert(r.items.some((x) => x.images.length > 1), 'ninguna galería');
  assert(/^\/svc\/shreddit\/community-more-posts\/hot\/\?.*after=/.test(r.next), 'siguiente: ' + r.next);
});
test('publicación: su vídeo (HLS de v.redd.it), y los comentarios anidados como vienen, con sus "más respuestas"', () => {
  const r = R.parsePost(doc('post.html'));
  assert(r && r.post.title && /^\w+$/.test(r.post.id), 'sin publicación');
  assert(/^https:\/\/v\.redd\.it\/[\w-]+\/HLSPlaylist\.m3u8/.test(r.post.video), 'vídeo: ' + r.post.video);
  assert(r.comments.length >= 5, 'comentarios de arriba: ' + r.comments.length);
  const all = []; const walk = (c) => { all.push(c); c.kids.forEach(walk); };
  r.comments.forEach(walk);
  assert(all.length >= 20, 'comentarios: ' + all.length);
  assert(all.every((c) => c.id && (c.deleted || (c.author && c.body))), 'un comentario sin autor o sin texto');
  assert(r.comments.some((c) => c.kids.length && c.kids.every((k) => k.depth === c.depth + 1)), 'sin respuestas anidadas');
  const more = all.flatMap((c) => c.more);
  assert(more.length && more.every((m) => /^\/svc\/shreddit\/more-comments\//.test(m.src) && m.fields.some(([k, v]) => k === 'cursor' && v)), 'sin "más respuestas"');
  assert(r.more && /top-level=1/.test(r.more.src) && r.more.fields.some(([k]) => k === 'cursor'), 'sin "más comentarios"');
});
test('más comentarios (lo que contesta Reddit al POST): comentarios y lo que sigue', () => {
  const r = R.parseComments(doc('more.html'));
  assert(r.comments.length >= 5 && r.comments.every((c) => c.id && (c.deleted || c.body)), 'comentarios: ' + r.comments.length);
});
test('búsqueda y persona: resultados con su dirección, comentarios de su perfil, y lo siguiente', () => {
  const s = R.parseSearch(doc('search.html'));
  assert(s.items.length >= 5 && s.items.every((x) => x.title && /\/comments\/\w+\//.test(x.permalink) && x.sub.startsWith('r/')), JSON.stringify(s.items[0]));
  assert(/^\/svc\/shreddit\/search\/\?.*cursor=/.test(s.next), 'siguiente: ' + s.next);
  const u = R.parseListing(doc('user.html'));
  const cs = u.items.filter((x) => x.kind === 'comment');
  assert(cs.length >= 5 && cs.every((c) => c.body && (c.href.startsWith('/user/') || c.href.startsWith('/r/'))), JSON.stringify(cs[0]));
  assert(/profile_overview-more-posts/.test(u.next), 'siguiente: ' + u.next);
});
test('la página de "Prove your humanity" no se toma por contenido', () => {
  assert(R.blocked(doc('challenge.html')));
});
test('el texto de un comentario entra limpio: nada que se ejecute, enlaces y fotos solo de fiar', () => {
  const live = new DOMParser().parseFromString('<!doctype html><html><body></body></html>', 'text/html');
  const src = new DOMParser().parseFromString(`<div class="md"><p onclick="x()" style="color:red" id="top">hola <b onmouseover="y()">b</b></p>
    <img src="x" onerror="alert(1)"><img src="https://i.redd.it/a.png" onload="z()"><img src="https://evil.example/t.png">
    <a href="javascript:alert(1)">j</a><a href="//evil.example/">p</a><a href="https://example.com/x" target="_top">ok</a><a href="/r/pics/">r</a>
    <script>alert(1)</script><iframe src="https://evil.example/"></iframe><form action="/x"><input name="a"></form>
    <svg><a href="javascript:1"><text>s</text></a></svg><math><mi>m</mi></math><template><img src=x onerror=1></template>
    <table><tr><td align="left" onclick="q()">t</td></tr></table><blockquote>c</blockquote></div>`, 'text/html');
  const out = R.cleanBody(src.querySelector('.md'), live);
  const h = out.outerHTML;
  assert(!/\son\w+=|javascript:|<script|<iframe|<form|<input|<svg|<math|<template|style=|id=|target=|evil\.example/i.test(h), h);
  const imgs = [...out.querySelectorAll('img')].map((i) => i.getAttribute('src'));
  assert(imgs.length === 1 && imgs[0] === 'https://i.redd.it/a.png', 'fotos: ' + imgs);
  const links = [...out.querySelectorAll('a')].map((x) => x.getAttribute('href') + ' ' + x.getAttribute('rel'));
  assert.deepStrictEqual(links, ['https://example.com/x noopener noreferrer', '/r/pics/ noopener noreferrer']);
  assert(/hola <b>b<\/b>/.test(h) && /<td align="left">t<\/td>/.test(h) && /<blockquote>c<\/blockquote>/.test(h), h);
});
console.log(n + ' pruebas bien');
