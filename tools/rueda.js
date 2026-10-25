// node tools/rueda.js <port> <part of the page's url> <steps> [delta] [every ms]
// Scrolls a page of a test instance with real mouse-wheel events (through the same input path as a real mouse,
// so the page's scrollbars and the compositor do what they do for a person), down and then back up.
const [port, part, steps = '30', delta = '120', every = '40'] = process.argv.slice(2);
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
  const n = Number(steps);
  for (let k = 0; k < n * 2; k++) {
    const dy = k < n ? Number(delta) : -Number(delta);
    await call('Input.dispatchMouseEvent', { type: 'mouseWheel', x: 600, y: 450, deltaX: 0, deltaY: dy });
    await new Promise((r) => setTimeout(r, Number(every)));
  }
  console.log('hecho');
  process.exit(0);
})();
