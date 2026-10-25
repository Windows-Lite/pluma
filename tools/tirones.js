// node tools/tirones.js <port> <part of the page's url> <seconds>
// Scrolls the page smoothly, one step per frame, and reports how long each frame took: the frames over 25 ms
// (more than one and a half frames at 60 Hz) are what a person sees as a stutter.
const [port, part, secs = '20'] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const t = list.find((x) => x.type === 'page' && x.url.includes(part));
  if (!t) { console.error('no page with', part); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id !== 1) return;
    const v = d.result && d.result.result && d.result.result.value;
    console.log(JSON.stringify(v));
    process.exit(0);
  };
  const expr = `new Promise((done) => {
    const deltas = []; let last = performance.now(), dir = 1; const end = last + ${Number(secs) * 1000};
    const step = (now) => {
      deltas.push(now - last); last = now;
      if (scrollY + innerHeight >= document.body.scrollHeight - 10) dir = -1; else if (scrollY <= 0) dir = 1;
      scrollBy(0, 6 * dir);
      if (now < end) requestAnimationFrame(step);
      else {
        deltas.shift();
        const s = [...deltas].sort((a, b) => a - b);
        done({ frames: deltas.length, mediana: +s[Math.floor(s.length / 2)].toFixed(1), peor: +s[s.length - 1].toFixed(1),
               de_mas_de_25ms: deltas.filter((x) => x > 25).length, de_mas_de_50ms: deltas.filter((x) => x > 50).length });
      }
    };
    requestAnimationFrame(step);
  })`;
  ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression: expr, awaitPromise: true, returnByValue: true } }));
})();
