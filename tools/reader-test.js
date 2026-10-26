// node tools/reader-test.js <port> <captura.exe> <window class> [captura's action: key 0x78 (F9) | click x y (the book)]
// The reader view's cleaner against hostile HTML, in a test instance (PLUMA_DEPURAR=<port>): serves an article full of
// script vectors on 127.0.0.1, opens it, presses F9 (captura.exe) and checks the reader page: nothing ran, nothing
// dangerous is left, the text is there. Then __plumaClean() on each vector alone. Exit code 0 = all good.
const http = require('http');
const { execFileSync } = require('child_process');
const [port, captura, cls, ...action] = process.argv.slice(2);

const VECTORS = [
  `<script>window.__pwn='script'</script>`,
  `<img src=x onerror="window.__pwn='onerror'">`,
  `<a href="javascript:window.__pwn='js'">js link</a>`,
  `<a href="  JaVaScRiPt:window.__pwn='js2'">js link 2</a>`,
  `<a href="&#106;avascript:window.__pwn='js3'">js link 3</a>`,
  `<iframe src="javascript:parent.__pwn='iframe'"></iframe>`,
  `<iframe srcdoc="<script>parent.__pwn='srcdoc'</script>"></iframe>`,
  `<svg><script>window.__pwn='svg'</script><a href="javascript:x"><text>svg</text></a></svg>`,
  `<svg><animate onbegin="window.__pwn='animate'" attributeName="x" dur="1s"/></svg>`,
  `<math><mtext><table><mglyph><style><img src=x onerror="window.__pwn='mxss'"></style></mglyph></table></mtext></math>`,
  `<noscript><p title="</noscript><img src=x onerror=window.__pwn='noscript'>"></p></noscript>`,
  `<form action="https://evil.example/"><input name=x autofocus onfocus="window.__pwn='focus'"><button>go</button></form>`,
  `<div style="background:url(javascript:alert(1))">styled</div>`,
  `<img src="data:image/svg+xml;base64,PHN2ZyBvbmxvYWQ9ImFsZXJ0KDEpIj48L3N2Zz4=">`,
  `<object data="https://evil.example/x.swf"></object><embed src="https://evil.example/x.swf">`,
  `<details open ontoggle="window.__pwn='toggle'"><summary>more</summary>inside</details>`,
  `<p id="pluma-data">clobber</p><p id="content">clobber 2</p>`,
  `<meta http-equiv="refresh" content="0;url=javascript:alert(1)"><base href="javascript:/">`,
  `<a href="https://ok.example/page" target="_blank" onclick="window.__pwn='click'">ok link</a>`,
  `<img src="https://ok.example/a.png" srcset="javascript:alert(1) 1x, https://ok.example/b.png 2x">`,
  `<template><img src=x onerror="window.__pwn='template'"></template>`,
  `<style>body{display:none}</style><link rel="stylesheet" href="https://evil.example/x.css">`,
  `<video src="https://evil.example/v.mp4" autoplay onplay="window.__pwn='video'"></video>`,
];
const BAD = /<script|<iframe|<svg|<math|<style|<form|<input|<object|<embed|<meta|<base|<link|<template|<video|<noscript|\son\w+\s*=|javascript:|data:image\/svg|\sstyle=|\sid="(pluma-data|content)"/i;

const para = (i) => `<p>Paragraph ${i} of a test article for the reader view of Pluma, long enough for Readability to take it as the
  body of an article and not as a menu, a footer or an advertisement. It goes on with a few more words, commas, and full stops.</p>`;
const article = `<!doctype html><html lang="en"><head><meta charset="utf-8"><title>A hostile article</title>
<meta property="og:type" content="article"><meta property="og:site_name" content="Test site"></head><body>
<header><nav><a href="/">Home</a> <a href="/a">Section</a></nav></header>
<article><h1>A hostile article</h1><p class="byline">By Test Author</p>
${[0, 1, 2].map(para).join('')}
${VECTORS.map((v, i) => `<p>Vector ${i} keeps this sentence around it so that the paragraph stays in the article text.</p>${v}`).join('\n')}
${[3, 4, 5, 6].map(para).join('')}
</article><footer>footer</footer></body></html>`;

const server = http.createServer((q, r) => { r.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); r.end(article); });
let fails = 0;
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) fails++; };

server.listen(0, '127.0.0.1', async () => {
  const url = `http://127.0.0.1:${server.address().port}/2026/10/hostile-article.html`;
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const t = list.find((x) => x.type === 'page' && x.url.startsWith('http'));
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let n = 0; const wait = {};
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && wait[d.id]) { wait[d.id](d.result); delete wait[d.id]; } };
  const call = (method, params = {}) => new Promise((r) => { const id = ++n; wait[id] = r; ws.send(JSON.stringify({ id, method, params })); });
  const ev = async (e) => { const r = await call('Runtime.evaluate', { expression: e, returnByValue: true }); return r && r.result && r.result.value; };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  await call('Page.navigate', { url });
  await sleep(2500);
  execFileSync(captura, [cls, require('path').join(require('os').tmpdir(), 'reader-test.png'), ...(action.length ? action : ['key', '0x78'])]);
  let reader = false;
  for (let i = 0; i < 40 && !reader; i++) { await sleep(250); reader = !!(await ev(`!!document.querySelector('meta[name="pluma-reader"]') && !!document.querySelector('#content p')`)); }
  check(reader, 'F9 (or the book) opens the reader view at the same address');
  if (reader) {
    await sleep(1500);
    check((await ev('location.href')) === url, 'same address');
    check((await ev('window.__pwn')) === undefined, 'nothing of the article ran');
    check((await ev(`[...document.scripts].filter((s) => s.type !== 'speculationrules').length`)) === 2, 'only the reader page\'s two scripts (and Pluma\'s speculation rules)');
    const html = await ev(`document.getElementById('content').innerHTML`);
    check(!BAD.test(html), 'the article on screen has none of the vectors');
    check(/Paragraph 6 of a test article/.test(html) && /Vector 22 keeps/.test(html), 'the article\'s text is all there');
    check(/href="https:\/\/ok\.example\/page"/.test(html), 'a safe link stays');
    check((await ev(`document.title`)) === 'A hostile article', 'title');
    for (let i = 0; i < VECTORS.length; i++) {
      const out = await ev(`__plumaClean(${JSON.stringify(VECTORS[i])})`);
      check(typeof out === 'string' && !BAD.test(out), `vector ${i} alone: ${JSON.stringify(out)}`);
    }
    check((await ev('window.__pwn')) === undefined, 'nothing ran while cleaning');
  }
  ws.close(); server.close();
  console.log(fails ? `${fails} failed` : 'all passed');
  process.exit(fails ? 1 : 0);
});
