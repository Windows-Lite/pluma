// A small JSON reader, enough for extension manifests and the strings WebView2 hands back. Comments and trailing
// commas are allowed, as Chrome allows them in manifests.
#pragma once
#include <string>
#include <vector>
#include <utility>

struct Json {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<Json> arr;
    std::vector<std::pair<std::wstring, Json>> obj;
    const Json &operator[](const wchar_t *k) const {
        static const Json none;
        if (kind != OBJ) return none;
        for (auto &kv : obj) if (kv.first == k) return kv.second;
        return none;
    }
    bool is_str() const { return kind == STR; }
    bool is_obj() const { return kind == OBJ; }
};

class JsonReader {
    const std::wstring &t;
    size_t i = 0;
    void space() {
        while (i < t.size()) {
            if (iswspace(t[i])) { i++; continue; }
            if (t[i] == L'/' && i + 1 < t.size() && t[i + 1] == L'/') { while (i < t.size() && t[i] != L'\n') i++; continue; }
            if (t[i] == L'/' && i + 1 < t.size() && t[i + 1] == L'*') { size_t e = t.find(L"*/", i + 2); i = e == std::wstring::npos ? t.size() : e + 2; continue; }
            break;
        }
    }
    bool str(std::wstring &out) {
        if (i >= t.size() || t[i] != L'"') return false;
        i++;
        while (i < t.size() && t[i] != L'"') {
            wchar_t c = t[i++];
            if (c != L'\\') { out += c; continue; }
            if (i >= t.size()) return false;
            wchar_t e = t[i++];
            switch (e) {
            case L'n': out += L'\n'; break; case L't': out += L'\t'; break; case L'r': out += L'\r'; break;
            case L'b': out += L'\b'; break; case L'f': out += L'\f'; break;
            case L'u': {
                if (i + 4 > t.size()) return false;
                out += (wchar_t)wcstol(t.substr(i, 4).c_str(), NULL, 16);
                i += 4;
                break;
            }
            default: out += e;
            }
        }
        if (i >= t.size()) return false;
        i++;
        return true;
    }
public:
    bool ok = true;
    explicit JsonReader(const std::wstring &text) : t(text) {}
    Json value(int depth = 0) {
        Json v;
        space();
        if (i >= t.size() || depth > 64) { ok = false; return v; }
        wchar_t c = t[i];
        if (c == L'{') {
            v.kind = Json::OBJ; i++;
            while (true) {
                space();
                if (i < t.size() && t[i] == L'}') { i++; break; }
                std::wstring k;
                if (!str(k)) { ok = false; return v; }
                space();
                if (i >= t.size() || t[i] != L':') { ok = false; return v; }
                i++;
                v.obj.push_back({ k, value(depth + 1) });
                if (!ok) return v;
                space();
                if (i < t.size() && t[i] == L',') { i++; continue; }
                if (i < t.size() && t[i] == L'}') { i++; break; }
                ok = false; return v;
            }
        } else if (c == L'[') {
            v.kind = Json::ARR; i++;
            while (true) {
                space();
                if (i < t.size() && t[i] == L']') { i++; break; }
                v.arr.push_back(value(depth + 1));
                if (!ok) return v;
                space();
                if (i < t.size() && t[i] == L',') { i++; continue; }
                if (i < t.size() && t[i] == L']') { i++; break; }
                ok = false; return v;
            }
        } else if (c == L'"') {
            v.kind = Json::STR;
            if (!str(v.s)) ok = false;
        } else if (!t.compare(i, 4, L"true")) { v.kind = Json::BOOL; v.b = true; i += 4; }
        else if (!t.compare(i, 5, L"false")) { v.kind = Json::BOOL; i += 5; }
        else if (!t.compare(i, 4, L"null")) { i += 4; }
        else {
            wchar_t *end = NULL;
            v.n = wcstod(t.c_str() + i, &end);
            if (end == t.c_str() + i) { ok = false; return v; }
            v.kind = Json::NUM;
            i = end - t.c_str();
        }
        return v;
    }
};
inline Json json_parse(const std::wstring &text, bool *ok = nullptr) {
    JsonReader r(text);
    Json v = r.value();
    if (ok) *ok = r.ok;
    return v;
}
