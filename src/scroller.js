// Pluma's Scroller: the videos of any page, one after another, tall, with the wheel or at the end of each (as TikTok).
// One engine for every site, no code for any one of them. It runs in a world of Pluma's own inside the page
// (scroller.cpp: Page.createIsolatedWorld): it sees the page's DOM, the page never sees it, and only it can call Pluma
// (a binding added to this world alone). Nothing it reads is trusted: Pluma checks every address again before mpv.
// Where the videos come from, in this order (as yt-dlp's generic extractor and the stream-detector extensions):
//   1. the tab's network (Pluma watches the responses: HLS/DASH lists, video files), matched with the page's videos
//      by the id their addresses share (X: the poster's number is the list's)
//   2. the page: <video>/<source>, gifs, og:video, JSON-LD VideoObject, players' set-up in scripts (JW Player, KVS:
//      file:, video_url:), any attribute that holds a video's address (Reddit's players)
//   3. publications: links of the same site that repeat with the same shape of address and carry a thumbnail (an
//      image board's list, a light Reddit page). Their HTML is read (2 at a time, only the next few, stopping on 429/
//      403), inert (XMLHttpRequest's document: no script of it runs; and it passes Trusted Types), with 2's readers.
// The layer is a closed shadow root in the top layer: the page's CSS does not reach it, and its keys and the wheel go
// to it. Pluma's player (mpv, video.cpp) plays in its slot; until a video plays, the slot shows its poster.
(() => {
  'use strict';
  const CFG = /*PLUMA_CFG*/{ test: true }/*PLUMA_CFG_FIN*/;
  const G = globalThis;

  // ---------------------------------------------------------------- readers (pure: a document and its address)
  const VIDEO_EXT = /\.(mp4|m4v|webm|mov|ogv|mkv)$/i, LIST_EXT = /\.(m3u8)$/i, DASH_EXT = /\.(mpd)$/i, GIF_EXT = /\.(gif)$/i;
  const SEGMENT_EXT = /\.(ts|m4s|m4a|aac|mp3|vtt|cmfv|cmfa|init)$/i;
  const MEDIA_IN_TEXT = /https?:(?:\\?\/){2}[^\s"'<>\\]+?\.(?:mp4|m4v|webm|mov|m3u8|mpd)(?:\?[^\s"'<>\\]*)?(?=["'\s<>\\]|$)/gi;
  const abs = (u, base) => {
    if (!u || typeof u !== 'string') return '';
    u = u.trim().replace(/\\\//g, '/').replace(/\\u0026/gi, '&').replace(/&amp;/g, '&');
    if (u.startsWith('//')) u = 'https:' + u;
    try { const x = new URL(u, base); return x.protocol === 'https:' || x.protocol === 'http:' ? x.href.split('#')[0] : ''; } catch (e) { return ''; }
  };
  const pathOf = (u) => { try { return new URL(u).pathname; } catch (e) { return ''; } };
  // what an address is by its type or its name: 'hls', 'dash', 'file', 'gif' or ''
  const kindOf = (u, type) => {
    const t = (type || '').toLowerCase(), p = pathOf(u);
    if (/mpegurl/.test(t) || LIST_EXT.test(p)) return 'hls';
    if (/dash\+xml/.test(t) || DASH_EXT.test(p)) return 'dash';
    if (t === 'image/gif' || GIF_EXT.test(p)) return /format=mp4|\.gif\?.*mp4/i.test(u) ? 'file' : 'gif';
    if (SEGMENT_EXT.test(p)) return '';
    if (/^video\//.test(t) || VIDEO_EXT.test(p)) return 'file';
    return '';
  };
  const RANK = { hls: 4, dash: 3, file: 2, gif: 1 };
  // the ids an address carries (long numbers, long tokens): a list and its poster share one (X, many CDNs)
  const tokens = (u) => {
    const out = [];
    for (const s of pathOf(u).split(/[^A-Za-z0-9_-]+/)) {
      if ((s.length >= 8 && /\d/.test(s)) || (s.length >= 14 && /^[A-Za-z0-9_-]+$/.test(s))) out.push(s);
    }
    return out;
  };
  const text = (e) => (e && e.textContent ? e.textContent.replace(/\s+/g, ' ').trim() : '');
  const meta = (d, ...names) => {
    for (const n of names) {
      const m = d.querySelector(`meta[property="${n}"], meta[name="${n}"]`);
      if (m && m.getAttribute('content')) return m.getAttribute('content');
    }
    return '';
  };
  const titleOf = (d) => (meta(d, 'og:title', 'twitter:title') || text(d.querySelector('title')) || '').slice(0, 200);

  // the videos in a document, in its order: { src, kind, el, poster, title, how } (kind 'blob': the page plays it from
  // a stream of its own, there is no address; 'embed': another site's player in a frame)
  function mediaIn(d, base, live) {
    const out = [], seen = new Set();
    const add = (x) => {
      if (x.src) { const k = x.src; if (seen.has(k)) return; seen.add(k); }
      out.push(x);
    };
    const root = d.body || d.documentElement;
    if (!root) return out;
    // <video> and its <source>s; gifs; links straight to a video file; frames of a player
    const els = root.querySelectorAll('video, img, a[href], iframe, embed, object, [src], [data-src], [data-video], [data-mp4], [data-hls], [data-video-url], [data-url], [packaged-media-json]');
    for (const el of els) {
      const tag = el.localName;
      if (tag === 'video') {
        const poster = abs(el.getAttribute('poster'), base);
        const cands = [];
        const own = live && el.currentSrc ? el.currentSrc : el.getAttribute('src') || '';
        if (own) cands.push([own, el.getAttribute('type') || '']);
        for (const s of el.querySelectorAll('source')) cands.push([s.getAttribute('src') || '', s.getAttribute('type') || '']);
        let best = null;
        for (const [u, ty] of cands) {
          const a = abs(u, base), k = a ? kindOf(a, ty) || (/^video\//i.test(ty) || !ty ? 'file' : '') : '';
          if (a && k && (!best || RANK[k] > RANK[best.kind])) best = { src: a, kind: k };
        }
        if (best) add({ src: best.src, kind: best.kind, el, poster, how: 'video' });
        else if (cands.some(([u]) => /^blob:|^mediasource:/i.test(u)) || (live && (el.currentSrc || '').startsWith('blob:')) || poster)
          add({ src: '', kind: 'blob', el, poster, how: 'video' });
        continue;
      }
      if (tag === 'img') {
        const a = abs(el.getAttribute('src') || el.getAttribute('data-src'), base);
        if (a && kindOf(a) === 'gif') {
          const w = +(el.getAttribute('width') || (live ? el.naturalWidth : 0) || 0), h = +(el.getAttribute('height') || (live ? el.naturalHeight : 0) || 0);
          if ((!w || w >= 120) && (!h || h >= 120)) add({ src: a, kind: 'gif', el, poster: '', how: 'gif' });
        }
        continue;
      }
      if (tag === 'a') {
        const a = abs(el.getAttribute('href'), base), k = a ? kindOf(a) : '';
        if (k) add({ src: a, kind: k, el, poster: '', how: 'link' });
        continue;
      }
      if (tag === 'iframe' || tag === 'embed' || tag === 'object') {
        const a = abs(el.getAttribute('src') || el.getAttribute('data'), base);
        const w = +(el.getAttribute('width') || 0) || (live ? el.clientWidth : 0), big = !w || w >= 200;
        if (a && big && (el.hasAttribute('allowfullscreen') || /autoplay|fullscreen|encrypted-media/.test(el.getAttribute('allow') || '') || /\/embed\/|\/player|\/video/i.test(a)))
          add({ src: '', embed: a, kind: 'embed', el, poster: '', how: 'frame' });
        continue;
      }
      // any other element whose attributes hold a video's address (a site's own player element)
      let best = null;
      for (const at of el.attributes) {
        if (at.name === 'href' || at.value.length > 40000 || !/https?:|\/\//.test(at.value)) continue;
        const found = at.value.match(MEDIA_IN_TEXT) || (/^(https?:)?\/\//.test(at.value) ? [at.value] : []);
        for (const u of found) {
          const a = abs(u, base), k = a ? kindOf(a) : '';
          if (k && (!best || RANK[k] > RANK[best.kind])) best = { src: a, kind: k };
        }
      }
      if (best) add({ src: best.src, kind: best.kind, el, poster: abs(el.getAttribute('poster') || el.getAttribute('thumbnail'), base), how: 'attr' });
    }
    // the document's own: og:video, JSON-LD, a player set up in a script (these say what the page is about)
    const page = [];
    const ogType = meta(d, 'og:video:type');
    for (const n of ['og:video:secure_url', 'og:video:url', 'og:video', 'twitter:player:stream']) {
      const a = abs(meta(d, n), base);
      if (!a) continue;
      const k = kindOf(a, /video|mpegurl/.test(ogType) ? ogType : '');
      if (k) { page.push({ src: a, kind: k, how: 'og' }); break; }
      if (/text\/html/.test(ogType) || n === 'og:video') { page.push({ src: '', embed: a, kind: 'embed', how: 'og' }); break; }
    }
    for (const s of d.querySelectorAll('script[type="application/ld+json"]')) {
      let j; try { j = JSON.parse(s.textContent); } catch (e) { continue; }
      const walk = (o, depth) => {
        if (!o || typeof o !== 'object' || depth > 6) return;
        if (Array.isArray(o)) { for (const x of o) walk(x, depth + 1); return; }
        const ty = [].concat(o['@type'] || []);
        if (ty.includes('VideoObject')) {
          const c = abs(o.contentUrl, base), e = abs(o.embedUrl, base), th = abs([].concat(o.thumbnailUrl || [])[0], base);
          if (c && kindOf(c)) page.push({ src: c, kind: kindOf(c), how: 'ld', poster: th, title: typeof o.name === 'string' ? o.name : '' });
          else if (e) page.push({ src: '', embed: e, kind: 'embed', how: 'ld', poster: th });
        }
        for (const k of Object.keys(o)) if (typeof o[k] === 'object') walk(o[k], depth + 1);
      };
      walk(j, 0);
    }
    let budget = 3e6;
    for (const s of d.querySelectorAll('script:not([src])')) {
      const t = s.textContent || '';
      if (/ld\+json/.test(s.getAttribute('type') || '') || t.length > budget) continue;
      budget -= t.length;
      // players' set-up: file: / video_url: / src: / sources: [...] (JW Player, KVS, Video.js, Flowplayer, plain)
      const re = /(?:file|src|source|video_url|videoUrl|video_alt_url|contentUrl|hls|hlsUrl|mp4|stream|url)["']?\s*[:=]\s*["'](?:function\/0\/)?((?:https?:)?(?:\\?\/){2}[^"'\s<>]+?)["']/gi;
      let m;
      while ((m = re.exec(t))) {
        const a = abs(m[1], base), k = a ? kindOf(a) : '';
        if (k && k !== 'gif') page.push({ src: a, kind: k, how: 'script' });
      }
    }
    // the document's own go with its biggest video element, if it has one without an address (else at the end)
    for (const p of page) {
      if (p.src && seen.has(p.src)) continue;
      const host = out.find((x) => x.kind === 'blob' && !x.src) || (p.kind === 'embed' ? out.find((x) => x.kind === 'embed' && x.embed === p.embed) : null);
      if (host && p.src) { host.src = p.src; host.kind = p.kind; host.how = p.how; if (!host.poster) host.poster = p.poster || ''; seen.add(p.src); continue; }
      if (host) continue;
      add({ src: p.src, kind: p.kind, embed: p.embed, el: null, poster: p.poster || abs(meta(d, 'og:image'), base), title: p.title || '', how: p.how });
    }
    return out;
  }
  // the one video a publication's page is about: the best of what it has, its own player first
  const HOW = { video: 6, attr: 5, og: 4, ld: 4, script: 3, link: 2, gif: 1, frame: 0 };
  function mainMedia(d, base) {
    const all = mediaIn(d, base, false).filter((x) => x.src || x.embed);
    if (!all.length) return null;
    let best = null;
    for (const x of all) {
      const s = (x.src ? 100 : 0) + (HOW[x.how] || 0) * 10 + (RANK[x.kind] || 0);
      if (!best || s > best.s) best = { s, x };
    }
    const x = best.x;
    return { src: x.src, kind: x.src ? x.kind : 'embed', embed: x.embed || '', poster: x.poster || abs(meta(d, 'og:image', 'twitter:image'), base),
             title: x.title || titleOf(d) };
  }

  // publications: links of this site with the same shape of address, carrying a thumbnail. The shape: numbers and long
  // ids become *, and one other part may vary (/r/<any>/comments/*/*)
  function shape(u) {
    const segs = u.pathname.split('/').filter(Boolean).map((s) => (/\d/.test(s) && s.length >= 2) || s.length > 24 ? '*' : s.toLowerCase());
    const q = [...u.searchParams.keys()].sort().map((k) => k + '=' + (/^\d+$/.test(u.searchParams.get(k)) ? '*' : u.searchParams.get(k).toLowerCase().slice(0, 30)));
    const keys = [segs.join('/') + '?' + q.join('&')];
    if (segs.length >= 3 && segs.includes('*'))
      segs.forEach((s, i) => { if (s !== '*') keys.push(segs.map((x, j) => (j === i ? '?' : x)).join('/') + '?' + q.join('&')); });
    return keys;
  }
  function posts(d, base) {
    let origin, here;
    try { origin = new URL(base).origin; here = base.split('#')[0]; } catch (e) { return null; }
    const groups = new Map();
    for (const a of d.querySelectorAll('a[href]')) {
      let u; try { u = new URL(a.getAttribute('href'), base); } catch (e) { continue; }
      if (u.origin !== origin) continue;
      const href = u.href.split('#')[0];
      if (href === here || kindOf(href)) continue;
      if (!/\d/.test(u.pathname + u.search) && u.pathname.split('/').filter(Boolean).length < 2) continue;   // (menus, sections)
      for (const k of shape(u)) {
        let g = groups.get(k);
        if (!g) groups.set(k, (g = { key: k, links: new Map() }));
        if (!g.links.has(href)) g.links.set(href, []);
        g.links.get(href).push(a);
      }
    }
    let best = null;
    for (const g of groups.values()) {
      if (g.links.size < 3) continue;
      let thumbs = 0;
      for (const as of g.links.values()) if (as.some((a) => a.querySelector('img, picture, video'))) thumbs++;
      if (thumbs < 3 || thumbs < g.links.size * 0.4) continue;
      const score = thumbs * 10 - (g.key.split('?').length > 2 ? 1 : 0) - (g.key.includes('/?') ? 2 : 0);
      if (!best || score > best.score) best = { ...g, score };
    }
    if (!best) return null;
    const out = [];
    for (const [href, as] of best.links) {
      const a = as.find((x) => x.querySelector('img, picture, video')) || as[0];
      // its card: up from the link while nothing else of the list is inside
      let card = a;
      for (let i = 0; i < 5 && card.parentElement; i++) {
        const p = card.parentElement;
        let other = false;
        for (const x of p.querySelectorAll('a[href]')) {
          if (x === a || as.includes(x)) continue;
          let h; try { h = new URL(x.getAttribute('href'), base).href.split('#')[0]; } catch (e) { continue; }
          if (best.links.has(h) && h !== href) { other = true; break; }
        }
        if (other) break;
        card = p;
      }
      const img = a.querySelector('img') || card.querySelector('img');
      out.push({ href, a, card, thumb: img ? abs(img.getAttribute('src') || img.getAttribute('data-src'), base) : '',
                 title: (a.getAttribute('title') || (img && img.getAttribute('alt')) || text(card.querySelector('h1, h2, h3')) || '').slice(0, 200) });
    }
    return { key: best.key, list: out };
  }
  // the next page of a list: rel=next, or a link that says so
  function nextPage(d, base) {
    const l = d.querySelector('link[rel~="next"][href], a[rel~="next"][href]');
    if (l) return abs(l.getAttribute('href'), base);
    for (const a of d.querySelectorAll('a[href]')) {
      const t = (text(a) || a.getAttribute('aria-label') || a.getAttribute('title') || '').toLowerCase();
      if (/^(next|next page|siguiente|página siguiente|suivant|weiter|nächste|successivo|próxima|›|»|>|>>|older|más antiguos)$/.test(t)) {
        const u = abs(a.getAttribute('href'), base);
        if (u && u !== base.split('#')[0]) return u;
      }
    }
    return '';
  }
  const R = { mediaIn, mainMedia, posts, nextPage, kindOf, tokens, abs, titleOf };
  if (CFG.test) return R;   // (tools\scroller-test.js: the readers only)

  // ---------------------------------------------------------------- the channel to Pluma
  const send0 = G[CFG.bind];
  try { delete G[CFG.bind]; } catch (e) {}
  const send = (o) => { try { const s = JSON.stringify(o); if (s.length < 60000 && send0) send0(s); } catch (e) {} };
  const TX = (k) => (CFG.texts && CFG.texts[k]) || k;
  const log = (s) => { if (CFG.log) send({ t: 'log', s: String(s).slice(0, 180) }); };
  const host = (() => { try { return location.hostname.replace(/^www\./, ''); } catch (e) { return ''; } })();

  // ---------------------------------------------------------------- the feed
  const S = { items: [], keys: new Set(), cur: -1, dir: 1, open: false, net: [], netKeys: new Set(), resolving: 0, attempts: 0,
              slow: 0, next: '', nextBusy: false, nextDone: false, aspect: 0, playing: null, waiting: 0, ended: 0, muted: false, loop: false,
              lastBox: '', paused: new Map(), shownAt: 0, unplayable: 0, lastMore: 0, moreTimer: 0, dry: 0, want: -1 };
  const keyOf = (x) => x.post || x.src || x.embed || '';
  const itemOf = (x, extra) => ({ src: x.src || '', kind: x.kind, embed: x.embed || '', el: x.el || null, card: null, post: '', poster: x.poster || '',
                                  title: x.title || '', state: x.src ? 'ready' : x.kind === 'blob' ? 'blob' : x.kind === 'embed' ? 'embed' : 'post', list: '', why: '',
                                  ...extra });
  function place(it) {
    // in the page's order (the layer's list follows the page), never before the one on screen
    const k = keyOf(it);
    if (!k || S.keys.has(k)) return false;
    S.keys.add(k);
    if (it.el && it.el.isConnected) {
      for (let i = Math.max(0, S.cur + 1); i < S.items.length; i++) {
        const o = S.items[i];
        if (o.el && o.el.isConnected && (it.el.compareDocumentPosition(o.el) & Node.DOCUMENT_POSITION_FOLLOWING)) { S.items.splice(i, 0, it); return true; }
      }
    }
    S.items.push(it);
    return true;
  }
  function scan() {
    // what the page has now (the page keeps loading under the layer, and X takes away what scrolled by: what was seen
    // stays in the list by its address)
    const found = mediaIn(document, location.href, true).filter((x) => !(x.el && x.el.closest && x.el.closest('pluma-scroller')));
    const pp = found.filter((x) => x.kind !== 'embed').length <= 2 ? posts(document, location.href) : null;
    let added = 0;
    for (const x of found) {
      if (x.kind === 'blob') matchNet(x);
      const it = itemOf(x);
      if (x.kind === 'blob' && !x.src) { it.key2 = 'blob:' + (x.poster || Math.random()); }
      if (pp) {
        const p = x.el ? pp.list.find((q) => q.card.contains(x.el)) : null;
        if (p) { it.post = p.href; it.card = p.card; if (!it.title) it.title = p.title; }
      }
      if (!it.title) it.title = cardTitle(it.el);
      if (!keyOf(it)) { if (S.keys.has(it.key2)) continue; S.keys.add(it.key2); S.items.push(it); added++; continue; }
      if (place(it)) added++;
    }
    if (pp) for (const p of pp.list) {
      if (S.keys.has(p.href)) continue;
      if (place({ src: '', kind: 'post', embed: '', el: p.a, card: p.card, post: p.href, poster: p.thumb, title: p.title, state: 'post', list: '', why: '' })) added++;
    }
    // the network's lists that no element of the page claims (a player in a frame of another site): at the end
    for (const n of S.net) {
      if (n.used || S.keys.has(n.url)) continue;
      if (n.kind === 'file' && n.size && n.size < 1500000) continue;
      if (place({ src: n.url, kind: n.kind, embed: '', el: null, card: null, post: '', poster: '', title: document.title || '', state: 'ready', list: '', why: '', net: true })) { n.used = true; added++; }
    }
    if (added) { render(); lookahead(); log('cola ' + S.items.length + ' (+' + added + ')'); }
    if (S.cur < 0 && S.items.length) go(0, 1);
    return added;
  }
  function cardTitle(el) {
    if (!el) return document.title || '';
    const c = el.closest('article, [role=article], li, figure, .post, .card') || el.parentElement;
    const h = c && c.querySelector('h1, h2, h3, figcaption, [data-testid=tweetText]');
    return (text(h) || el.getAttribute('aria-label') || el.getAttribute('title') || document.title || '').slice(0, 200);
  }
  // a video the page plays from a stream of its own: the network's list with the same id as its poster
  function matchNet(x) {
    const ids = tokens(x.poster || '');
    if (!ids.length) return false;
    let best = null;
    for (const n of S.net) {
      if (n.used) continue;
      if (tokens(n.url).some((t) => ids.includes(t)) && (!best || RANK[n.kind] > RANK[best.kind] || (n.kind === best.kind && n.url.length < best.url.length))) best = n;
    }
    if (!best) return false;
    for (const n of S.net) if (n.kind === best.kind && tokens(n.url).some((t) => ids.includes(t))) n.used = true;
    x.src = best.url; x.kind = best.kind; x.how = 'net';
    return true;
  }
  function netIn(list) {
    let fresh = false;
    for (const n of list || []) {
      if (!n || typeof n.url !== 'string' || S.netKeys.has(n.url.split('?')[0])) continue;
      const kind = kindOf(n.url, n.type || '') || (n.type && /^video\//.test(n.type) ? 'file' : '');
      if (!kind || kind === 'gif') continue;
      S.netKeys.add(n.url.split('?')[0]);
      S.net.push({ url: n.url, kind, size: +n.size || 0 });
      fresh = true;
    }
    if (!fresh) return;
    // items waiting for theirs
    for (const it of S.items) {
      if (it.state !== 'blob') continue;
      const x = { poster: it.poster };
      if (matchNet(x)) { it.src = x.src; it.kind = x.kind; it.state = 'ready'; S.keys.add(it.src); if (S.items[S.cur] === it) play(S.cur); }
    }
    if (S.open) scan();
  }

  // ---------------------------------------------------------------- publications: their page, read ahead
  function readPage(url) {
    return new Promise((resolve) => {
      const x = new XMLHttpRequest();
      x.open('GET', url);
      x.responseType = 'document';
      x.timeout = 15000;
      x.onload = () => resolve({ status: x.status, doc: x.response });
      x.onerror = x.ontimeout = () => resolve({ status: 0, doc: null });
      try { x.send(); } catch (e) { resolve({ status: 0, doc: null }); }
    });
  }
  async function resolve(it) {
    if (it.state !== 'post') return;
    it.state = 'resolving';
    S.resolving++;
    S.attempts++;
    const r = await readPage(it.post);
    S.resolving--;
    if (r.status === 429 || r.status === 403 || r.status === 503) {
      // the site asks to slow down: nothing more for a minute, and the person is told
      it.state = 'post';
      S.slow = Date.now() + 60000;
      toast(TX('La web pide ir más despacio: se sigue en un minuto'));
      log('web lenta ' + r.status);
      return;
    }
    const m = r.doc && r.status >= 200 && r.status < 300 ? mainMedia(r.doc, it.post) : null;
    if (m && m.src) {
      Object.assign(it, { src: m.src, kind: m.kind, state: 'ready', poster: it.poster || m.poster, title: it.title || m.title });
      if (S.keys.has(m.src) && S.items.some((o) => o !== it && o.src === m.src)) { it.state = 'skip'; it.why = 'repetido'; }
      else { S.keys.add(m.src); S.dry = 0; }
    } else if (m && m.embed) Object.assign(it, { embed: m.embed, kind: 'embed', state: 'embed', poster: it.poster || m.poster, title: it.title || m.title });
    else { it.state = 'skip'; it.why = 'sin vídeo'; }
    if (S.items[S.cur] === it) { if (it.state === 'skip') go(S.cur + S.dir, S.dir, true); else play(S.cur); }
    render();
    lookahead();
  }
  function lookahead() {
    // the next three that play, read ahead (two at a time, a dozen pages at most for them: most may be pictures)
    if (!S.open || Date.now() < S.slow) { if (S.open && Date.now() < S.slow) setTimeout(lookahead, S.slow - Date.now() + 50); return; }
    let ready = 0, tried = 0;
    for (let i = Math.max(0, S.cur); i < S.items.length && ready < 3 && tried < 12; i++) {
      const it = S.items[i];
      if (it.state === 'ready' || it.state === 'blob' || it.state === 'embed') { if (i > S.cur) ready++; continue; }
      if (it.state === 'resolving') { tried++; continue; }
      if (it.state !== 'post') continue;
      tried++;
      if (S.resolving >= 2) break;
      resolve(it);
    }
    preload();
    // the list is about to end: more from the page (phase 2)
    const left = S.items.slice(S.cur + 1).filter((x) => x.state !== 'skip').length;
    if (left < 5) more();
  }
  function preload() {
    // the next one: its poster now, and its list read by the page (lists are often signed for the page that asked)
    for (let i = S.cur + 1; i < S.items.length; i++) {
      const it = S.items[i];
      if (it.state === 'skip') continue;
      if (it.poster && !it.pre) { it.pre = new Image(); it.pre.src = it.poster; }
      if (it.state === 'ready' && it.kind === 'hls' && !it.list && !it.listAsked) readList(it);
      break;
    }
  }
  function readList(it) {
    it.listAsked = true;
    const c = new AbortController();
    const timer = setTimeout(() => c.abort(), 4000);
    return fetch(it.src, { credentials: 'include', cache: 'no-store', signal: c.signal }).then((r) => (r.ok ? r.text() : '')).then((t) => {
      clearTimeout(timer);
      if (t && t.startsWith('#EXTM3U') && t.length < 256 * 1024) it.list = t;
    }).catch(() => clearTimeout(timer));
  }
  // more of the list: the page's own next page (rel=next, "siguiente"), read like a publication; or the page itself,
  // scrolled to its end under the layer, which loads more by itself (infinite lists)
  // A page at a time, 1.5 s apart, and at most five pages in a row that bring nothing new to play (a list of pictures,
  // or the same videos again) until the person moves on: never a loop of requests against the site.
  async function more() {
    if (S.nextBusy || S.nextDone || S.dry >= 5) return;
    const wait = S.lastMore + 1500 - Date.now();
    if (wait > 0) { if (!S.moreTimer) S.moreTimer = setTimeout(() => { S.moreTimer = 0; if (S.open) lookahead(); }, wait); return; }
    S.lastMore = Date.now();
    S.dry++;
    S.nextBusy = true;
    const before = S.items.length;
    if (!S.next) S.next = nextPage(document, location.href);
    if (S.next && !S.nextDone) {
      const r = await readPage(S.next);
      if (r.doc && r.status >= 200 && r.status < 300) {
        const base = S.next;
        const pp = posts(r.doc, base), found = mediaIn(r.doc, base, false);
        for (const x of found) if (x.src) place(itemOf(x, { el: null, title: x.title || titleOf(r.doc) }));
        if (pp) for (const p of pp.list) place({ src: '', kind: 'post', embed: '', el: null, card: null, post: p.href, poster: p.thumb, title: p.title, state: 'post', list: '', why: '' });
        const n = nextPage(r.doc, base);
        S.next = n && n !== base ? n : '';
        if (!S.next) S.nextDone = true;
        log('página siguiente: ' + (S.items.length - before));
      } else if (r.status === 429 || r.status === 403) { S.slow = Date.now() + 60000; toast(TX('La web pide ir más despacio: se sigue en un minuto')); }
      else S.nextDone = true;
    } else {
      const se = document.scrollingElement || document.documentElement;
      se.scrollTo({ top: se.scrollHeight, behavior: 'instant' });
      await new Promise((r) => setTimeout(r, 1200));
      scan();
      if (S.items.length === before) S.endTries = (S.endTries || 0) + 1;
      if (S.endTries > 6) S.nextDone = true;
    }
    S.nextBusy = false;
    render();
    // the person is waiting at the end: what came, or (nothing yet) another try in a moment, or "no more"
    if (S.want >= 0) { const i = S.want; S.want = -1; go(i, S.dir, true); }
    else if (S.items.length > before) lookahead();
  }

  // ---------------------------------------------------------------- moving through it
  function go(i, dir, auto) {
    S.dir = dir || 1;
    if (!auto) S.dry = 0;
    S.want = -1;
    while (i >= 0 && i < S.items.length && S.items[i].state === 'skip') i += S.dir;
    if (i < 0) { bump(); return; }
    if (i >= S.items.length) {
      if (!S.nextDone && S.dry < 5) { S.want = i; more(); notice(TX('Buscando más vídeos…'), true); }
      else { notice(S.items.some((x) => x.state !== 'skip') ? TX('No hay más vídeos') : TX('No hay vídeos en esta página'), false); send({ t: 'hide', on: true }); }
      return;
    }
    S.cur = i;
    play(i);
    lookahead();
  }
  function play(i) {
    const it = S.items[i];
    if (!it) return;
    S.playing = it;
    it.seen = true;
    S.shownAt = Date.now();
    clearTimeout(S.waiting);
    render();
    // the page under the layer follows (its list loads more there, and Esc leaves the person at this one)
    const el = it.card || it.el;
    if (el && el.isConnected) try { el.scrollIntoView({ block: 'center', behavior: 'instant' }); } catch (e) {}
    if (it.state === 'ready') {
      notice(TX('Cargando…'), true);
      const start = () => {
        if (S.playing !== it) return;
        send({ t: 'play', src: it.src, hls: it.kind === 'hls', gif: it.kind === 'gif', list: it.list || '', title: it.title || document.title || '', author: host, box: box() });
      };
      if (it.kind === 'hls' && !it.list && !it.listAsked) readList(it).then(start); else start();
      return;
    }
    send({ t: 'hide', on: true });
    if (it.state === 'post' || it.state === 'resolving') { notice(TX('Cargando…'), true); if (it.state === 'post') resolve(it); return; }
    if (it.state === 'blob') {
      // a video the page plays itself: the page loads it (in sight under the layer) and its list shows in the network
      notice(TX('Cargando…'), true);
      if (it.el && it.el.isConnected) { try { it.el.muted = true; const p = it.el.play(); if (p) p.catch(() => {}); } catch (e) {} }
      S.waiting = setTimeout(() => { if (S.playing === it && it.state === 'blob') unplayable(it, TX('Este vídeo solo se ve en la página')); }, 5000);
      return;
    }
    if (it.state === 'embed') { unplayable(it, TX('Este vídeo solo se ve en la página')); return; }
  }
  function unplayable(it, why) {
    // said, and on to the next one a moment later (DRM, another site's player, a page without its address)
    it.why = why;
    S.unplayable++;
    notice(why, false);
    S.waiting = setTimeout(() => { if (S.playing === it) { it.state = 'skip'; go(S.cur + S.dir, S.dir, true); } }, 1600);
  }
  function step(dir) { if (S.open) go(S.cur + dir, dir); }
  function bump() { notice(TX('Es el primero'), false); setTimeout(() => { if (S.playing) notice('', false); }, 900); }

  // ---------------------------------------------------------------- Pluma's player tells
  function player(e) {
    if (!S.open || !e) return;
    if (e.ev === 'started') {
      notice('', false);
      if (e.aspect > 0.2 && e.aspect < 5) { S.aspect = e.aspect; layout(); }
      if (S.playing) S.playing.ms = e.ms;
    } else if (e.ev === 'ended') { if (!S.loop) go(S.cur + 1, 1, true); }
    else if (e.ev === 'failed') { const it = S.playing; if (it) { it.state = 'skip'; it.why = TX('Este vídeo no se pudo ver'); S.unplayable++; notice(it.why, false); setTimeout(() => { if (S.playing === it) go(S.cur + S.dir, S.dir, true); }, 900); } }
    else if (e.ev === 'state') { if (typeof e.muted === 'boolean') S.muted = e.muted; if (typeof e.loop === 'boolean') S.loop = e.loop; render(); }
  }

  // ---------------------------------------------------------------- the layer
  const H = document.createElement('pluma-scroller');
  const root = H.attachShadow({ mode: 'closed' });
  const sheet = new CSSStyleSheet();
  sheet.replaceSync(`
    :host { all: initial; }
    * { box-sizing: border-box; }
    .bg { position: fixed; inset: 0; background: #0b0b0c; color: #f2f2f3; font: 14px/1.4 "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif;
          user-select: none; overflow: hidden; touch-action: none; }
    .slot { position: absolute; background: #000; border-radius: 12px; overflow: hidden; }
    .slot img { position: absolute; inset: 0; width: 100%; height: 100%; object-fit: contain; filter: brightness(.82); }
    .msg { position: absolute; left: 0; right: 0; top: 50%; transform: translateY(-50%); text-align: center; padding: 0 24px; font-size: 15px;
           text-shadow: 0 1px 3px #000; }
    .msg.busy::before { content: ""; display: block; margin: 0 auto 12px; width: 28px; height: 28px; border-radius: 50%;
           border: 3px solid rgba(255,255,255,.25); border-top-color: #fff; animation: spin .8s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }
    .cap { position: absolute; overflow: hidden; }
    .cap .t { font-size: 15px; font-weight: 600; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
    .cap .s { margin-top: 2px; font-size: 12.5px; color: rgba(255,255,255,.62); }
    .rail { position: absolute; display: flex; flex-direction: column; gap: 12px; align-items: center; }
    button { all: unset; width: 48px; height: 48px; border-radius: 50%; background: rgba(255,255,255,.1); display: grid; place-items: center;
             cursor: pointer; transition: background .12s; }
    button:hover { background: rgba(255,255,255,.2); }
    button:focus-visible { outline: 2px solid #fff; outline-offset: 2px; }
    button.on { background: rgba(255,255,255,.92); }
    button.on svg { fill: #0b0b0c; }
    button[disabled] { opacity: .35; cursor: default; }
    svg { width: 24px; height: 24px; fill: #fff; }
    .count { font-size: 12.5px; color: rgba(255,255,255,.7); min-width: 48px; text-align: center; }
    .close { position: absolute; top: 14px; left: 14px; }
    .hint { position: absolute; left: 76px; top: 26px; font-size: 12.5px; line-height: 1.5; color: rgba(255,255,255,.55); transition: opacity .6s; }
    .toast { position: absolute; left: 50%; bottom: 22px; transform: translateX(-50%); background: rgba(255,255,255,.95); color: #111;
             border-radius: 10px; padding: 9px 14px; font-size: 13px; opacity: 0; transition: opacity .2s; pointer-events: none; max-width: 70vw; }
    .toast.on { opacity: 1; }
  `);
  root.adoptedStyleSheets = [sheet];
  const mk = (tag, cls, parent) => { const e = document.createElement(tag); if (cls) e.className = cls; if (parent) parent.append(e); return e; };
  const ICON = {
    up: 'M12 8.3 5.7 14.6l1.4 1.4L12 11.1l4.9 4.9 1.4-1.4z', down: 'M12 15.7 18.3 9.4 16.9 8 12 12.9 7.1 8 5.7 9.4z',
    x: 'M18.3 7.1 16.9 5.7 12 10.6 7.1 5.7 5.7 7.1l4.9 4.9-4.9 4.9 1.4 1.4 4.9-4.9 4.9 4.9 1.4-1.4-4.9-4.9z',
    vol: 'M3 9v6h4l5 5V4L7 9H3zm13.5 3A4.5 4.5 0 0 0 14 8v8a4.5 4.5 0 0 0 2.5-4zM14 3.2v2.1a7 7 0 0 1 0 13.4v2.1a9 9 0 0 0 0-17.6z',
    mute: 'M16.5 12A4.5 4.5 0 0 0 14 8v2.2l2.5 2.5V12zm2.5 0c0 .9-.2 1.8-.5 2.6l1.5 1.5A8.8 8.8 0 0 0 21 12a9 9 0 0 0-7-8.8v2.1a7 7 0 0 1 5 6.7zM4.3 3 3 4.3 7.7 9H3v6h4l5 5v-6.7l4.3 4.3a7 7 0 0 1-2.3 1.3v2.1a9 9 0 0 0 3.7-1.9L19.7 21 21 19.7l-9-9L4.3 3zM12 4 9.9 6.1 12 8.2V4z',
    loop: 'M7 7h10v3l4-4-4-4v3H5v6h2V7zm10 10H7v-3l-4 4 4 4v-3h12v-6h-2v4z',
  };
  const icon = (d) => { const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); s.setAttribute('viewBox', '0 0 24 24');
                        const p = document.createElementNS('http://www.w3.org/2000/svg', 'path'); p.setAttribute('d', d); s.append(p); return s; };
  const bg = mk('div', 'bg', root);
  const slot = mk('div', 'slot', bg), poster = mk('img', '', slot), msg = mk('div', 'msg', slot);
  poster.alt = '';
  const cap = mk('div', 'cap', bg), capT = mk('div', 't', cap), capS = mk('div', 's', cap);
  const rail = mk('div', 'rail', bg);
  const btn = (d, label, act, parent = rail) => { const b = mk('button', '', parent); b.append(icon(d)); b.title = label; b.setAttribute('aria-label', label);
                                                  b.addEventListener('click', (e) => { if (e.isTrusted) act(); }); return b; };
  const bUp = btn(ICON.up, TX('Anterior (↑)'), () => step(-1));
  const count = mk('div', 'count', rail);
  const bDown = btn(ICON.down, TX('Siguiente (↓)'), () => step(1));
  const bMute = btn(ICON.vol, TX('Silenciar (M)'), () => send({ t: 'cmd', c: 'mute' }));
  const bLoop = btn(ICON.loop, TX('Bucle'), () => send({ t: 'cmd', c: 'loop' }));
  const bClose = btn(ICON.x, TX('Salir (Esc)'), () => close(), bg);
  bClose.classList.add('close');
  const hint = mk('div', 'hint', bg);
  hint.textContent = TX('Rueda, ↑ ↓ o arrastra: otro vídeo · Espacio: pausa · M: sonido · Esc: salir');
  const tst = mk('div', 'toast', bg);
  let tstTimer = 0;
  function toast(s) { tst.textContent = s; tst.classList.add('on'); clearTimeout(tstTimer); tstTimer = setTimeout(() => tst.classList.remove('on'), 4500); }
  function notice(s, busy) { msg.textContent = s; msg.className = 'msg' + (busy && s ? ' busy' : ''); }

  function box() {
    const r = slot.getBoundingClientRect();
    return [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height), devicePixelRatio];
  }
  function layout() {
    // the slot: as tall as the window lets it, the video's shape (until it is known, a phone's), the buttons on its right
    const W = innerWidth, Hh = innerHeight, capH = 58, railW = 64;
    const a = S.aspect || (S.playing && S.playing.aspect) || 9 / 16;
    let h = Math.max(120, Hh - 28 - capH), w = h * a;
    const maxW = Math.max(160, W - 2 * (railW + 40));
    if (w > maxW) { w = maxW; h = w / a; }
    const x = Math.round((W - w) / 2), y = Math.round(Math.max(14, (Hh - capH - h) / 2));
    Object.assign(slot.style, { left: x + 'px', top: y + 'px', width: Math.round(w) + 'px', height: Math.round(h) + 'px' });
    hint.style.maxWidth = Math.max(0, x - 76 - 16) + 'px';
    hint.style.display = x - 76 - 16 < 120 ? 'none' : '';
    Object.assign(cap.style, { left: x + 'px', top: (y + Math.round(h) + 10) + 'px', width: Math.round(w) + 'px', height: (capH - 10) + 'px' });
    Object.assign(rail.style, { left: Math.min(W - railW + 8, x + Math.round(w) + 18) + 'px', top: Math.max(y, y + Math.round(h) - 4 * 60 - 26) + 'px' });
    const b = box(), k = b.join(',');
    if (k !== S.lastBox) { S.lastBox = k; send({ t: 'box', r: b.slice(0, 4), dpr: b[4] }); }
  }
  function render() {
    const it = S.playing;
    const total = S.items.filter((x) => x.state !== 'skip').length, at = S.items.slice(0, S.cur).filter((x) => x.state !== 'skip').length + 1;
    count.textContent = S.cur >= 0 ? at + ' / ' + total + (S.nextDone ? '' : '+') : '';
    bUp.disabled = S.cur <= 0;
    capT.textContent = it ? it.title || '' : '';
    capS.textContent = host + (S.unplayable ? ' · ' + S.unplayable + TX(' vídeos no se pueden ver aquí') : '');
    bMute.replaceChildren(icon(S.muted ? ICON.mute : ICON.vol));
    bMute.title = S.muted ? TX('Activar sonido (M)') : TX('Silenciar (M)');
    bLoop.classList.toggle('on', S.loop);
    const p = it && it.poster;
    if (p && poster.getAttribute('src') !== p) { poster.src = p; poster.style.display = ''; }
    if (!p) { poster.removeAttribute('src'); poster.style.display = 'none'; }
    if (it && !it.aspect && it.el && it.el.videoWidth) it.aspect = it.el.videoWidth / it.el.videoHeight;
    layout();
  }

  // ---------------------------------------------------------------- input: everything goes to the layer while it is open
  let wheelAt = 0, wheelLast = 0, wheelSum = 0;
  function wheel(dy) {
    // one notch, one video; a long swipe of a touchpad too (it waits for 450 ms and for the swipe to calm)
    const now = Date.now();
    if (now - wheelLast > 200) wheelSum = 0;
    wheelLast = now;
    wheelSum += dy;
    if (now - wheelAt < 450 || Math.abs(wheelSum) < 40) return;
    wheelAt = now; wheelSum = 0;
    step(dy > 0 ? 1 : -1);
  }
  const onKey = (e) => {
    if (!S.open || !e.isTrusted || e.ctrlKey || e.altKey || e.metaKey) return;
    const k = e.key;
    let used = true;
    if (k === 'ArrowDown' || k === 'PageDown' || k === 'j' || k === 'J') step(1);
    else if (k === 'ArrowUp' || k === 'PageUp' || k === 'k' || k === 'K') step(-1);
    else if (k === ' ' || k === 'Spacebar') send({ t: 'cmd', c: 'pause' });
    else if (k === 'm' || k === 'M') send({ t: 'cmd', c: 'mute' });
    else if (k === 'Escape') close();
    else used = false;
    if (used) { e.preventDefault(); e.stopImmediatePropagation(); }
  };
  const onWheel = (e) => { if (!e.isTrusted) return; e.preventDefault(); e.stopPropagation(); wheel(e.deltaY); };
  let dragY = null;
  bg.addEventListener('pointerdown', (e) => { if (e.isTrusted && (e.target === bg || e.target === slot || e.target === poster || e.target === msg)) dragY = e.clientY; });
  bg.addEventListener('pointerup', (e) => {
    if (!e.isTrusted || dragY === null) return;
    const dy = e.clientY - dragY; dragY = null;
    if (Math.abs(dy) > 60) step(dy < 0 ? 1 : -1);
  });
  bg.addEventListener('wheel', onWheel, { passive: false });
  // the page's own videos are quiet while the layer is open (and as they were, muted or not, once it closes)
  const quiet = (el) => {
    if (!(el instanceof HTMLMediaElement) || S.paused.has(el) || el === (S.playing && S.playing.el && S.playing.state === 'blob' ? S.playing.el : null)) return;
    S.paused.set(el, el.muted);
    try { el.muted = true; el.pause(); } catch (e) {}
  };
  const onPlay = (e) => {
    const el = e.target;
    if (!S.open || !(el instanceof HTMLMediaElement)) return;
    if (S.playing && S.playing.state === 'blob' && S.playing.el === el) { try { el.muted = true; } catch (x) {} return; }   // (loading its list)
    S.paused.delete(el);
    quiet(el);
  };
  // the layer stays: put back if the page takes it out, and on top if the page opens something in the top layer
  let mo = null, scanTimer = 0;
  function keep() {
    if (!S.open) return;
    if (!H.isConnected) (document.body || document.documentElement).append(H);
    try { if (!H.matches(':popover-open')) H.showPopover(); } catch (e) {}
  }
  function open() {
    if (S.open) return;
    S.open = true;
    H.setAttribute('popover', 'manual');
    for (const [k, v] of Object.entries({ position: 'fixed', inset: '0', width: '100vw', height: '100vh', margin: '0', padding: '0', border: '0',
                                          background: 'transparent', 'max-width': 'none', 'max-height': 'none', overflow: 'hidden', display: 'block',
                                          'z-index': '2147483647', opacity: '1', visibility: 'visible', transform: 'none', filter: 'none' }))
      H.style.setProperty(k, v, 'important');
    (document.body || document.documentElement).append(H);
    keep();
    for (const el of document.querySelectorAll('video, audio')) quiet(el);
    addEventListener('keydown', onKey, true);
    addEventListener('play', onPlay, true);
    addEventListener('resize', layout);
    mo = new MutationObserver((list) => {
      if (!H.isConnected) keep();
      if (list.every((m) => H.contains(m.target) || m.target === H)) return;
      clearTimeout(scanTimer);
      scanTimer = setTimeout(scan, 500);
    });
    mo.observe(document.documentElement, { childList: true, subtree: true });
    S.keepTimer = setInterval(keep, 1000);
    setTimeout(() => { hint.style.opacity = '0'; }, 5000);
    try { H.focus(); bDown.focus({ preventScroll: true }); } catch (e) {}
    layout();
    notice(TX('Buscando vídeos…'), true);
    netIn(CFG.net);
    scan();
    if (!S.items.length && nextPage(document, location.href)) more();
    if (!S.items.length) { setTimeout(() => { if (S.open && S.cur < 0 && S.items.length === 0) { notice(TX('No hay vídeos en esta página'), false); } }, 4000); }
  }
  function close() {
    if (!S.open) return;
    S.open = false;
    const it = S.playing, el = it && (it.card || it.el);
    clearInterval(S.keepTimer);
    clearTimeout(S.waiting);
    if (mo) mo.disconnect();
    removeEventListener('keydown', onKey, true);
    removeEventListener('play', onPlay, true);
    removeEventListener('resize', layout);
    try { H.hidePopover(); } catch (e) {}
    H.remove();
    for (const [m, muted] of S.paused) try { m.muted = muted; } catch (e) {}
    S.paused.clear();
    // back where the video is in the page
    if (el && el.isConnected) try { el.scrollIntoView({ block: 'center', behavior: 'instant' }); } catch (e) {}
    send({ t: 'close' });
  }
  G.__plumaScroller = { open, close, step, net: netIn, player, wheel, count: () => S.items.length };
  open();
  return 1;
})()
