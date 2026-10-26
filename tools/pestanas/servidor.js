// Pages for tools\pestanas\prueba.ps1: node servidor.js <port>
//   /form   a textarea (what the person types must survive the tab being hidden), a counter (timers go on after
//           being frozen) and the page's freeze/resume events
//   /plain  nothing to lose (it may be let go)
//   /ws     a WebSocket that the server pings every second (does it reconnect after being frozen?)
//   /llamada  the camera and the microphone in use (fake devices: a call, neither frozen nor let go)
const http = require('http');
const port = +process.argv[2] || 9551;
const page = (title, body) => `<!doctype html><meta charset="utf-8"><title>${title}</title>
<style>body{font:16px system-ui;margin:24px;background:#fff;color:#111}textarea{width:90%;height:120px;font:16px system-ui}</style>
<h1>${title}</h1><p>Cargada a las <b id="t"></b>; contador: <b id="n">0</b></p>${body}
<script>
  document.getElementById('t').textContent = new Date().toLocaleTimeString();
  window.ticks = 0; window.events = [];
  setInterval(() => { document.getElementById('n').textContent = ++window.ticks; }, 1000);
  for (const e of ['freeze', 'resume', 'visibilitychange', 'pageshow']) document.addEventListener(e, () => window.events.push(e + '@' + window.ticks));
</script>`;
const pages = {
  '/form': page('Formulario', '<p>Escribe algo y vete a otra pestaña:</p><textarea id="txt"></textarea>'),
  '/plain': page('Sin nada que perder', '<p>Una página normal.</p>'),
  '/llamada': page('Llamada', `<p id="s">conectando</p><script>
    navigator.mediaDevices.getUserMedia({ audio: true, video: true }).then((st) => {
      window.stream = st;
      setInterval(() => { document.getElementById('s').textContent = 'pistas vivas: ' + st.getTracks().filter((x) => x.readyState === 'live').length; }, 500);
    }, (e) => { document.getElementById('s').textContent = 'error: ' + e.name; });</script>`),
  '/ws': page('WebSocket', `<p id="s">conectando</p><textarea id="txt"></textarea><script>
    window.got = 0; window.opens = 0; window.closes = 0;
    const go = () => {
      const ws = new WebSocket('ws://' + location.host + '/socket');
      ws.onopen = () => window.opens++;
      ws.onmessage = () => { window.got++; document.getElementById('s').textContent = 'mensajes: ' + window.got + ', conexiones: ' + window.opens; };
      ws.onclose = () => { window.closes++; setTimeout(go, 1000); };
    };
    go();</script>`),
};
const srv = http.createServer((q, r) => {
  const p = pages[q.url.split('?')[0]];
  r.writeHead(p ? 200 : 404, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
  r.end(p || 'no');
}).listen(port, '127.0.0.1');
// the WebSocket: the handshake by hand (no packages), then a text frame "ping" every second
srv.on('upgrade', (q, sock) => {
  const key = require('crypto').createHash('sha1').update(q.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + key + '\r\n\r\n');
  const t = setInterval(() => sock.write(Buffer.from([0x81, 4, 0x70, 0x69, 0x6e, 0x67])), 1000);
  sock.on('close', () => clearInterval(t)); sock.on('error', () => clearInterval(t));
});
