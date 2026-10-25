// node tools/cdp.js <port> <part of the target's url> <expression>
// Evaluates an expression in a page of a test instance (PLUMA_DEPURAR=<port>) and prints the result as JSON.
const [port, part, expr] = process.argv.slice(2);
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const ok = (x) => x.webSocketDebuggerUrl && (part.startsWith('http') ? x.url.startsWith(part) : x.url.includes(part));
  const t = list.find((x) => ok(x) && x.type === 'page') || list.find(ok);
  if (!t) { console.error('no target with', part, '\n', list.map((x) => x.type + ' ' + x.url).join('\n')); process.exit(1); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id !== 1) return;
    const r = d.result && d.result.result;
    console.log(JSON.stringify(d.result && d.result.exceptionDetails ? d.result.exceptionDetails : r && r.value, null, 1));
    ws.close();
    process.exit(0);
  };
  ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression: expr, returnByValue: true, awaitPromise: true } }));
  setTimeout(() => { console.error('timeout'); process.exit(2); }, 15000);
})();
