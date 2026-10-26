// End to end test of "Usar contraseña segura" in a test instance (tools\prueba.ps1, PLUMA_DEPURAR=<port>), with the
// pages of tools\passgen served by tools\passgen\serve.py:
//   node tools\passgen-e2e.js <devtools port> <pages port> fill|typed|forge|gen|after <captura.exe> <window class> <out folder>
// fill: the suggestion on the sign-up form (and not on the login form), keyboard, Esc, a React-like form, maxlength,
//       the setting switched off and on without reloading, and the sign-up form sent.
// typed: a login typed by hand and sent (its prompt is left to the person). gen: a suggested password sent, nothing else.
// forge: a page that reports a password Pluma did not make (and a frame of another site that tries too).
// after (the instance started again): the login form offers the saved password.
const [port, pages, phase, captura, cls, out] = process.argv.slice(2);
const { execFileSync } = require('child_process');
const base = `http://127.0.0.1:${pages}/`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let bad = 0;
const check = (name, ok, extra) => { console.log((ok ? 'ok    ' : 'FALLA ') + name + (extra !== undefined ? ': ' + JSON.stringify(extra) : '')); if (!ok) bad++; };
const shot = (name) => { if (captura && cls) execFileSync(captura, [cls, `${out}\\${name}.png`]); };

async function connect(match) {
  for (let i = 0; i < 60; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const t = list.find((x) => x.type === 'page' && match(x.url));
      if (t) {
        const ws = new WebSocket(t.webSocketDebuggerUrl);
        await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
        let id = 0;
        const call = (method, params = {}) => new Promise((r) => {
          const i = ++id;
          const on = (m) => { const d = JSON.parse(m.data); if (d.id === i) { ws.removeEventListener('message', on); r(d.result || d); } };
          ws.addEventListener('message', on);
          ws.send(JSON.stringify({ id: i, method, params }));
        });
        const ev = async (expression) => { const r = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true }); return r.result && r.result.value; };
        return { ws, call, ev };
      }
    } catch (e) {}
    await sleep(500);
  }
  throw new Error('no page');
}
async function go(p, path) {
  await p.call('Page.navigate', { url: base + path });
  for (let i = 0; i < 40; i++) { await sleep(150); if (await p.ev(`location.href === ${JSON.stringify(base + path)} && document.readyState === 'complete'`)) break; }
  await sleep(300);
}
async function click(p, sel, dx, dy) {
  const v = await p.ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; const b = e.getBoundingClientRect(); return [b.x, b.y, b.width, b.height]; })()`);
  if (!v) throw new Error('no element ' + sel);
  const x = dx === undefined ? v[0] + v[2] / 2 : v[0] + dx, y = dy === undefined ? v[1] + v[3] / 2 : v[1] + dy;
  for (const type of ['mousePressed', 'mouseReleased'])
    await p.call('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: 1, clickCount: 1 });
  await sleep(250);
}
async function key(p, k, code, vk) {
  for (const type of ['rawKeyDown', 'keyUp']) await p.call('Input.dispatchKeyEvent', { type, key: k, code, windowsVirtualKeyCode: vk });
  await sleep(200);
}
const shownQ = `!!document.querySelector('pluma-passgen')`;
const val = (sel) => `document.querySelector(${JSON.stringify(sel)}).value`;

(async () => {
  const p = await connect((u) => u.startsWith('http'));
  await p.call('Page.enable');
  await p.call('Emulation.setFocusEmulationEnabled', { enabled: true });   // (the test window is off the screen, never focused)
  if (phase === 'fill') {
    // a login form: never
    await go(p, 'login.html');
    await click(p, '#pw');
    check('formulario de entrar: no sale', !(await p.ev(shownQ)));
    // the sign-up form: on focus, under the field
    await go(p, 'signup.html');
    await click(p, '#pw');
    check('formulario de registro: sale al enfocar la contraseña', await p.ev(shownQ));
    const geo = await p.ev(`(() => { const h = document.querySelector('pluma-passgen').getBoundingClientRect(), f = document.querySelector('#pw').getBoundingClientRect(); return { below: h.top >= f.bottom, gap: h.top - f.bottom, left: h.left, w: h.width, h: h.height }; })()`);
    check('debajo del campo', geo.below && geo.gap < 12, geo);
    shot('sugerencia');
    // Esc hides it; a click on the field brings it back
    await key(p, 'Escape', 'Escape', 27);
    check('Esc la esconde', !(await p.ev(shownQ)));
    await click(p, '#pw');
    check('un clic en el campo la vuelve a sacar', await p.ev(shownQ));
    // the keyboard: Enter alone does nothing to it; Down + Enter fills
    await key(p, 'ArrowDown', 'ArrowDown', 40);
    shot('sugerencia-teclado');
    await key(p, 'Enter', 'Enter', 13);
    const a = await p.ev(`[${val('#pw')}, ${val('#pw2')}, location.pathname]`);
    check('Abajo + Intro rellena la contraseña y la de confirmar', a[0].length === 15 && a[0] === a[1] && a[2].endsWith('signup.html'), a);
    check('sin esconder nada en la página', await p.ev(`!document.querySelector('pluma-passgen')`));
    await p.ev(`document.activeElement.blur()`);
    await sleep(200);
    shot('rellenado');
    // a click on it, on a fresh form; and the form sent (the engine saves the password then)
    await go(p, 'signup.html');
    await click(p, '#pw');
    const h = await p.ev(`(() => { const b = document.querySelector('pluma-passgen').getBoundingClientRect(); return [b.x, b.y]; })()`);
    for (const type of ['mousePressed', 'mouseReleased']) await p.call('Input.dispatchMouseEvent', { type, x: h[0] + 40, y: h[1] + 22, button: 'left', buttons: 1, clickCount: 1 });
    await sleep(300);
    const b = await p.ev(`[${val('#pw')}, ${val('#pw2')}, document.activeElement && document.activeElement.id]`);
    check('un clic en la sugerencia rellena las dos', b[0].length === 15 && b[0] === b[1], b);
    check('el campo sigue con el foco', b[2] === 'pw', b[2]);
    // React and the like see it
    await go(p, 'react.html');
    await click(p, '#pw');
    check('formulario como React: sale', await p.ev(shownQ));
    await key(p, 'ArrowDown', 'ArrowDown', 40);
    await key(p, 'Enter', 'Enter', 13);
    const r = await p.ev(`document.getElementById('state').textContent`);
    check('React ve las dos contraseñas', r === 'estado: pw=15 pw2=15 iguales=true', r);
    // a page as strict as accounts.google.com (CSP without inline styles, Trusted Types): nothing of ours is blocked
    await go(p, 'csp.html');
    await p.ev(`(window.__v = [], document.addEventListener('securitypolicyviolation', (e) => window.__v.push(e.violatedDirective + ' ' + e.blockedURI)), 1)`);
    await click(p, '#pw');
    const c = await p.ev(`(() => { const h = document.querySelector('pluma-passgen'); return { shown: !!h, position: h && getComputedStyle(h).position, height: h && h.getBoundingClientRect().height, violations: window.__v }; })()`);
    check('CSP estricta y Trusted Types: sale, con sus estilos y sin violaciones', c.shown && c.position === 'fixed' && c.height > 40 && !c.violations.length, c);
    await key(p, 'ArrowDown', 'ArrowDown', 40);
    await key(p, 'Enter', 'Enter', 13);
    check('CSP estricta: rellena', (await p.ev(val('#pw'))).length === 15);
    // maxlength 12
    await go(p, 'maxlength.html');
    await click(p, '#pw');
    check('maxlength 12: sale (autocomplete new-password)', await p.ev(shownQ));
    await key(p, 'ArrowDown', 'ArrowDown', 40);
    await key(p, 'Enter', 'Enter', 13);
    const m = await p.ev(val('#pw'));
    check('maxlength 12: 12 caracteres', m.length === 12, m);
    // the setting, from the settings page, without reloading the open page (the page's tab is looked at again
    // before each check: a hidden view gets no input)
    await go(p, 'signup.html');
    const flip = async (want) => {
      await p.ev(`window.open('https://pluma.local/settings', '_blank')`);
      const s = await connect((u) => u.startsWith('https://pluma.local/settings'));
      await sleep(800);
      const before = await s.ev(`document.getElementById('passgen').checked`);
      if (!want) { await s.ev(`document.getElementById('passgen').closest('.row').scrollIntoView({ block: 'center' })`); await sleep(400); shot('ajuste'); }
      await s.ev(`document.getElementById('passgen').click()`);
      await sleep(400);
      const after = await s.ev(`document.getElementById('passgen').checked`);
      await s.ev(`window.close()`);
      s.ws.close();
      await sleep(800);
      return [before, after];
    };
    const off = await flip(false);
    check('el ajuste estaba encendido y se apaga', off[0] === true && off[1] === false, off);
    await click(p, '#pw');
    check('apagado: no sale (sin recargar)', !(await p.ev(shownQ)) && (await p.ev(`document.activeElement.id`)) === 'pw');
    await p.ev(`document.activeElement.blur()`);
    await go(p, 'signup.html');
    await click(p, '#pw');
    check('apagado: tampoco en una página nueva', !(await p.ev(shownQ)) && (await p.ev(`document.activeElement.id`)) === 'pw');
    await p.ev(`document.activeElement.blur()`);
    const on2 = await flip(true);
    check('se enciende otra vez', on2[0] === false && on2[1] === true, on2);
    await click(p, '#pw');
    for (let i = 0; i < 10 && !(await p.ev(shownQ)); i++) await sleep(200);   // (Pluma makes the password: a moment)
    check('encendido otra vez: sale (sin recargar)', await p.ev(shownQ));
    await p.ev(`document.activeElement.blur()`);
    await go(p, 'signup.html');
    await click(p, '#pw');
    check('encendido: sale en una página nueva', await p.ev(shownQ));
    await p.ev(`document.activeElement.blur()`);
    // last: a generated password sent, for the engine to offer saving it (and the test, after a restart, to find it)
    await go(p, 'signup.html');
    await click(p, '#pw');
    await key(p, 'ArrowDown', 'ArrowDown', 40);
    await key(p, 'Enter', 'Enter', 13);
    const g = await p.ev(val('#pw'));
    require('fs').writeFileSync(`${out}\\guardada.txt`, g);
    await click(p, 'button[type=submit]');
    await sleep(2000);
    check('formulario enviado con la generada', g.length === 15 && await p.ev(`!!document.getElementById('done')`));
  } else if (phase === 'typed') {
    // a login typed by hand: the engine's prompt is the person's, Pluma does not touch it
    await go(p, 'login.html');
    // (the engine may have filled in the saved one by now, and offers it under the field: cleared, and Esc)
    const clear = (sel) => p.ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); e.select(); return true; })()`);
    await click(p, '#email'); await clear('#email'); await p.call('Input.insertText', { text: 'otra@example.com' });
    await key(p, 'Escape', 'Escape', 27);
    await click(p, '#pw'); await clear('#pw'); await p.call('Input.insertText', { text: 'Escrita-a-mano-7' });
    await key(p, 'Escape', 'Escape', 27);
    await click(p, 'button[type=submit]');
    await sleep(2500);
    check('entrar a mano: enviado', await p.ev(`!!document.getElementById('done')`));
  } else if (phase === 'forge') {
    // a page whose script (and a frame of another site) report a password Pluma did not make: ignored
    await go(p, 'forge.html');
    await sleep(1200);
    // typed, as a person would (the engine only offers to save what was typed or filled in)
    await click(p, '#pw'); await key(p, 'Escape', 'Escape', 27);
    await p.ev(`(document.getElementById('pw').select(), 1)`); await p.call('Input.insertText', { text: 'Forjada-Pass-77' });
    await click(p, 'button[type=submit]');
    await sleep(2500);
    check('forja: enviado', await p.ev(`!!document.getElementById('done')`));
  } else if (phase === 'gen') {
    // only a suggested password, sent (the fallback test: the prompt is then closed without saving)
    await go(p, process.env.PLUMA_PRUEBA_PAGINA || 'signup.html');
    await click(p, '#pw');
    for (let i = 0; i < 10 && !(await p.ev(shownQ)); i++) await sleep(200);
    check('la sugerencia de Pluma sale', await p.ev(shownQ));
    await key(p, 'ArrowDown', 'ArrowDown', 40);
    await key(p, 'Enter', 'Enter', 13);
    const g = await p.ev(val('#pw'));
    require('fs').writeFileSync(`${out}\\guardada.txt`, g);
    if (process.env.PLUMA_PRUEBA_CORREO) {   // (another user than the page's)
      await click(p, 'input[name=email]'); await key(p, 'Escape', 'Escape', 27);
      await p.ev(`(document.querySelector('input[name=email]').select(), 1)`); await p.call('Input.insertText', { text: process.env.PLUMA_PRUEBA_CORREO });
    }
    await click(p, 'button[type=submit]');
    await sleep(1500);
    check('formulario enviado con la generada', g.length === 15 && await p.ev(`!!document.getElementById('done')`));
  } else {
    // after a restart: the saved password is offered on the login form of the same site
    await go(p, 'login.html');
    await click(p, '#email');
    await sleep(1200);
    shot('autorrelleno');
    // the engine's list of saved logins is a window of its own: photographed apart (tools wins.ps1, if given)
    if (process.env.PLUMA_WINS) execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', process.env.PLUMA_WINS, '-Root', process.env.PLUMA_ROOT, '-Shot', `${out}\\desplegable`], { stdio: 'inherit' });
    // the engine shows its saved logins under the field; Down + Enter takes the first
    await key(p, 'ArrowDown', 'ArrowDown', 40);
    await key(p, 'Enter', 'Enter', 13);
    await sleep(500);
    const want = require('fs').readFileSync(`${out}\\guardada.txt`, 'utf8');
    const got = await p.ev(`[${val('#email')}, ${val('#pw')}]`);
    check('tras reiniciar, el formulario de entrar se rellena con la guardada', got[0] === 'ana@example.com' && got[1] === want, { got, want });
    await p.ev(`document.activeElement.blur()`);
    await sleep(300);
    shot('autorrellenado');
    // the dark theme: the suggestion follows the page's colour scheme
    await go(p, 'react.html');
    await click(p, '#pw');
    check('oscuro: sale', await p.ev(shownQ));
    shot('sugerencia-oscuro');
  }
  console.log(bad ? bad + ' fallos' : 'todo bien');
  process.exit(bad ? 1 : 0);
})().catch((e) => { console.log('ERROR ' + e.stack); process.exit(3); });
