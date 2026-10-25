// node tools/clics.js <port> <part of the page's url> <clicks> [every ms]
// How fast a click is answered: a button is put on the page and clicked with real input events; for each click, the
// time until the page's handler runs and until the next frame shows the change (milliseconds).
const [port, part, count = '60', every = '230'] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const t = list.find((x) => x.type === 'page' && x.url.includes(part));
  if (!t) { console.error('no page with', part); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const call = (method, params) => new Promise((r) => {
    const i = ++id;
    const on = (m) => { const d = JSON.parse(m.data); if (d.id === i) { ws.removeEventListener('message', on); r(d.result); } };
    ws.addEventListener('message', on);
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  await call('Runtime.evaluate', { expression: `(() => {
    window.__lat = [];
    const b = document.createElement('button');
    b.style.cssText = 'position:fixed;left:20px;top:20px;width:220px;height:90px;z-index:2147483647;background:#2f6fed';
    document.documentElement.appendChild(b);
    b.addEventListener('pointerdown', (e) => {
      const t0 = e.timeStamp, th = performance.now();
      b.style.background = b.style.background.includes('47') ? '#e5484d' : '#2f6fed';
      requestAnimationFrame(() => requestAnimationFrame((t2) => window.__lat.push([th - t0, t2 - t0])));
    });
  })()` });
  for (let k = 0; k < Number(count); k++) {
    await call('Input.dispatchMouseEvent', { type: 'mousePressed', x: 120, y: 60, button: 'left', buttons: 1, clickCount: 1 });
    await call('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 120, y: 60, button: 'left', buttons: 0, clickCount: 1 });
    await new Promise((r) => setTimeout(r, Number(every)));
  }
  const r = await call('Runtime.evaluate', { expression: 'window.__lat', returnByValue: true });
  const lat = r.result.value;
  const stats = (k) => {
    const v = lat.map((x) => x[k]).sort((a, b) => a - b);
    return { mediana: +v[Math.floor(v.length / 2)].toFixed(1), p95: +v[Math.floor(v.length * 0.95)].toFixed(1), peor: +v[v.length - 1].toFixed(1) };
  };
  console.log(JSON.stringify({ clics: lat.length, hasta_la_pagina: stats(0), hasta_verlo: stats(1), de_mas_de_100ms: lat.filter((x) => x[1] > 100).length }));
  process.exit(0);
})();
