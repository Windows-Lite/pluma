// node escribir.js <cdp port> <part of the page's url> <selector> <text>: types into that field as a person would
// (CDP Input.insertText: a trusted input event, which is what Pluma looks at; setting .value would not count)
const [port, part, sel, text] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const t = list.find((x) => x.type === 'page' && x.url.includes(part));
  if (!t) { console.error('no page with', part); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let n = 0; const wait = {};
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (wait[d.id]) wait[d.id](d); };
  const call = (method, params) => new Promise((r) => { const id = ++n; wait[id] = r; ws.send(JSON.stringify({ id, method, params })); });
  await call('Runtime.evaluate', { expression: `document.querySelector(${JSON.stringify(sel)}).focus()` });
  await call('Input.insertText', { text });
  const v = await call('Runtime.evaluate', { expression: `document.querySelector(${JSON.stringify(sel)}).value`, returnByValue: true });
  console.log(JSON.stringify(v.result.result.value));
  ws.close(); process.exit(0);
})();
setTimeout(() => { console.error('timeout'); process.exit(2); }, 15000);
