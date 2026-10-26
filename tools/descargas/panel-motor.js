// node tools\descargas\panel-motor.js <port> [regex]: the engine's downloads flyout (edge://downloads-hub, a page of
// its own): prints its buttons and items and, with a regex, gives the first matching button a real click.
// Never "Open file" from here: that button is what took the engine down (downloads.cpp).
const [port, re] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const t = list.find((x) => x.url.startsWith('edge://downloads-hub'));
  if (!t) { console.log('no engine flyout'); process.exit(0); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0;
  const call = (method, params) => new Promise((r) => {
    const i = ++id;
    const on = (m) => { const d = JSON.parse(m.data); if (d.id === i) { ws.removeEventListener('message', on); r(d.result); } };
    ws.addEventListener('message', on);
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  const expr = `(() => { const out = [], items = []; const walk = (n) => { for (const e of n.querySelectorAll('*')) {
      if (e.tagName === 'DOWNLOAD-ITEM') items.push((e.shadowRoot ? e.shadowRoot.textContent : e.textContent).replace(/\\s+/g, ' ').trim().slice(0, 200));
      const lab = (e.getAttribute('aria-label') || e.getAttribute('title') || (e.matches('button,[role=button],[role=menuitem]') ? e.textContent.trim() : '') || '').slice(0, 80);
      if (lab && e.matches('button,[role=button],[role=menuitem],mai-button')) { const b = e.getBoundingClientRect(); if (b.width > 0) out.push([lab, b.x + b.width / 2, b.y + b.height / 2]); }
      if (e.shadowRoot) walk(e.shadowRoot); } }; walk(document); return { out, items }; })()`;
  const r = (await call('Runtime.evaluate', { returnByValue: true, expression: expr })).result.value;
  console.log('items:', JSON.stringify(r.items));
  console.log('buttons:', r.out.map((x) => x[0]).join(' | '));
  if (re) {
    const it = r.out.find((x) => new RegExp(re, 'i').test(x[0]) && !/open file|abrir archivo/i.test(x[0]));
    if (!it) { console.log('no button', re); process.exit(0); }
    for (const type of ['mousePressed', 'mouseReleased'])
      await call('Input.dispatchMouseEvent', { type, x: it[1], y: it[2], button: 'left', buttons: 1, clickCount: 1 });
    console.log('clic en', it[0]);
  }
  process.exit(0);
})();
