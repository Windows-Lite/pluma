// node tools/peticiones.js <port> <part of the page's url> <seconds>
// Reloads a page of a test instance and lists the requests its video player makes to googlevideo.com: method, the
// format (itag), whether they are plain range downloads or YouTube's own streaming protocol (sabr/ump).
const [port, part, secs = '12'] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const t = list.find((x) => x.type === 'page' && x.url.includes(part));
  if (!t) { console.error('no page with', part); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const seen = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.method !== 'Network.requestWillBeSent') return;
    const r = d.params.request;
    if (!/googlevideo\.com\/videoplayback/.test(r.url)) return;
    const u = new URL(r.url);
    const q = Object.fromEntries(u.searchParams);
    seen.push({ metodo: r.method, itag: q.itag, mime: q.mime, range: q.range || '', rn: q.rn || '', sabr: q.sabr || '', ump: q.ump || '',
                n: (q.n || '').length, pot: !!q.pot, cuerpo: r.hasPostData ? 'sí' : 'no', largo_url: r.url.length });
  };
  const send = (method, params) => ws.send(JSON.stringify({ id: ++id, method, params }));
  send('Network.enable', {});
  send('Page.reload', { ignoreCache: false });
  await new Promise((r) => setTimeout(r, Number(secs) * 1000));
  console.log(JSON.stringify(seen.slice(0, 12), null, 0));
  console.log('total', seen.length);
  process.exit(0);
})();
