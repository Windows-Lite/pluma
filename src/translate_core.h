// Pluma: the parts of the page translation that need no Windows (translate.cpp uses them; tools\traducir-test.cpp
// tests them): reading Google's answer back into the page's text nodes, and guessing a page's language locally.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cwctype>
#include <cwchar>
#include <cstdlib>

// &amp; &lt; &gt; &quot; &#39; &#NNN; &#xHH; &nbsp; -> the characters (anything else is left as it came)
inline std::wstring tr_decode_entities(const std::wstring &s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'&') { out += s[i]; continue; }
        size_t semi = s.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 10) { out += s[i]; continue; }
        std::wstring name = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        if (name == L"amp") cp = L'&';
        else if (name == L"lt") cp = L'<';
        else if (name == L"gt") cp = L'>';
        else if (name == L"quot") cp = L'"';
        else if (name == L"apos") cp = L'\'';
        else if (name == L"nbsp") cp = 0xA0;
        else if (name.size() > 1 && name[0] == L'#') {
            bool hex = name[1] == L'x' || name[1] == L'X';
            std::wstring digits = name.substr(hex ? 2 : 1);
            bool ok = !digits.empty();
            for (wchar_t c : digits) if (!(hex ? iswxdigit(c) : iswdigit(c))) ok = false;
            if (ok) cp = wcstoul(digits.c_str(), nullptr, hex ? 16 : 10);
            if (cp > 0x10FFFF) cp = 0;
        }
        if (!cp) { out += s[i]; continue; }
        if (cp >= 0x10000) {   // (UTF-16)
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else out += (wchar_t)cp;
        i = semi;
    }
    return out;
}

// One piece of the page went out as "<a i=0>Le </a><a i=1>chat</a> noir" (one marker per text node; a piece of one
// node goes without markers) and came back translated, the markers moved where the words went. This gives each node
// its text: what is inside its marker, plus the text that follows it outside any marker (Google leaves words between
// markers: "<a i=0>El</a> <a i=1>gato</a> está durmiendo."); text before the first marker goes to the first one found.
// A node whose marker is missing gets "" (its words went into another node's); no marker at all: everything to node 0.
// Other tags Google may add are dropped. Entities are decoded.
inline std::vector<std::wstring> tr_split_markers(const std::wstring &html, size_t n) {
    std::vector<std::wstring> seg(n);
    if (!n) return seg;
    std::vector<bool> seen(n, false);
    std::vector<long> stack;      // markers open
    long last = -1;               // the last marker that was open (text outside markers goes to it)
    std::wstring before;          // text before any marker
    auto lowc = [](wchar_t c) { return (wchar_t)towlower(c); };
    auto put = [&](const std::wstring &text) {
        if (text.empty()) return;
        long at = !stack.empty() ? stack.back() : last;
        if (at < 0) before += text; else seg[(size_t)at] += text;
    };
    size_t i = 0;
    std::wstring run;
    while (i < html.size()) {
        if (html[i] != L'<') { run += html[i++]; continue; }
        size_t close = html.find(L'>', i);
        if (close == std::wstring::npos) { run += html.substr(i); break; }
        std::wstring tag = html.substr(i + 1, close - i - 1);
        put(run); run.clear();
        i = close + 1;
        // </a>
        size_t k = 0;
        while (k < tag.size() && iswspace(tag[k])) k++;
        if (k < tag.size() && tag[k] == L'/') {
            k++;
            while (k < tag.size() && iswspace(tag[k])) k++;
            if (k < tag.size() && lowc(tag[k]) == L'a' && (k + 1 == tag.size() || iswspace(tag[k + 1]))) {
                if (!stack.empty()) { last = stack.back(); stack.pop_back(); }
            }
            continue;
        }
        // <a i=N> or <a i="N">
        if (k < tag.size() && lowc(tag[k]) == L'a' && k + 1 < tag.size() && iswspace(tag[k + 1])) {
            size_t p = k + 1;
            while (p < tag.size() && iswspace(tag[p])) p++;
            if (p < tag.size() && lowc(tag[p]) == L'i') {
                p++;
                while (p < tag.size() && iswspace(tag[p])) p++;
                if (p < tag.size() && tag[p] == L'=') {
                    p++;
                    while (p < tag.size() && (iswspace(tag[p]) || tag[p] == L'"' || tag[p] == L'\'')) p++;
                    size_t d = p;
                    while (d < tag.size() && iswdigit(tag[d])) d++;
                    if (d > p && d - p < 6) {
                        long idx = wcstol(tag.substr(p, d - p).c_str(), nullptr, 10);
                        if (idx >= 0 && (size_t)idx < n) { stack.push_back(idx); seen[(size_t)idx] = true; last = idx; continue; }
                    }
                }
            }
        }
        // any other tag: dropped
    }
    put(run);
    bool any = false;
    for (bool s : seen) any = any || s;
    if (!any) { seg.assign(n, L""); seg[0] = before; }
    else if (!before.empty()) {
        for (size_t j = 0; j < n; j++) if (seen[j]) { seg[j] = before + seg[j]; break; }
    }
    for (auto &s : seg) s = tr_decode_entities(s);
    return seg;
}

// The page's language from its own text, locally (nothing of the page goes anywhere to find out): the script of its
// letters (Japanese, Korean, Chinese, Cyrillic, Arabic...) and, for the Latin ones, the most common little words of
// each language. "" when it cannot tell. *strong: sure enough to overrule the page's own lang attribute.
inline std::wstring tr_guess_lang(const std::wstring &text, bool *strong = nullptr) {
    if (strong) *strong = false;
    long latin = 0, kana = 0, han = 0, hangul = 0, cyr = 0, ukr = 0, arabic = 0, hebrew = 0, greek = 0, thai = 0, deva = 0;
    for (wchar_t c : text) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7)) latin++;
        else if (c >= 0x3040 && c <= 0x30FF) kana++;
        else if (c >= 0x4E00 && c <= 0x9FFF) han++;
        else if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF)) hangul++;
        else if (c >= 0x0400 && c <= 0x04FF) { cyr++; if (c == 0x0456 || c == 0x0457 || c == 0x0454 || c == 0x0491) ukr++; }
        else if (c >= 0x0600 && c <= 0x06FF) arabic++;
        else if (c >= 0x0590 && c <= 0x05FF) hebrew++;
        else if (c >= 0x0370 && c <= 0x03FF) greek++;
        else if (c >= 0x0E00 && c <= 0x0E7F) thai++;
        else if (c >= 0x0900 && c <= 0x097F) deva++;
    }
    long cjk = kana + han;
    struct { long n; const wchar_t *code; } scripts[] = {
        { cjk, kana * 10 > cjk ? L"ja" : L"zh" }, { hangul, L"ko" }, { cyr, ukr * 50 > cyr ? L"uk" : L"ru" }, { arabic, L"ar" },
        { hebrew, L"he" }, { greek, L"el" }, { thai, L"th" }, { deva, L"hi" } };
    long best = 0; const wchar_t *best_code = nullptr;
    for (auto &s : scripts) if (s.n > best) { best = s.n; best_code = s.code; }
    // (a CJK letter carries a word: they weigh more than Latin ones)
    long best_w = best * (best_code && (best_code[0] == L'j' || best_code[0] == L'z' || best_code[0] == L'k') ? 3 : 1);
    if (best_code && best >= 12 && best_w > latin) { if (strong) *strong = best_w > latin * 2; return best_code; }
    if (latin < 60) return L"";
    static const struct { const wchar_t *code; const wchar_t *words; } kStop[] = {
        { L"en", L" the and of to is that with for this are was you have from not be on as it by at or an which will their has " },
        { L"es", L" el la los las de que y en un una es por con para del se no al como más pero su sus lo le está son también muy " },
        { L"fr", L" le la les des de et est un une du que qui dans pour pas sur au aux avec ce il elle sont en ne nous vous mais " },
        { L"de", L" der die das und ist nicht ein eine zu den mit von sich auf für dem des auch es ich sie wir aber oder wird " },
        { L"it", L" il lo la gli le di che è e un una per non del della con sono nel alla anche si ma più questo come " },
        { L"pt", L" o a os as de que e do da em um uma para com não é dos das no na por mais se mas foi ao são " },
        { L"nl", L" de het een en van is dat op te niet met zijn voor ook maar er aan om wordt bij " },
        { L"pl", L" i w nie na się to z że do jest jak o co ale od po tak za " },
    };
    const int L = (int)(sizeof(kStop) / sizeof(kStop[0]));
    std::vector<int> score(L, 0);
    std::wstring w;
    auto word = [&]() {
        if (w.empty() || w.size() > 12) { w.clear(); return; }
        std::wstring key = L" " + w + L" ";
        for (int k = 0; k < L; k++) if (wcsstr(kStop[k].words, key.c_str())) score[k]++;
        w.clear();
    };
    for (wchar_t c : text) {
        if (iswalpha(c) || (c >= 0xC0 && c <= 0x24F)) w += (wchar_t)towlower(c);
        else word();
    }
    word();
    int a = -1, b = -1;
    for (int k = 0; k < L; k++) {
        if (a < 0 || score[k] > score[a]) { b = a; a = k; }
        else if (b < 0 || score[k] > score[b]) b = k;
    }
    if (a < 0 || score[a] < 6) return L"";
    int second = b < 0 ? 0 : score[b];
    if (score[a] * 10 < second * 14) return L"";   // (less than 1.4 times the next one: not sure)
    if (strong) *strong = score[a] >= 15 && score[a] * 10 >= second * 25;
    return kStop[a].code;
}

// "fr-FR" -> "fr", "zh-Hant" -> "zh", " EN " -> "en"
inline std::wstring tr_primary(const std::wstring &lang) {
    std::wstring p;
    for (wchar_t c : lang) {
        if (c == L'-' || c == L'_') break;
        if (iswalpha(c)) p += (wchar_t)towlower(c);
    }
    if (p.size() < 2 || p.size() > 3 || p == L"und" || p == L"zxx" || p == L"mul") return L"";
    return p;
}
