// tools\sync\nube-falsa.js <port> <folder>: Google Drive, OneDrive and Dropbox as Pluma's backup sees them, on
// 127.0.0.1 (tools\sync\nube.ps1; Pluma test instances with PLUMA_SYNC_NUBE_PRUEBA=http://127.0.0.1:<port> send every
// "https://host/path" here as "/host/path"). Sign-in, token, account, list, delete, and the big uploads in pieces,
// with each provider's rules (Drive: pieces of a multiple of 256 KiB, 308 + Range, "bytes */total"; OneDrive:
// multiples of 320 KiB, 202 + nextExpectedRanges, GET of the session; Dropbox: offsets, incorrect_offset).
// The second piece of every upload is dropped once: Drive and OneDrive keep half of it and cut the connection,
// Dropbox keeps all of it and cuts before answering. GET /estado says what happened. Uploaded files go to <folder>.
const http = require('http');
const fs = require('fs');
const path = require('path');
const port = +process.argv[2] || 8215;
const dir = process.argv[3] || '.';
const base = `http://127.0.0.1:${port}`;
const st = { gdrive: { pieces: 0, drops: 0, asks: 0, done: [] }, onedrive: { pieces: 0, drops: 0, asks: 0, done: [] }, dropbox: { pieces: 0, drops: 0, asks: 0, done: [] }, errors: [] };
const sessions = {};   // id -> { prov, name, total, got: Buffer[], len, dropped, n }
const files = { gdrive: [], onedrive: [], dropbox: [] };   // { id, name }
let seq = 0;
for (const p of Object.keys(files)) fs.mkdirSync(path.join(dir, p), { recursive: true });

const json = (r, code, o, h = {}) => { r.writeHead(code, Object.assign({ 'Content-Type': 'application/json' }, h)); r.end(o === undefined ? '' : JSON.stringify(o)); };
const err = (r, code, why) => { st.errors.push(why); json(r, code, { error: why }); };
const body = (q) => new Promise((ok) => { const b = []; q.on('data', (c) => b.push(c)); q.on('end', () => ok(Buffer.concat(b))); });
const save = (prov, name, buf) => {
  const id = 'f' + (++seq);
  fs.writeFileSync(path.join(dir, prov, name), buf);
  files[prov] = files[prov].filter((f) => f.name !== name);
  files[prov].push({ id, name });
  st[prov].done.push({ name, bytes: buf.length });
  return id;
};
const range = (h) => { const m = /bytes (\d+)-(\d+)\/(\d+)/.exec(h || ''); return m ? { a: +m[1], b: +m[2], total: +m[3] } : null; };
// the second piece, the first time: keep part of it and cut the connection without an answer
function drop(q, s, buf, keep) {
  s.dropped = true; st[s.prov].drops++;
  if (keep) { s.got.push(buf.subarray(0, keep)); s.len += keep; }
  q.socket.destroy();
}

async function handle(q, r) {
  const u = new URL(q.url, base);
  const p = u.pathname;
  const buf = await body(q);
  if (p === '/estado') return json(r, 200, st);
  // ---- sign-in: the "browser" (Pluma's GET in tests) goes straight back to the loopback with a code
  if (p.endsWith('/o/oauth2/v2/auth') || p.endsWith('/oauth2/v2.0/authorize') || p.endsWith('/oauth2/authorize')) {
    const back = u.searchParams.get('redirect_uri') + '?code=c' + (++seq) + '&state=' + encodeURIComponent(u.searchParams.get('state'));
    r.writeHead(302, { Location: back }); return r.end();
  }
  if (p.endsWith('/token')) return json(r, 200, { access_token: 'at', refresh_token: 'rt', expires_in: 3600 });
  if ((q.headers.authorization || '') !== 'Bearer at' && !p.startsWith('/sesion/')) return err(r, 401, 'sin token en ' + p);
  if (p === '/www.googleapis.com/drive/v3/about') return json(r, 200, { user: { emailAddress: 'prueba@pluma.test' } });
  if (p === '/graph.microsoft.com/v1.0/me') return json(r, 200, { userPrincipalName: 'prueba@pluma.test' });
  if (p === '/api.dropboxapi.com/2/users/get_current_account') return json(r, 200, { email: 'prueba@pluma.test' });

  // ---- Google Drive
  if (p === '/www.googleapis.com/upload/drive/v3/files' && u.searchParams.get('uploadType') === 'resumable') {
    const meta = JSON.parse(buf.toString() || '{}');
    const total = +q.headers['x-upload-content-length'];
    if (!meta.name || !(meta.parents || []).includes('appDataFolder') || !total) return err(r, 400, 'drive: sesion sin nombre, carpeta o tamano');
    const id = 'g' + (++seq);
    sessions[id] = { prov: 'gdrive', name: meta.name, total, got: [], len: 0, dropped: false, n: 0 };
    r.writeHead(200, { Location: `${base}/sesion/${id}` }); return r.end();
  }
  if (p === '/www.googleapis.com/drive/v3/files' && q.method === 'GET') return json(r, 200, { files: files.gdrive });
  if (p.startsWith('/www.googleapis.com/drive/v3/files/') && q.method === 'DELETE') {
    files.gdrive = files.gdrive.filter((f) => f.id !== p.split('/').pop()); r.writeHead(204); return r.end();
  }
  // ---- OneDrive
  let m = /^\/graph\.microsoft\.com\/v1\.0\/me\/drive\/special\/approot:\/(.+):\/(createUploadSession|content)$/.exec(p);
  if (m) {
    const name = decodeURIComponent(m[1]);
    if (m[2] === 'content') { if (buf.length > 4 << 20) return err(r, 413, 'onedrive: subida simple de mas de 4 MB'); save('onedrive', name, buf); return json(r, 201, { id: 'x' }); }
    const id = 'o' + (++seq);
    sessions[id] = { prov: 'onedrive', name, total: 0, got: [], len: 0, dropped: false, n: 0 };
    return json(r, 200, { uploadUrl: `${base}/sesion/${id}` });
  }
  if (p === '/graph.microsoft.com/v1.0/me/drive/special/approot/children') return json(r, 200, { value: files.onedrive });
  if (p.startsWith('/graph.microsoft.com/v1.0/me/drive/items/') && q.method === 'DELETE') {
    files.onedrive = files.onedrive.filter((f) => f.id !== p.split('/').pop()); r.writeHead(204); return r.end();
  }
  // ---- Dropbox
  if (p.startsWith('/content.dropboxapi.com/2/files/')) {
    const arg = JSON.parse(q.headers['dropbox-api-arg'] || '{}');
    const what = p.slice('/content.dropboxapi.com/2/files/'.length);
    if (what === 'upload') { if (buf.length > 150 << 20) return err(r, 413, 'dropbox: subida simple de mas de 150 MB'); save('dropbox', arg.path.slice(1), buf); return json(r, 200, { name: arg.path.slice(1) }); }
    if (what === 'upload_session/start') {
      const id = 'd' + (++seq);
      sessions[id] = { prov: 'dropbox', got: [buf], len: buf.length, dropped: false, n: buf.length ? 1 : 0 };
      if (buf.length) st.dropbox.pieces++;
      return json(r, 200, { session_id: id });
    }
    const c = arg.cursor || {}, s = sessions[c.session_id];
    if (!s) return err(r, 404, 'dropbox: sesion desconocida');
    if (c.offset !== s.len) {
      st.dropbox.asks++;
      const e = { '.tag': 'incorrect_offset', correct_offset: s.len };
      return json(r, 409, { error_summary: 'incorrect_offset/', error: what === 'upload_session/finish' ? { '.tag': 'lookup_failed', lookup_failed: e } : e });
    }
    s.n++; st.dropbox.pieces++;
    if (s.n === 2 && !s.dropped) { s.got.push(buf); s.len += buf.length; s.dropped = true; st.dropbox.drops++; return q.socket.destroy(); }
    s.got.push(buf); s.len += buf.length;
    if (what === 'upload_session/append_v2') return json(r, 200, null);
    if (what === 'upload_session/finish') { save('dropbox', arg.commit.path.slice(1), Buffer.concat(s.got)); delete sessions[c.session_id]; return json(r, 200, { name: arg.commit.path.slice(1) }); }
    return err(r, 400, 'dropbox: ' + what);
  }
  if (p === '/api.dropboxapi.com/2/files/list_folder') return json(r, 200, { entries: files.dropbox.map((f) => ({ path_lower: '/' + f.name.toLowerCase(), name: f.name })), has_more: false });
  if (p === '/api.dropboxapi.com/2/files/delete_v2') {
    const a = JSON.parse(buf.toString()); files.dropbox = files.dropbox.filter((f) => '/' + f.name.toLowerCase() !== a.path); return json(r, 200, {});
  }

  // ---- the upload sessions (Drive and OneDrive)
  if (p.startsWith('/sesion/')) {
    const s = sessions[p.slice(8)];
    if (!s) return err(r, 404, 'sesion desconocida');
    const g = st[s.prov];
    if (s.prov === 'gdrive') {
      if ((q.headers.authorization || '') !== 'Bearer at') return err(r, 401, 'drive: pieza sin token');
      const cr = q.headers['content-range'] || '';
      const has = () => (s.len ? { Range: `bytes=0-${s.len - 1}` } : {});
      if (/^bytes \*\/\d+$/.test(cr)) {   // how much do you have?
        g.asks++;
        r.writeHead(308, has()); return r.end();
      }
      const rg = range(cr);
      if (!rg || rg.total !== s.total || rg.b - rg.a + 1 !== buf.length) return err(r, 400, 'drive: Content-Range mal: ' + cr);
      if (rg.a !== s.len) return err(r, 400, `drive: empieza en ${rg.a} y tengo ${s.len}`);
      const last = rg.b + 1 === s.total;
      if (!last && buf.length % (256 * 1024)) return err(r, 400, 'drive: pieza que no es multiplo de 256 KiB');
      s.n++; g.pieces++;
      if (s.n === 2 && !s.dropped) return drop(q, s, buf, Math.floor(buf.length / 2));
      s.got.push(buf); s.len += buf.length;
      if (last) { const id = save('gdrive', s.name, Buffer.concat(s.got)); delete sessions[p.slice(8)]; return json(r, 200, { id }); }
      r.writeHead(308, has()); return r.end();
    }
    // OneDrive: its upload URL takes no token
    if (q.headers.authorization) return err(r, 400, 'onedrive: token en la URL de la sesion');
    if (q.method === 'GET') { g.asks++; return json(r, 200, { nextExpectedRanges: [`${s.len}-`] }); }
    const rg = range(q.headers['content-range']);
    if (!rg || rg.b - rg.a + 1 !== buf.length) return err(r, 400, 'onedrive: Content-Range mal');
    s.total = rg.total;
    if (rg.a !== s.len) return json(r, 416, { error: { code: 'invalidRange' } });
    const last = rg.b + 1 === rg.total;
    if (!last && buf.length % (320 * 1024)) return err(r, 400, 'onedrive: pieza que no es multiplo de 320 KiB');
    s.n++; g.pieces++;
    if (s.n === 2 && !s.dropped) return drop(q, s, buf, Math.floor(buf.length / 2));
    s.got.push(buf); s.len += buf.length;
    if (last) { const id = save('onedrive', s.name, Buffer.concat(s.got)); delete sessions[p.slice(8)]; return json(r, 201, { id }); }
    return json(r, 202, { nextExpectedRanges: [`${s.len}-`] });
  }
  err(r, 404, 'no se: ' + q.method + ' ' + p);
}
http.createServer((q, r) => handle(q, r).catch((e) => { try { err(r, 500, 'excepcion: ' + e.message); } catch {} })).listen(port, '127.0.0.1');
