// node tools/traducir-prueba.js <port> <step>   checks the page translator (src/translate.js) on a test instance
// (PLUMA_DEPURAR=<port>) with a page at https://example.com/ whose body this script replaces with a fixture in French
// (in its own div: the page or an extension may add things of its own to the body).
// Steps, in this order (tools\traducir-prueba.ps1 runs them, asking Pluma to translate in between):
//   prep       the fixture, and a copy of its HTML
//   check      waits for the translation, then: the text changed, the layout (every element) did not, and what must
//              never be translated (script, style, code, pre, textarea, contenteditable, translate=no) is untouched
//   dyn        adds a paragraph: it must be translated too (MutationObserver)
//   restored   after "show original": the body's HTML is exactly the fixture's again, with the new paragraph
const [port, step] = process.argv.slice(2);
const FIXTURE = `
<h1 id="h">Le chat noir et la maison</h1>
<p id="a">Le chat noir dort sur le canapé pendant que la pluie tombe doucement sur la ville.</p>
<p id="b">Les enfants lisent <a href="https://example.com/livre" class="lien">un livre très intéressant</a> avec <b>leurs amis</b> dans le jardin.</p>
<ul><li id="li">Une liste avec des pommes et des poires.</li></ul>
<p id="e">Bonjour <script>var keep = "Le chat ne doit pas changer";</script>tout le monde, comment allez-vous aujourd'hui ?</p>
<style id="st">.lien { color: red; } /* le style ne change pas */</style>
<p id="c">Le code <code id="code">document.querySelector("le chat")</code> reste comme il est.</p>
<pre id="pre">Le texte préformaté ne se traduit pas.</pre>
<textarea id="ta">Le texte que vous écrivez reste à vous.</textarea>
<div id="ce" contenteditable="true">Ce que vous éditez ne change pas.</div>
<p id="no" translate="no">Ce paragraphe dit de ne pas le traduire.</p>
<p id="nt" class="notranslate">Celui-ci non plus, avec la classe notranslate.</p>
<input id="in" placeholder="Écrivez votre nom ici">
<p id="ti" title="Une petite aide pour vous">Passez la souris sur ce texte pour voir une aide.</p>`;
const DYN = '<p id="dyn">Le petit chien court dans le jardin avec son ami le chat.</p>';

function run(target, expr) {
  return new Promise(async (resolve, reject) => {
    const ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; }).catch(reject);
    ws.onmessage = (m) => {
      const d = JSON.parse(m.data);
      if (d.id !== 1) return;
      ws.close();
      if (d.result.exceptionDetails) reject(new Error(JSON.stringify(d.result.exceptionDetails)));
      else resolve(d.result.result.value);
    };
    ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression: expr, returnByValue: true, awaitPromise: true } }));
  });
}
let chosen = null;
async function evaluate(expr) {
  // the example.com page on screen (a test instance may hold other views of it, hidden: a preload, a spare)
  for (let i = 0; i < 40 && !chosen; i++) {
    const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    for (const t of list.filter((x) => x.type === 'page' && x.url.startsWith('https://example.com')))
      if ((await run(t, 'document.visibilityState').catch(() => '')) === 'visible') { chosen = t; break; }
    if (!chosen) await sleep(250);
  }
  if (!chosen) {
    const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const states = [];
    for (const t of list.filter((x) => x.type === 'page')) states.push(t.url + ' ' + (await run(t, 'document.visibilityState').catch((e) => e.message)));
    throw new Error('no example.com page on screen: ' + states.join(' | '));
  }
  return run(chosen, expr);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitDone(ms) {
  for (let t = 0; t < ms; t += 300) {
    const s = await evaluate('window.__plumaTr ? __plumaTr.stats() : null');
    if (s && s.on && s.done > 0 && s.left === 0) return s;
    await sleep(300);
  }
  throw new Error('the translation did not finish');
}

const steps = {
  async prep() {
    return evaluate(`(() => { document.body.innerHTML = '<div id="fx"></div>'; const fx = document.getElementById('fx'); fx.innerHTML = ${JSON.stringify(FIXTURE)}; window.__orig = fx.innerHTML;
      window.__texts = {}; for (const e of document.querySelectorAll('[id]')) window.__texts[e.id] = e.id === 'ta' ? e.value : e.textContent;
      window.__count = fx.getElementsByTagName('*').length; return { ok: true, elements: window.__count }; })()`);
  },
  async check() {
    const s = await waitDone(30000);
    return evaluate(`(() => {
      const fails = [], T = window.__texts, $ = (id) => document.getElementById(id);
      for (const id of ['h', 'a', 'b', 'li', 'e', 'c', 'ti']) if ($(id).textContent === T[id]) fails.push(id + ' not translated');
      for (const id of ['st', 'code', 'pre', 'ce', 'no', 'nt']) if ($(id).textContent !== T[id]) fails.push(id + ' changed: ' + $(id).textContent);
      if ($('ta').value !== T.ta) fails.push('textarea changed');
      if (!/Le chat ne doit pas changer/.test($('e').querySelector('script').textContent)) fails.push('script changed');
      if ($('in').placeholder === 'Écrivez votre nom ici') fails.push('placeholder not translated');
      if ($('ti').title === 'Une petite aide pour vous') fails.push('title not translated');
      if ($('fx').getElementsByTagName('*').length !== window.__count) fails.push('elements changed');
      const a = $('b').querySelector('a.lien');
      if (!a || a.getAttribute('href') !== 'https://example.com/livre' || !$('b').querySelector('b')) fails.push('the link or the bold went');
      return { ok: !fails.length, fails, stats: ${JSON.stringify(s)}, a: $('a').textContent, b: $('b').textContent, e: $('e').textContent };
    })()`);
  },
  async dyn() {
    await evaluate(`(() => { document.getElementById('fx').insertAdjacentHTML('beforeend', ${JSON.stringify(DYN)}); window.__dyn = document.getElementById('dyn').outerHTML; return 1; })()`);
    for (let t = 0; t < 15000; t += 300) {
      const txt = await evaluate(`document.getElementById('dyn').textContent`);
      if (!/Le petit chien/.test(txt)) return { ok: true, dyn: txt };
      await sleep(300);
    }
    return { ok: false, fails: ['the new paragraph was not translated'] };
  },
  async restored() {
    await sleep(800);
    return evaluate(`(() => { const want = window.__orig + window.__dyn, got = document.getElementById('fx').innerHTML;
      let at = 0; while (at < want.length && want[at] === got[at]) at++;
      return { ok: got === want, firstDifference: got === want ? null : [want.slice(at, at + 80), got.slice(at, at + 80)],
               stats: window.__plumaTr && __plumaTr.stats() }; })()`);
  },
};

(async () => {
  try {
    const r = await steps[step]();
    console.log(step + ': ' + JSON.stringify(r));
    process.exit(r && r.ok === false ? 1 : 0);
  } catch (e) {
    console.log(step + ': FALLA ' + e.message);
    process.exit(1);
  }
})();
