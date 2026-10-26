// node tools/clic.js <port> <part of the page's url> <css selector> [left|middle|right] [ctrl]
// A real (trusted) click on an element of a page of a test instance, through the DevTools protocol.
const [port, part, selector, button = 'left', mod] = process.argv.slice(2);
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
  const r = await call('Runtime.evaluate', { returnByValue: true, expression:
    `(() => { const e = document.querySelector(${JSON.stringify(selector)}); if (!e) return null; e.scrollIntoView({block:'center'}); const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2, e.href || '']; })()` });
  const v = r.result.value;
  if (!v) { console.error('no element', selector); process.exit(1); }
  const modifiers = mod === 'ctrl' ? 2 : 0;
  for (const type of ['mousePressed', 'mouseReleased'])
    await call('Input.dispatchMouseEvent', { type, x: v[0], y: v[1], button, buttons: button === 'middle' ? 4 : button === 'right' ? 2 : 1, clickCount: 1, modifiers });
  console.log('clic', button, 'en', v[2]);
  process.exit(0);
})();
