// Pluma: "Use a strong password" on sign-up forms (Configuración > Privacidad y datos, `sugerir_contrasenas`).
// Edge's own needs Microsoft's sync, which the engine has not got, so Pluma brings it: passgen.js (RCDATA 220) goes
// into every page and frame of every view, with its words in the chosen language. The page draws the suggestion and
// fills the fields; the engine's own password manager saves it when the form is sent, as any typed password (in
// incognito nothing is kept, and the suggestion says so). No page talks to Pluma: the setting reaches the open pages
// through ExecuteScript with an event only Pluma knows the name of; new documents get the script with it written in.
#include "pluma.h"
#include <bcrypt.h>

static std::wstring g_key;   // the name of the switch event: random, per run

static const std::string &source() {
    static const std::string s = [] {
        HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(220), RT_RCDATA);
        HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
        return g ? std::string((const char *)LockResource(g), SizeofResource(g_inst, r)) : std::string();
    }();
    return s;
}
static const std::wstring &key() {
    if (g_key.empty()) {
        BYTE b[16] = {};
        BCryptGenRandom(NULL, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        wchar_t s[40] = L"";
        for (int i = 0; i < 16; i++) swprintf_s(s + i * 2, 3, L"%02x", b[i]);
        g_key = s;
    }
    return g_key;
}
static std::wstring script(bool incognito) {
    std::wstring src = wide(source());
    const wchar_t *a = L"/*PLUMA_CFG*/", *b = L"/*PLUMA_CFG_FIN*/";
    size_t i = src.find(a), j = src.find(b);
    if (i == std::wstring::npos || j == std::wstring::npos || j < i) return L"";
    auto bl = [](bool x) { return x ? L"true" : L"false"; };
    std::wstring cfg = L"{on:" + std::wstring(bl(g_set.suggest_passwords)) + L",incognito:" + bl(incognito) + L",key:" + json_escape(key()) +
                       L",test:false,use:" + json_escape(T(L"Usar contraseña segura")) + L",save:" + json_escape(T(L"Pluma la guarda para este sitio")) +
                       L",nosave:" + json_escape(T(L"En incógnito no se guarda: apúntala")) + L"}";
    return src.substr(0, i) + cfg + src.substr(j + wcslen(b));
}
static void add(Tab *t) {
    int id = t->id;
    std::wstring s = script(t->incognito);
    if (s.empty()) return;
    t->view->AddScriptToExecuteOnDocumentCreated(s.c_str(), Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
        [id](HRESULT hr, LPCWSTR sid) -> HRESULT { Tab *t = tab_by_id(id); if (t && SUCCEEDED(hr) && sid) t->passgen_id = sid; return S_OK; }).Get());
}
void passgen_attach(Tab *t) {
    t->passgen_id.clear();
    if (t->view) add(t);
}
void passgen_settings_changed() {
    // new documents get the script as it is now (setting, language); the open ones are switched on or off at once
    // (their frames, at their next page)
    std::wstring flip = L"dispatchEvent(new CustomEvent(" + json_escape(L"pluma-passgen-" + key()) + L",{detail:" +
                        (g_set.suggest_passwords ? L"true" : L"false") + L"}))";
    for (auto &t : g_tabs) {
        if (!t->view) continue;
        if (!t->passgen_id.empty()) t->view->RemoveScriptToExecuteOnDocumentCreated(t->passgen_id.c_str());
        t->passgen_id.clear();
        add(t.get());
        t->view->ExecuteScript(flip.c_str(), NULL);
    }
}
