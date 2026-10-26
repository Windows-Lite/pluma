// tools\descargas\servidor.js <port>: a page with three links that download as attachments (prueba.ps1)
const http = require('http');
const port = +process.argv[2] || 9531;
const pdf = Buffer.from("%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 300 200]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>endobj\n4 0 obj<</Length 44>>stream\nBT /F1 24 Tf 40 100 Td (Hola Pluma) Tj ET\nendstream endobj\n5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF");
const zip = Buffer.concat([Buffer.from([0x50, 0x4b, 0x05, 0x06]), Buffer.alloc(18)]);
const big = Buffer.alloc(30 * 1024 * 1024, 7);
http.createServer((q, r) => {
  const u = q.url.split('?')[0];
  if (u === '/prueba-192.pdf') { r.writeHead(200, { 'Content-Type': 'application/pdf', 'Content-Disposition': 'attachment; filename="prueba-192.pdf"', 'Content-Length': pdf.length }); return r.end(pdf); }
  if (u === '/prueba-192.zip') { r.writeHead(200, { 'Content-Type': 'application/zip', 'Content-Disposition': 'attachment; filename="prueba-192.zip"', 'Content-Length': zip.length }); return r.end(zip); }
  if (u === '/prueba-192.exe') {   // a program: the kind of file the engine may warn about
    const exe = Buffer.concat([Buffer.from('MZ'), Buffer.alloc(4094)]);
    r.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Disposition': 'attachment; filename="prueba-192.exe"', 'Content-Length': exe.length }); return r.end(exe);
  }
  if (u === '/lento-192.bin') {   // 30 MB slowly (~3 MB/s): progress and cancel
    r.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Disposition': 'attachment; filename="lento-192.bin"', 'Content-Length': big.length });
    let o = 0; const t = setInterval(() => { if (o >= big.length) { clearInterval(t); return r.end(); } r.write(big.subarray(o, o + 300000)); o += 300000; }, 100);
    q.on('close', () => clearInterval(t)); return;
  }
  r.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  r.end('<!doctype html><title>Descargas</title><h1>Descargas</h1><p><a href="/prueba-192.pdf">PDF</a> <a href="/prueba-192.zip">ZIP</a> <a href="/lento-192.bin">lento</a> <a href="/prueba-192.exe">EXE</a>');
}).listen(port, '127.0.0.1');
