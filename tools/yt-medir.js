// node tools/yt-medir.js <port> <url> [cold] [seconds]
// Loads a page in the first tab of a test instance (PLUMA_DEPURAR=<port>) and measures what YouTube's light page is
// about: bytes that came over the network (compressed, as transferred; googlevideo.com apart: the video itself,
// which Pluma's player downloads outside the page anyway), requests, and when the content is on screen (the first
// video, result or title drawn, light page or YouTube's). cold: the cache and YouTube's service worker go first.
const [port, url, cold, secs = '12'] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  setTimeout(() => { console.log(JSON.stringify({ url, error: 'timeout' })); process.exit(2); }, 60000);
  // the tab (not Pluma's empty keeper view, about:blank)
  const pages = list.filter((x) => x.type === 'page' && /^https?:/.test(x.url));
  const t = pages.find((x) => x.url.includes('youtube.com')) || pages[0];
  if (!t) { console.error('no page'); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const waiting = new Map();
  const reqs = new Map();
  let bytes = 0, video = 0, count = 0, failed = 0;
  const blocked = [], stats = [];
  const statsUrl = (u) => /\/api\/stats\/(playback|watchtime)/.test(u);
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d.result || d.error); waiting.delete(d.id); return; }
    if (d.method === 'Network.requestWillBeSent') { reqs.set(d.params.requestId, d.params.request.url); }
    else if (d.method === 'Network.responseReceived' && statsUrl(d.params.response.url)) stats.push(d.params.response.url.split('?')[0].split('/').pop() + ' ' + d.params.response.status);
    else if (d.method === 'Network.loadingFinished') {
      const u = reqs.get(d.params.requestId) || '';
      if (/googlevideo\.com/.test(u)) video += d.params.encodedDataLength; else { bytes += d.params.encodedDataLength; count++; }
    } else if (d.method === 'Network.loadingFailed') {
      failed++;
      if (statsUrl(reqs.get(d.params.requestId) || '')) stats.push((reqs.get(d.params.requestId) || '').split('?')[0].split('/').pop() + ' ' + d.params.errorText);
      if (d.params.blockedReason || /BLOCKED/.test(d.params.errorText || '')) blocked.push((reqs.get(d.params.requestId) || '').slice(0, 90));
    }
  };
  const call = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  await call('Network.enable', {});
  if (cold === 'cold') {
    await call('Network.clearBrowserCache');
    await call('Storage.clearDataForOrigin', { origin: 'https://www.youtube.com', storageTypes: 'cache_storage,service_workers' });
    await call('Network.setCacheDisabled', { cacheDisabled: false });
  }
  await call('Page.navigate', { url: 'about:blank' });
  await new Promise((r) => setTimeout(r, 700));
  bytes = 0; video = 0; count = 0; failed = 0; reqs.clear(); blocked.length = 0; stats.length = 0;
  const t0 = Date.now();
  await call('Page.navigate', { url });
  // content on screen: a drawn thumbnail of a video, or the watch page's title, in either page
  const probe = `(() => {
    const lite = !!document.querySelector('#movie_player .veil, #main .item, #main .note') && !document.querySelector('ytd-app');
    const sel = lite ? '#main .item img, #main .note, h1.vt' :
      'ytd-rich-item-renderer img.yt-core-image--loaded, ytd-video-renderer img.yt-core-image--loaded, yt-thumbnail-view-model img, h1.ytd-watch-metadata yt-formatted-string, ytd-feed-nudge-renderer, #contents ytd-grid-video-renderer img';
    const e = document.querySelector(sel);
    const ok = !!e && (e.tagName !== 'IMG' || (e.complete && e.naturalWidth > 0)) && (e.textContent !== undefined);
    return [ok, lite];
  })()`;
  let shown = -1, lite = false;
  while (Date.now() - t0 < 20000) {
    const r = await call('Runtime.evaluate', { expression: probe, returnByValue: true });
    const v = r && r.result && r.result.value;
    if (v && v[0]) { shown = Date.now() - t0; lite = v[1]; break; }
    await new Promise((r) => setTimeout(r, 50));
  }
  await new Promise((r) => setTimeout(r, Math.max(0, +secs * 1000 - (Date.now() - t0))));
  const out = { url, cold: cold === 'cold', page: lite ? 'ligera' : 'YouTube', content_ms: shown, mb: +(bytes / 1048576).toFixed(2),
                requests: count, failed, video_mb: +(video / 1048576).toFixed(2), blocked: blocked.slice(0, 3), stats };
  console.log(JSON.stringify(out));
  process.exit(0);
})();
