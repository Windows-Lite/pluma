# Test pages for the strong password suggestion (tools/passgen-e2e.js): python serve.py <port>, on 127.0.0.1 only.
# A form sent with POST gets a "done" page (http.server alone answers 501), as a real sign-up does.
import http.server, sys, os

class H(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length') or 0))
        body = b'<!doctype html><meta charset="utf-8"><title>Hecho</title><p id="done">Cuenta creada</p><a href="login.html">Entrar</a>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def end_headers(self):
        # csp.html: as strict as accounts.google.com (Trusted Types, no inline styles): the suggestion must still work
        if self.path.startswith('/csp.html'):
            self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self'; require-trusted-types-for 'script'; trusted-types 'none'")
        # noref.html: a page whose same-origin POSTs carry "Origin: null" (Pluma has to know them by Sec-Fetch-Site)
        if self.path.startswith('/noref.html'):
            self.send_header('Referrer-Policy', 'no-referrer')
        super().end_headers()
    def log_message(self, *a):
        pass

os.chdir(os.path.dirname(os.path.abspath(__file__)))
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
