// node tools/lite-medir.js <port> <url> [cold] [seconds]
// Loads a page of YouTube Music or Reddit in the first tab of a test instance (PLUMA_DEPURAR=<port>) and measures
// what Pluma's light pages are about, against the site's own: bytes that came over the network (compressed, as
// transferred; googlevideo.com and v.redd.it apart: the music or the video itself, which Pluma's player downloads
// outside the page), requests, and when the content is on screen (the first card, row, post or comment drawn, with
// its picture loaded, light page or the site's). cold: the cache and the site's service worker go first.
// (tools/yt-medir.js is the same for YouTube.)
const [port, url, cold, secs = '12'] = process.argv.slice(2);
const origin = new URL(url).origin;
const probes = {
  'https://music.youtube.com': {
    lite: "!!document.getElementById('bar') && !document.querySelector('ytmusic-app')",
    liteSel: 'main .card img, main .trk img, main .head img.big',
    normalSel: 'ytmusic-two-row-item-renderer img, ytmusic-responsive-list-item-renderer img, ytmusic-responsive-header-renderer img',
  },
  'https://www.reddit.com': {
    lite: "!!document.getElementById('rl') && !document.querySelector('shreddit-app')",
    liteSel: 'main .post h3, main .cm .md',
    normalSel: 'shreddit-post [slot=title], shreddit-comment [slot=comment]',
  },
}[origin];
if (!probes) { console.error('no sé medir', origin); process.exit(1); }
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  setTimeout(() => { console.log(JSON.stringify({ url, error: 'timeout' })); process.exit(2); }, 60000);
  const pages = list.filter((x) => x.type === 'page' && /^https?:/.test(x.url));
  const t = pages.find((x) => x.url.startsWith(origin)) || pages[0];
  if (!t) { console.error('no page'); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const waiting = new Map(), reqs = new Map();
  let bytes = 0, media = 0, count = 0, failed = 0;
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d.result || d.error); waiting.delete(d.id); return; }
    if (d.method === 'Network.requestWillBeSent') reqs.set(d.params.requestId, d.params.request.url);
    else if (d.method === 'Network.loadingFinished') {
      const u = reqs.get(d.params.requestId) || '';
      if (/googlevideo\.com|v\.redd\.it/.test(u)) media += d.params.encodedDataLength; else { bytes += d.params.encodedDataLength; count++; }
    } else if (d.method === 'Network.loadingFailed') failed++;
  };
  const call = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  await call('Network.enable', {});
  if (cold === 'cold') {
    await call('Network.clearBrowserCache');
    await call('Storage.clearDataForOrigin', { origin, storageTypes: 'cache_storage,service_workers' });
  }
  await call('Page.navigate', { url: 'about:blank' });
  await new Promise((r) => setTimeout(r, 700));
  bytes = 0; media = 0; count = 0; failed = 0; reqs.clear();
  const t0 = Date.now();
  await call('Page.navigate', { url });
  const probe = `(() => {
    const lite = ${probes.lite};
    const e = document.querySelector(lite ? ${JSON.stringify(probes.liteSel)} : ${JSON.stringify(probes.normalSel)});
    return [!!e && (e.tagName !== 'IMG' || (e.complete && e.naturalWidth > 0)), lite];
  })()`;
  let shown = -1, lite = false;
  while (Date.now() - t0 < 25000) {
    const r = await call('Runtime.evaluate', { expression: probe, returnByValue: true });
    const v = r && r.result && r.result.value;
    if (v && v[0]) { shown = Date.now() - t0; lite = v[1]; break; }
    await new Promise((r) => setTimeout(r, 50));
  }
  await new Promise((r) => setTimeout(r, Math.max(0, +secs * 1000 - (Date.now() - t0))));
  console.log(JSON.stringify({ url, cold: cold === 'cold', page: lite ? 'ligera' : 'normal', content_ms: shown, mb: +(bytes / 1048576).toFixed(2),
                               requests: count, failed, media_mb: +(media / 1048576).toFixed(2) }));
  process.exit(0);
})();
