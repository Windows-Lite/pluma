// node tools/yt.js <port>
// What a YouTube watch page is doing: codec, resolution, buffer, and the page's own memory (JS heap, DOM nodes).
const [port] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const t = list.find((x) => x.type === 'page' && x.url.includes('youtube.com/watch'));
  if (!t) { console.error('no YouTube watch page'); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const call = (method, params) => new Promise((r) => {
    const i = ++id;
    const on = (m) => { const d = JSON.parse(m.data); if (d.id === i) { ws.removeEventListener('message', on); r(d.result); } };
    ws.addEventListener('message', on);
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  await call('Performance.enable', {});
  const m = await call('Performance.getMetrics', {});
  const get = (n) => (m.metrics.find((x) => x.name === n) || {}).value;
  const r = await call('Runtime.evaluate', { returnByValue: true, expression: `(() => {
    const p = document.getElementById('movie_player'), v = document.querySelector('video');
    let s = {}; try { s = p.getStatsForNerds(); } catch (e) {}
    const b = v && v.buffered.length ? v.buffered.end(v.buffered.length - 1) - v.currentTime : 0;
    return { codecs: s.codecs, resolucion: s.resolution, calidad: p && p.getPlaybackQuality && p.getPlaybackQuality(),
             tiempo: v && +v.currentTime.toFixed(1), pausado: v && v.paused, bufer_s: +b.toFixed(1),
             perdidos: v && v.getVideoPlaybackQuality().droppedVideoFrames, nodos: document.getElementsByTagName('*').length };
  })()` });
  console.log(JSON.stringify(Object.assign(r.result.value || {}, { heap_js_MB: Math.round(get('JSHeapUsedSize') / 1048576), heap_js_total_MB: Math.round(get('JSHeapTotalSize') / 1048576) })));
  process.exit(0);
})();
