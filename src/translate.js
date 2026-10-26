// Pluma's page translator (RCDATA 220). translate.cpp puts it in a page only when the person asks for a translation
// (or for a language set to "always"); it never touches the network nor talks to Pluma: Pluma asks it for pieces of
// text (take), translates them itself and hands them back (put), as plain strings that go into the same text nodes
// with nodeValue. Nothing is parsed as HTML in the page, so the layout, the links, the listeners and the page's own
// scripts stay as they were, and the translation cannot inject anything.
// A piece is the run of text nodes of one block (a paragraph with its links and bold words), sent as
// "<a i=0>Le </a><a i=1>chat</a>" so the sentence is translated whole (as TWP and Firefox do). Visible pieces go first.
// New text (a MutationObserver) is translated as it comes; "show original" puts back every node still holding a
// translation (the text a page changed meanwhile is left alone).
(() => {
  if (window.__plumaTr) return;
  const SKIP = 'script,style,noscript,template,textarea,input,select,option,code,pre,kbd,samp,svg,math,iframe,object,[translate="no"],.notranslate';
  const INLINE = new Set(['A', 'ABBR', 'ACRONYM', 'B', 'BDI', 'BDO', 'BIG', 'CITE', 'DFN', 'EM', 'FONT', 'I', 'LABEL', 'MARK', 'Q', 'S',
                          'SMALL', 'SPAN', 'STRONG', 'SUB', 'SUP', 'TIME', 'U', 'TT', 'VAR', 'WBR', 'BR', 'DATA', 'INS', 'DEL', 'NOBR']);
  const MAX_CHARS = 1500, MAX_NODES = 40, CACHE_MAX = 6000;
  const letters = /\p{L}/u;
  const CLOSE = /^[,.;:!?)\]}%»、。，．]/, OPEN = /[(\[{¿¡«]$/;
  const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  let rec = new WeakMap();        // node -> { orig, tr }: what was in it and what Pluma put in it
  let queued = new WeakSet();     // nodes in a piece waiting for (or on its way to) a translation
  let applied = [];               // WeakRefs to the nodes holding a translation
  let queue = [], inflight = new Map(), nextId = 1, on = false, tl = '', total = 0, done = 0;
  let cache = new Map();          // piece sent -> its translation (the same text again, as a site re-renders, costs nothing)
  let mo = null, dirty = new Set(), dirtyTimer = 0, sortedY = null, unsorted = false;

  // attributes and the title, as nodes (the same code then serves them)
  const attrNodes = new WeakMap();
  function attrNode(el, a) {
    let m = attrNodes.get(el);
    if (!m) attrNodes.set(el, m = {});
    return m[a] || (m[a] = { el, get isConnected() { return el.isConnected; },
                             get nodeValue() { return el.getAttribute(a) || ''; }, set nodeValue(v) { el.setAttribute(a, v); } });
  }
  const titleNode = { isConnected: true, get nodeValue() { return document.title; }, set nodeValue(v) { document.title = v; } };

  function blockOf(n) {
    let e = n.parentElement;
    while (e && INLINE.has(e.tagName) && e.parentElement) e = e.parentElement;
    return e;
  }
  function fresh(n) {   // text that is not Pluma's translation and not on its way to one
    const v = n.nodeValue;
    if (!v || !/\S/.test(v) || queued.has(n)) return false;
    const r = rec.get(n);
    return !(r && v === r.tr);
  }
  function prio(el) {
    // how far from the screen (0 = on it); what is not shown at all, last
    if (!el || !el.getBoundingClientRect) return 0;
    const r = el.getBoundingClientRect();
    if (!r.width && !r.height) return 1e9;
    if (r.bottom < 0) return -r.bottom + 1;
    if (r.top > innerHeight) return r.top - innerHeight + 1;
    return 0;
  }
  function push(block, nodes) {
    // (the punctuation and numbers between words go with them, so the translation keeps them in place; a piece
    // without any letter is not sent)
    if (!nodes.some((n) => letters.test(n.nodeValue))) return;
    const parts = nodes.map(n => { const m = /^(\s*)([\s\S]*?)(\s*)$/.exec(n.nodeValue); return { lead: m[1], core: m[2], trail: m[3], full: n.nodeValue }; });
    const html = nodes.length === 1 ? esc(parts[0].core)
      : parts.map((x, i) => (i && (parts[i - 1].trail || x.lead) ? ' ' : '') + '<a i=' + i + '>' + esc(x.core) + '</a>').join('');
    const p = { id: nextId++, block, nodes, parts, html, prio: 0 };
    for (const n of nodes) queued.add(n);
    total++;
    const c = cache.get(html);
    if (c) { apply(p, c); return; }
    queue.push(p);
    unsorted = true;
  }
  function scan(root) {
    if (!root) return;
    if (root.nodeType === 3) root = root.parentNode;
    if (!root || (root.nodeType !== 1 && root.nodeType !== 9 && root.nodeType !== 11)) return;
    if (root.nodeType === 1 && (root.closest(SKIP) || root.isContentEditable)) return;
    const skip = new Map();   // parent -> not to translate (asked once per parent)
    const skipped = e => {
      if (!e) return true;
      let s = skip.get(e);
      if (s === undefined) skip.set(e, s = !!(e.closest(SKIP) || e.isContentEditable));
      return s;
    };
    const w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, { acceptNode: n => fresh(n) && !skipped(n.parentElement) ? 1 : 3 });
    let block = null, nodes = [], len = 0, n;
    while ((n = w.nextNode())) {
      const b = blockOf(n);
      if (nodes.length && (b !== block || len > MAX_CHARS || nodes.length >= MAX_NODES)) { push(block, nodes); nodes = []; len = 0; }
      block = b; nodes.push(n); len += n.nodeValue.length;
    }
    if (nodes.length) push(block, nodes);
    // what the page shows in fields and tooltips
    if (root.querySelectorAll) {
      const els = [...root.querySelectorAll('[placeholder],[title]')];
      if (root.nodeType === 1 && (root.hasAttribute('placeholder') || root.hasAttribute('title'))) els.push(root);
      for (const e of els) {
        if (e.closest('[translate="no"],.notranslate,script,style,svg,code,pre') || e.isContentEditable) continue;
        for (const a of ['placeholder', 'title']) {
          if (!e.hasAttribute(a)) continue;
          const p = attrNode(e, a);
          if (fresh(p)) push(e, [p]);
        }
      }
    }
  }
  function apply(p, segs) {
    let any = false;
    p.nodes.forEach((n, i) => {
      queued.delete(n);
      const x = p.parts[i], s = segs[i];
      if (n.nodeValue !== x.full) {   // the page changed it meanwhile: its new words, in the next round
        if (on && n.parentNode) { dirty.add(n.parentNode); if (!dirtyTimer) dirtyTimer = setTimeout(flush, 250); }
        return;
      }
      if (typeof s !== 'string') return;
      const t = s.trim();
      // the node's own spaces; and a space the translation put between two nodes that had none (Japanese -> Spanish),
      // only where neither neighbour keeps one of its own ("un libro  con" otherwise), never after an opening bracket
      // nor before a closing one, a comma or a full stop ("mantequilla ." otherwise)
      const prev = p.parts[i - 1], next = p.parts[i + 1];
      const near = (j) => typeof segs[j] === 'string' ? segs[j].trim() : '';
      const lead = CLOSE.test(t) ? '' : x.lead || (prev && !prev.trail && /^\s/.test(s) && !OPEN.test(near(i - 1)) ? ' ' : '');
      const trail = x.trail || (next && !next.lead && /\s$/.test(s) && !OPEN.test(t) && !CLOSE.test(near(i + 1)) ? ' ' : '');
      const v = lead + t + trail;
      rec.set(n, { orig: x.full, tr: v });
      if (v !== x.full) { n.nodeValue = v; applied.push(new WeakRef(n)); any = true; }
    });
    done++;
    if (applied.length > 50000) applied = applied.filter(r => r.deref());
    return any;
  }
  function observe(list) {
    if (!on) return;
    for (const m of list) {
      if (m.type === 'characterData') {
        const n = m.target, r = rec.get(n);
        if (r && n.nodeValue === r.tr) continue;   // (Pluma's own change)
        if (r) rec.delete(n);                     // the page wrote something new in it: translated again
        dirty.add(n.parentNode);
      } else for (const a of m.addedNodes) if (a.nodeType === 1 || a.nodeType === 3) dirty.add(a);
    }
    if (dirty.size && !dirtyTimer) dirtyTimer = setTimeout(flush, 250);
  }
  function flush() {
    clearTimeout(dirtyTimer); dirtyTimer = 0;
    const d = [...dirty]; dirty.clear();
    for (const r of d) if (r && r.isConnected) scan(r);
  }
  function stats() { return { on, tl, total, done, left: queue.length + inflight.size }; }

  const api = {
    start(lang) {
      if (on) api.restore();
      if (lang !== tl) { cache.clear(); tl = lang; }
      on = true; total = done = 0; sortedY = null;
      if (fresh(titleNode)) push(document.documentElement, [titleNode]);
      if (document.body) {
        scan(document.body);
        mo = new MutationObserver(observe);
        mo.observe(document.body, { childList: true, subtree: true, characterData: true });
      }
      return stats();
    },
    take(budget) {
      // the next pieces, those nearest the screen first, up to budget characters: [[id, html, nodes], ...]
      if (!on) return null;
      if (dirty.size) flush();
      if (unsorted || sortedY === null || Math.abs(scrollY - sortedY) > innerHeight / 2) {
        for (const p of queue) p.prio = prio(p.block);
        queue.sort((a, b) => a.prio - b.prio);
        sortedY = scrollY; unsorted = false;
      }
      const q = [];
      let used = 0;
      while (queue.length && used < budget) {
        const p = queue.shift();
        if (!p.nodes.some(n => n.isConnected)) { total--; for (const n of p.nodes) queued.delete(n); continue; }   // (gone meanwhile)
        inflight.set(p.id, p);
        q.push([p.id, p.html, p.nodes.length]);
        used += p.html.length;
      }
      const s = stats(); s.q = q;
      return s;
    },
    put(list) {
      // [[id, [text per node]], ...]; [id, null] goes back to the queue
      for (const [id, segs] of list) {
        const p = inflight.get(id);
        if (!p) continue;
        inflight.delete(id);
        if (!Array.isArray(segs)) { queue.push(p); unsorted = true; continue; }
        if (cache.size >= CACHE_MAX) cache.delete(cache.keys().next().value);
        cache.set(p.html, segs);
        if (on) apply(p, segs);
      }
      return stats();
    },
    requeue() { for (const p of inflight.values()) queue.push(p); inflight.clear(); unsorted = true; return stats(); },
    restore() {
      on = false;
      if (mo) { mo.disconnect(); mo = null; }
      clearTimeout(dirtyTimer); dirtyTimer = 0; dirty.clear();
      for (const w of applied) {
        const n = w.deref(), r = n && rec.get(n);
        if (r && n.nodeValue === r.tr) n.nodeValue = r.orig;
      }
      applied = []; queue = []; inflight.clear(); total = done = 0;
      rec = new WeakMap(); queued = new WeakSet();
      return stats();
    },
    stats,
  };
  Object.defineProperty(window, '__plumaTr', { value: Object.freeze(api), configurable: false, enumerable: false, writable: false });
})();
