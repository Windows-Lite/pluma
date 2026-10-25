// Pluma: userscripts (the .user.js files Violentmonkey and Tampermonkey run), without an extension. Each enabled
// script goes into every page as its own injected script, so a broken one cannot stop the others; it checks its
// @match/@include/@exclude against the page and runs at its @run-at. Also Pluma's own base script.
#include "pluma.h"
#include "json.h"

std::vector<UserScript> g_scripts;
std::wstring scripts_folder() {
    std::wstring f = g_state + L"\\scripts";
    CreateDirectoryW(f.c_str(), NULL);
    return f;
}
static std::wstring ini() { return g_state + L"\\ajustes.ini"; }

static bool parse(const std::string &src, UserScript &s) {
    std::wstring w = wide(src);
    size_t a = w.find(L"==UserScript=="), b = w.find(L"==/UserScript==");
    if (a == std::wstring::npos || b == std::wstring::npos || b < a) return false;
    std::wstring head = w.substr(a, b - a);
    size_t pos = 0;
    while (pos < head.size()) {
        size_t nl = head.find(L'\n', pos);
        std::wstring line = trim(head.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? head.size() : nl + 1;
        if (starts_with(line, L"//")) line = trim(line.substr(2));
        if (line.empty() || line[0] != L'@') continue;
        size_t sp = line.find_first_of(L" \t");
        std::wstring key = line.substr(1, sp == std::wstring::npos ? std::wstring::npos : sp - 1);
        std::wstring val = sp == std::wstring::npos ? L"" : trim(line.substr(sp));
        if (key == L"name" && s.name.empty()) s.name = val;
        else if (key == L"version") s.version = val;
        else if (key == L"description" && s.description.empty()) s.description = val;
        else if (key == L"match") s.match.push_back(val);
        else if (key == L"include") s.include.push_back(val);
        else if (key == L"exclude" || key == L"exclude-match") s.exclude.push_back(key == L"exclude-match" ? L"m:" + val : val);
        else if (key == L"run-at") s.run_at = val;
        else if (key == L"noframes") s.noframes = true;
    }
    s.code = src;
    return true;
}
void scripts_load() {
    g_scripts.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((scripts_folder() + L"\\*.user.js").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string src;
        UserScript s;
        s.file = fd.cFileName;
        if (!read_file(scripts_folder() + L"\\" + s.file, src) || !parse(src, s)) continue;
        if (s.name.empty()) s.name = s.file;
        wchar_t on[8];
        GetPrivateProfileStringW(L"scripts", s.file.c_str(), L"1", on, 8, ini().c_str());
        s.enabled = wcscmp(on, L"0") != 0;
        g_scripts.push_back(s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_scripts.begin(), g_scripts.end(), [](const UserScript &a, const UserScript &b) { return lower(a.name) < lower(b.name); });
}
void script_toggle(size_t i) {
    if (i >= g_scripts.size()) return;
    g_scripts[i].enabled = !g_scripts[i].enabled;
    WritePrivateProfileStringW(L"scripts", g_scripts[i].file.c_str(), g_scripts[i].enabled ? L"1" : L"0", ini().c_str());
}
bool script_install(const std::wstring &json_text, std::wstring &name_out) {
    // the page's text comes as a JSON string from ExecuteScript
    bool ok = false;
    Json j = json_parse(json_text, &ok);
    if (!ok || !j.is_str()) return false;
    std::string src = utf8(j.s);
    UserScript s;
    if (!parse(src, s) || (s.match.empty() && s.include.empty())) return false;
    std::wstring base = s.name.empty() ? L"script" : s.name;
    for (auto &c : base) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'-';
    base = trim(base);
    if (base.size() > 80) base.resize(80);
    std::wstring file = base + L".user.js";
    if (!write_file(scripts_folder() + L"\\" + file, src)) return false;
    WritePrivateProfileStringW(L"scripts", file.c_str(), L"1", ini().c_str());
    name_out = s.name.empty() ? file : s.name;
    scripts_load();
    return true;
}

// what every script carries: matching, @run-at and the GM_* functions
static const wchar_t *kPrelude = LR"JS(
const __m = (p) => {
  if (p === '<all_urls>') return /^(https?|file|ftp):/;
  const m = /^(\*|https?|file|ftp):\/\/([^/]*)(\/.*)$/.exec(p);
  if (!m) return null;
  const esc = (s) => s.replace(/[.+?^${}()|[\]\\]/g, '\\$&');
  const sc = m[1] === '*' ? 'https?' : m[1];
  const h = m[2] === '*' ? '[^/]*' : m[2].startsWith('*.') ? '(?:[^/]*\\.)?' + esc(m[2].slice(2)) : esc(m[2]);
  return new RegExp('^' + sc + ':\\/\\/' + h + esc(m[3]).replace(/\*/g, '.*') + '$');
};
const __g = (p) => {
  if (p.length > 2 && p[0] === '/' && p.endsWith('/')) return new RegExp(p.slice(1, -1));
  return new RegExp('^' + p.replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*') + '$');
};
const __t = (list, href) => list.some((p) => {
  try { const r = p.startsWith('m:') ? __m(p.slice(2)) : __g(p); return !!r && r.test(href); }
  catch (e) { return false; }
});
const __ok = (s) => {
  const href = location.href;
  if (s.noframes && window !== window.top) return false;
  if (__t(s.exclude, href)) return false;
  return __t(s.match.map((p) => 'm:' + p), href) || __t(s.include, href);
};
const __when = (s, fn) => {
  const run = () => { try { fn(); } catch (e) { console.error('[Pluma] ' + s.name + ':', e); } };
  if (s.runAt === 'document-start') run();
  else if (s.runAt === 'document-end') { if (document.readyState !== 'loading') run(); else document.addEventListener('DOMContentLoaded', run, { once: true }); }
  else { const go = () => setTimeout(run, 0); if (document.readyState !== 'loading') go(); else document.addEventListener('DOMContentLoaded', go, { once: true }); }
};
const __gm = (s) => {
  const key = '__pluma_gm__' + s.name + '__';
  const load = () => { try { return JSON.parse(localStorage.getItem(key) || '{}'); } catch (e) { return {}; } };
  const save = (o) => { try { localStorage.setItem(key, JSON.stringify(o)); } catch (e) {} };
  const info = { script: { name: s.name, version: s.version, description: s.description }, scriptHandler: 'Pluma', version: '1' };
  const get = (k, d) => { const o = load(); return k in o ? o[k] : d; };
  const set = (k, v) => { const o = load(); o[k] = v; save(o); };
  const del = (k) => { const o = load(); delete o[k]; save(o); };
  const list = () => Object.keys(load());
  const style = (css) => { const e = document.createElement('style'); e.textContent = css; (document.head || document.documentElement).appendChild(e); return e; };
  const open = (url, o) => window.open(url, o === true || (o && o.active === false) ? '__pluma_bg' : '_blank', 'noopener');
  const clip = (t) => { if (navigator.clipboard) navigator.clipboard.writeText(String(t)).catch(() => {}); };
  const notify = (d) => console.info('[Pluma] ' + s.name + ':', typeof d === 'string' ? d : d && d.text);
  const xhr = (d) => {
    const c = new AbortController();
    fetch(d.url, { method: d.method || 'GET', headers: d.headers, body: d.data, signal: c.signal, credentials: d.anonymous ? 'omit' : 'include' })
      .then(async (r) => {
        const text = await r.text();
        let response = text;
        if (d.responseType === 'json') { try { response = JSON.parse(text); } catch (e) { response = null; } }
        const res = { status: r.status, statusText: r.statusText, responseText: text, response, finalUrl: r.url, readyState: 4,
                      responseHeaders: [...r.headers].map(([k, v]) => k + ': ' + v).join('\r\n') };
        d.onload && d.onload(res); d.onloadend && d.onloadend(res);
      }, (e) => { d.onerror && d.onerror(e); d.onloadend && d.onloadend(e); });
    return { abort: () => c.abort() };
  };
  const GM = { info, getValue: async (k, d) => get(k, d), setValue: async (k, v) => set(k, v), deleteValue: async (k) => del(k),
               listValues: async () => list(), addStyle: style, openInTab: open, setClipboard: clip, notification: notify, xmlHttpRequest: xhr };
  return [info, GM, style, get, set, del, list, open, clip, notify, xhr];
};
)JS";

static std::wstring js_list(const std::vector<std::wstring> &v) {
    std::wstring o = L"[";
    for (size_t i = 0; i < v.size(); i++) o += (i ? L"," : L"") + json_escape(v[i]);
    return o + L"]";
}
std::vector<std::wstring> scripts_sources() {
    std::vector<std::wstring> out;
    for (auto &s : g_scripts) {
        if (!s.enabled) continue;
        std::wstring meta = L"{name:" + json_escape(s.name) + L",version:" + json_escape(s.version) + L",description:" + json_escape(s.description) +
                            L",match:" + js_list(s.match) + L",include:" + js_list(s.include) + L",exclude:" + js_list(s.exclude) +
                            L",runAt:" + json_escape(s.run_at) + L",noframes:" + (s.noframes ? L"true" : L"false") + L"}";
        std::wstring src = L"(() => {\n" + std::wstring(kPrelude) + L"const __s = " + meta + L";\nif (!__ok(__s)) return;\nconst __a = __gm(__s);\n"
                           L"__when(__s, () => (function (GM_info, GM, GM_addStyle, GM_getValue, GM_setValue, GM_deleteValue, GM_listValues, "
                           L"GM_openInTab, GM_setClipboard, GM_notification, GM_xmlhttpRequest, unsafeWindow) {\n" +
                           wide(s.code) + L"\n}).apply(window, __a.concat([window])));\n})();\n";
        out.push_back(src);
        if (g_dev) write_file(g_state + L"\\inyectado-" + s.file, utf8(src));   // for tests: exactly what goes into the pages
    }
    return out;
}

// Pluma's own lines in every page.
// 1) Ctrl+click and the middle button on a link open it in a tab behind: the page marks the new window with a name
//    that Pluma recognises (it cannot see the mouse inside the page otherwise).
// 2) Prefetch on hover: with the mouse resting on a link of the same site for a moment, the next page is fetched
//    before the click (Chromium's speculation rules; logouts and the like are left alone).
const std::wstring &base_script() {
    static const std::wstring s = LR"JS((() => {
  if (window.__plumaBase) return;
  Object.defineProperty(window, '__plumaBase', { value: true });
  const bg = (e) => {
    if (e.defaultPrevented || !e.isTrusted) return;
    if (!(e.button === 1 || (e.button === 0 && (e.ctrlKey || e.metaKey) && !e.shiftKey && !e.altKey))) return;
    const path = e.composedPath ? e.composedPath() : [];
    const a = path.find((n) => n && n.tagName === 'A' && n.href) || (e.target && e.target.closest && e.target.closest('a[href]'));
    if (!a || !/^https?:/i.test(a.href) || a.hasAttribute('download')) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    window.open(a.href, '__pluma_bg', 'noopener');
  };
  window.addEventListener('click', bg);
  window.addEventListener('auxclick', bg);
  if (window !== window.top) return;
  const rules = () => {
    try {
      if (!HTMLScriptElement.supports || !HTMLScriptElement.supports('speculationrules')) return;
      if (document.querySelector('script[type="speculationrules"]')) return;
      const s = document.createElement('script');
      s.type = 'speculationrules';
      s.textContent = JSON.stringify({ prefetch: [{ eagerness: 'moderate', where: { and: [
        { href_matches: '/*' },
        { not: { href_matches: { pathname: '*(logout|log-out|signout|sign-out|logoff|cerrar-sesion|salir|delete|remove|unsubscribe)*' } } },
        { not: { selector_matches: '[download], [rel~="nofollow"], [target="_blank"]' } } ] } }] });
      (document.head || document.documentElement).appendChild(s);
    } catch (e) {}
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', rules, { once: true }); else rules();
})();)JS";
    return s;
}
