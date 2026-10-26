// Pluma: "Use a strong password" on sign-up forms, as Chrome and Edge do (their own needs their account sync, which
// the engine has not got). Injected in every page and frame by passgen.cpp (AddScriptToExecuteOnDocumentCreated), so it
// runs before the page's own scripts and keeps the browser's own functions. One focusin listener; nothing else runs
// until a password field gets the focus. The suggestion is drawn in a closed shadow root in the top layer, with its
// own styles (nothing of the page's CSS reaches it, nothing of it reaches the page).
// Which fields: Chromium's local rules (autocomplete="new-password"; new + confirm fields; names that say so), never
// a current-password one nor a lone password field on a login form.
// The password: as Chromium's generator (15 characters, no look-alike ones, the field's minlength/maxlength/pattern),
// plus a symbol, from crypto.getRandomValues without modulo bias.
(() => {
  const CFG = /*PLUMA_CFG*/{ on: true, incognito: false, key: 'test', test: true, use: 'Usar contraseña segura',
    save: 'Pluma la guarda para este sitio', nosave: 'En incógnito no se guarda: apúntala' }/*PLUMA_CFG_FIN*/;
  const G = globalThis;

  // ---------------------------------------------------------------- the generator
  const LOWER = 'abcdefghijkmnpqrstuvwxyz', UPPER = 'ABCDEFGHJKLMNPQRSTUVWXYZ', DIGITS = '23456789', SYMBOLS = '-_.:!';
  const rnd = G.crypto.getRandomValues.bind(G.crypto);
  const uniform = (n) => {   // 0..n-1, without modulo bias
    const lim = Math.floor(0x100000000 / n) * n, b = new Uint32Array(1);
    for (;;) { rnd(b); if (b[0] < lim) return b[0] % n; }
  };
  const build = (len, classes) => {
    // one of each class (while it fits), the rest from all of them, shuffled; no "--" or "__" (they read as one line)
    const all = classes.join('');
    for (let tries = 0; tries < 20; tries++) {
      const out = classes.slice(0, len).map((c) => c[uniform(c.length)]);
      while (out.length < len) out.push(all[uniform(all.length)]);
      for (let i = out.length - 1; i > 0; i--) { const j = uniform(i + 1); const x = out[i]; out[i] = out[j]; out[j] = x; }
      const s = out.join('');
      if (!/--|__/.test(s)) return s;
    }
    return null;
  };
  const compile = (p) => {
    if (!p) return null;
    for (const flags of ['v', 'u']) { try { return new RegExp('^(?:' + p + ')$', flags); } catch (e) {} }
    return null;
  };
  // f: { minLength, maxLength, pattern } as the input element gives them (-1 / '' when absent); null if nothing fits
  const generate = (f) => {
    const lo = Math.max(4, f.minLength > 0 ? f.minLength : 0), hi = Math.min(200, f.maxLength > 0 ? f.maxLength : 200);
    if (lo > hi) return null;
    const len = Math.min(hi, Math.max(lo, 15));
    const re = compile(f.pattern);
    const sets = [[LOWER, UPPER, DIGITS, SYMBOLS], [LOWER, UPPER, DIGITS], [LOWER, DIGITS], [UPPER, DIGITS], [LOWER, UPPER], [LOWER], [DIGITS]];
    if (!re) return build(len, sets[0]);
    // a pattern: the usual length first, then the nearest others, from the strongest set of characters down
    const lens = [len];
    for (let d = 1; d <= 64; d++) { if (len + d <= hi) lens.push(len + d); if (len - d >= lo) lens.push(len - d); }
    for (const s of sets)
      for (let k = 0; k < lens.length; k++)
        for (let i = 0; i < (k ? 3 : 30); i++) {
          const p = build(lens[k], s);
          if (p && re.test(p)) return p;
        }
    return null;
  };

  // ---------------------------------------------------------------- which fields
  const NEW = /new|neu|nouv|nuev|novo|nuov|confirm|repeat|retype|re-?enter|again|verif|regist|sign[-_ ]?up|join|create\b|crear|repet|repit|password2|passwd2|pass2|pwd2/i;
  const OLD = /current|old|actual|anterior|existing|log[-_ ]?in|log[-_ ]?on|sign[-_ ]?in|entrar|iniciar|acceder/i;
  const SIGNUP = /regist|sign[-_ ]?up|\bjoin\b|\bcreate\b|crear|\balta\b|inscri|new.?account|nueva.?cuenta|nova.?conta|nouveau.?compte|konto.?(erstellen|anlegen)|nuovo.?account/i;
  const isPw = (el) => !!el && el.localName === 'input' && el.type === 'password';
  const shown = (el) => { const r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0 && G.getComputedStyle(el).visibility !== 'hidden'; };
  const ac = (el) => (el.getAttribute('autocomplete') || '').toLowerCase();
  const words = (el) => {
    let s = [el.name, el.id, el.placeholder, el.getAttribute('aria-label'), ac(el)].join(' ');
    if (el.labels) for (const l of el.labels) s += ' ' + l.textContent;
    return s;
  };
  const isCurrent = (el) => { if (/\bcurrent-password\b/.test(ac(el))) return true; const w = words(el); return OLD.test(w) && !NEW.test(w); };
  const scopeOf = (el) => el.form || el.getRootNode();
  const passwords = (scope) => [...(scope.elements || scope.querySelectorAll('input'))].filter((x) => isPw(x) && !x.disabled && shown(x));
  const formSaysSignup = (form) => {
    if (!form) return false;
    let s = [form.id, form.name, form.getAttribute('action'), form.className].join(' ');
    if (SIGNUP.test(s)) return true;
    for (const b of form.querySelectorAll('button:not([type]), button[type="submit"], input[type="submit"]')) {
      const t = (b.textContent || b.value || '') + ' ' + (b.id || '') + ' ' + (b.name || '');
      if (SIGNUP.test(t) && !OLD.test(t)) return true;
    }
    return false;
  };
  // the fields a suggestion fills (the focused one first), or null where Chrome would not offer one
  const classify = (el) => {
    if (!isPw(el) || el.readOnly || el.disabled || el.value) return null;
    if (isCurrent(el)) return null;
    const all = passwords(scopeOf(el));
    let group = all.filter((x) => !isCurrent(x));
    if (group.length === 3 && all.length === 3 && !/\bnew-password\b/.test(ac(group[0]))) group = group.slice(1);   // current, new, confirm
    if (!group.includes(el)) return null;
    const strong = /\bnew-password\b/.test(ac(el)) || NEW.test(words(el));
    if (group.length < 2 && !strong && !formSaysSignup(el.form)) return null;   // a lone password field: a login form
    if (group.length > 3) group = [el];
    return [el, ...group.filter((x) => x !== el && !x.value)];
  };
  if (CFG.test) { G.__plumaPassgenTest = { generate, classify }; return; }
  // never on Pluma's own pages nor on YouTube (its light pages; its accounts are on accounts.google.com)
  if (!/^https?:$/.test(location.protocol) || /(^|\.)(pluma\.local|youtube\.com)$/i.test(location.hostname)) return;

  // ---------------------------------------------------------------- the suggestion
  const D = document;
  const attach = Element.prototype.attachShadow;
  const setValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
  const generated = new WeakMap();   // field -> its suggestion (the same one each time, as Chrome)
  let on = CFG.on, host = null, box = null, opt = null, pwText = null, field = null, group = null, lit = false;
  const CSS = `
:host { all: initial; }
.box { box-sizing: border-box; width: var(--w); font: 13px/1.35 "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif;
  color: #1b1b1f; background: #fff; border: 1px solid rgba(0,0,0,.12); border-radius: 10px; padding: 4px;
  box-shadow: 0 8px 24px rgba(0,0,0,.14), 0 2px 6px rgba(0,0,0,.08); user-select: none; cursor: default; }
.opt { display: flex; align-items: center; gap: 10px; padding: 8px 10px; border-radius: 7px; }
.opt.lit { background: #eef1f6; }
svg { width: 20px; height: 20px; flex: none; fill: none; stroke: #3a66d6; stroke-width: 1.6; stroke-linecap: round; stroke-linejoin: round; }
.txt { min-width: 0; }
.t1 { font-weight: 600; }
.pw { font: 12.5px/1.4 Consolas, "Cascadia Mono", monospace; color: #44464f; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.foot { padding: 6px 10px 6px 40px; border-top: 1px solid rgba(0,0,0,.08); margin-top: 4px; color: #5d5f68; font-size: 12px; }
@media (prefers-color-scheme: dark) {
  .box { color: #e6e6ea; background: #2a2b30; border-color: rgba(255,255,255,.1); box-shadow: 0 8px 24px rgba(0,0,0,.5); }
  .opt.lit { background: #3a3c43; }
  svg { stroke: #8fb1ff; }
  .pw { color: #c3c5cf; }
  .foot { color: #a3a5ae; border-top-color: rgba(255,255,255,.08); }
}`;
  const make = () => {
    host = D.createElement('pluma-passgen');
    const root = attach.call(host, { mode: 'closed' });
    const sheet = new CSSStyleSheet();
    sheet.replaceSync(CSS);
    root.adoptedStyleSheets = [sheet];
    box = D.createElement('div'); box.className = 'box';
    opt = D.createElement('div'); opt.className = 'opt';
    const NS = 'http://www.w3.org/2000/svg';
    const svg = D.createElementNS(NS, 'svg'); svg.setAttribute('viewBox', '0 0 20 20');
    const c = D.createElementNS(NS, 'circle'); c.setAttribute('cx', '6.5'); c.setAttribute('cy', '10'); c.setAttribute('r', '3.5');
    const p = D.createElementNS(NS, 'path'); p.setAttribute('d', 'M10 10h7.5M15 10v2.5M17.5 10v2');
    svg.append(c, p);
    const txt = D.createElement('div'); txt.className = 'txt';
    const t1 = D.createElement('div'); t1.className = 't1'; t1.textContent = CFG.use;
    pwText = D.createElement('div'); pwText.className = 'pw';
    txt.append(t1, pwText);
    opt.append(svg, txt);
    const foot = D.createElement('div'); foot.className = 'foot'; foot.textContent = CFG.incognito ? CFG.nosave : CFG.save;
    box.append(opt, foot);
    root.append(box);
    box.addEventListener('pointerdown', (e) => e.preventDefault());   // the field keeps the focus
    opt.addEventListener('click', () => accept());
    opt.addEventListener('pointerenter', () => light(true));
    opt.addEventListener('pointerleave', () => light(false));
  };
  const light = (x) => { lit = x; if (opt) opt.classList.toggle('lit', x); };
  const css = (el, o) => { for (const k in o) el.style.setProperty(k, o[k], 'important'); };
  const place = () => {
    if (!host || !field) return;
    if (!field.isConnected || !shown(field)) { hide(); return; }
    const r = field.getBoundingClientRect(), vw = D.documentElement.clientWidth || innerWidth, vh = innerHeight;
    const w = Math.min(Math.max(r.width, 280), 380, vw - 16);
    box.style.setProperty('--w', w + 'px');
    const h = host.getBoundingClientRect().height || 90;
    let top = r.bottom + 4;
    if (top + h > vh - 4 && r.top - h - 4 >= 4) top = r.top - h - 4;
    css(host, { left: Math.round(Math.max(8, Math.min(r.left, vw - w - 8))) + 'px', top: Math.round(top) + 'px' });
  };
  const onScroll = () => place();
  const onKey = (e) => {
    if (e.key === 'ArrowDown') { light(true); e.preventDefault(); }
    else if (e.key === 'ArrowUp' && lit) { light(false); e.preventDefault(); }
    else if (e.key === 'Enter' && lit) { e.preventDefault(); e.stopImmediatePropagation(); accept(); }
    else if (e.key === 'Escape') { e.preventDefault(); e.stopImmediatePropagation(); hide(); }
    else if (e.key === 'Tab') hide();
  };
  const onInput = () => { if (field && field.value) hide(); };
  const onBlur = () => hide();
  const show = (el, fill) => {
    if (!host) make();
    if (field !== el) hide();
    let pw = generated.get(el);
    if (!pw) {
      pw = generate({ minLength: el.minLength, maxLength: el.maxLength, pattern: el.getAttribute('pattern') || '' });
      if (!pw) return;
      generated.set(el, pw);
    }
    field = el; group = fill;
    pwText.textContent = pw;
    light(false);
    css(host, { position: 'fixed', inset: 'auto', margin: '0', padding: '0', border: '0', background: 'transparent', overflow: 'visible',
                width: 'auto', height: 'auto', 'max-width': 'none', 'max-height': 'none', display: 'block', 'z-index': '2147483647',
                left: '-9999px', top: '0', opacity: '1', visibility: 'visible', transform: 'none', 'pointer-events': 'auto' });
    const dlg = el.closest('dialog[open]');   // (a modal dialog leaves everything outside it inert)
    const parent = dlg || D.documentElement;
    if (host.parentNode !== parent) parent.append(host);
    if (host.showPopover) { host.popover = 'manual'; try { if (!host.matches(':popover-open')) host.showPopover(); } catch (e) {} }
    place();
    el.addEventListener('keydown', onKey, true);
    el.addEventListener('input', onInput, { passive: true });
    el.addEventListener('focusout', onBlur, { passive: true });
    G.addEventListener('scroll', onScroll, { capture: true, passive: true });
    G.addEventListener('resize', onScroll, { passive: true });
  };
  function hide() {
    if (field) {
      field.removeEventListener('keydown', onKey, true);
      field.removeEventListener('input', onInput);
      field.removeEventListener('focusout', onBlur);
      G.removeEventListener('scroll', onScroll, true);
      G.removeEventListener('resize', onScroll);
    }
    field = null; group = null;
    if (host && host.isConnected) { try { if (host.hidePopover && host.matches(':popover-open')) host.hidePopover(); } catch (e) {} host.remove(); }
  }
  const put = (el, v) => {
    // as if typed: the native setter (React and the like keep their own copy of the value) and the events they hear
    setValue.call(el, v);
    el.dispatchEvent(new InputEvent('input', { bubbles: true, composed: true, inputType: 'insertReplacementText', data: v }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  function accept() {
    if (!field) return;
    const pw = generated.get(field), list = group || [field];
    hide();
    for (const el of list) put(el, pw);
  }
  const consider = (el) => {
    if (!on || !isPw(el) || field === el || G.__pluma) return;   // (__pluma: Pluma's light pages, lite.cpp)
    const fill = classify(el);
    if (fill) show(el, fill);
  };
  G.addEventListener('focusin', (e) => { const el = e.composedPath()[0]; if (isPw(el)) consider(el); }, { capture: true, passive: true });
  // a click on the field that already has the focus (after Esc, as Chrome) brings it back
  G.addEventListener('pointerdown', (e) => { const el = e.composedPath()[0]; if (isPw(el) && !field && el.getRootNode().activeElement === el) consider(el); },
                     { capture: true, passive: true });
  // the setting, switched without reloading (Pluma tells the open pages; the key is only Pluma's)
  G.addEventListener('pluma-passgen-' + CFG.key, (e) => { on = !!e.detail; if (!on) hide(); });
})();
