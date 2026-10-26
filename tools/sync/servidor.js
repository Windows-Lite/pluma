// tools\sync\servidor.js <port> <marker>: a site to sign in to (tools\sync\prueba.ps1). /entrar sets a persistent
// HttpOnly cookie and the page then keeps the marker in localStorage and IndexedDB; / says whether all three are there;
// /?grande=<MB> puts that much noise in Cache Storage (a profile as heavy as a real one).
const http = require('http');
const port = +process.argv[2] || 8213;
const M = process.argv[3] || 'x';
const cookie = 't213_' + M;
const page = (signed) => `<!doctype html><meta charset="utf-8"><title>Prueba de copia</title>
<style>body{font:16px system-ui;background:#f4f5f7;margin:0;display:grid;place-items:center;height:100vh}
.c{background:#fff;border-radius:14px;padding:28px 36px;box-shadow:0 2px 12px #0002;min-width:360px}
h1{margin:0 0 14px;font-size:24px}.ok{color:#178a3a}.no{color:#c62828}li{margin:6px 0;list-style:none}</style>
<div class="c"><h1 id="t">...</h1><ul><li id="c"></li><li id="l"></li><li id="i"></li></ul></div>
<script>
const M = ${JSON.stringify(M)}, SIGNED = ${signed};
const idb = (mode, f) => new Promise((res, rej) => {
  const o = indexedDB.open('t213db', 1);
  o.onupgradeneeded = () => o.result.createObjectStore('kv');
  o.onerror = () => rej(o.error);
  o.onsuccess = () => { const tx = o.result.transaction('kv', mode); const r = f(tx.objectStore('kv')); tx.oncomplete = () => res(r && r.result); tx.onerror = () => rej(tx.error); };
});
(async () => {
  const big = +new URLSearchParams(location.search).get('grande') || 0;   // MB of noise in Cache Storage (a heavy profile)
  if (big) {   // in 10 MB entries, as a real profile's (its biggest file was 20 MB)
    const c = await caches.open('t213');
    for (let i = 0; i * 10 < big; i++) {
      const b = new Uint8Array(Math.min(10, big - i * 10) << 20);
      for (let o = 0; o < b.length; o += 65536) crypto.getRandomValues(b.subarray(o, o + 65536));
      await c.put('/grande' + i, new Response(b));
    }
    window.__grande = true;
  }
  if (location.search.includes('nuevo=1')) {
    localStorage.setItem('t213', 'ls' + M);
    await idb('readwrite', (s) => s.put('idb' + M, 'k'));
    history.replaceState(null, '', '/');
  }
  const ls = localStorage.getItem('t213') === 'ls' + M;
  const v = await idb('readonly', (s) => s.get('k')).catch(() => null);
  const ix = v === 'idb' + M;
  const row = (id, ok, what) => { const e = document.getElementById(id); e.className = ok ? 'ok' : 'no'; e.textContent = (ok ? '✔ ' : '✘ ') + what; };
  row('c', SIGNED, 'Cookie de sesión'); row('l', ls, 'localStorage'); row('i', ix, 'IndexedDB');
  const all = SIGNED && ls && ix;
  const t = document.getElementById('t'); t.textContent = all ? 'Sesión iniciada' : 'Sin sesión'; t.className = all ? 'ok' : 'no';
  window.__estado = { cookie: SIGNED, ls, idb: ix };
})();
</script>`;
http.createServer((q, r) => {
  const u = q.url.split('?')[0];
  if (u === '/entrar') {
    r.writeHead(302, { 'Set-Cookie': `${cookie}=c${M}; Max-Age=31536000; Path=/; HttpOnly; SameSite=Lax`, Location: '/?nuevo=1', 'Cache-Control': 'no-store' });
    return r.end();
  }
  if (u !== '/') { r.writeHead(404); return r.end(); }
  const signed = (q.headers.cookie || '').split(/;\s*/).includes(`${cookie}=c${M}`);
  r.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
  r.end(page(signed));
}).listen(port, '127.0.0.1');
