// Pluma: checks how a translated piece goes back into the page's text nodes and the local language guess
// (src/translate_core.h): a slip there scrambles or loses a page's words.
//   c++ -std=c++17 -o traducir-test tools/traducir-test.cpp && ./traducir-test
#include "../src/translate_core.h"
#include <iostream>
#include <codecvt>
#include <locale>

static int fails = 0;
static std::string u8(const std::wstring &s) { return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(s); }
static void same(const std::vector<std::wstring> &got, const std::vector<std::wstring> &want, const char *what) {
    if (got == want) return;
    fails++;
    std::cout << "FALLA: " << what << "\n   got:";
    for (auto &s : got) std::cout << " [" << u8(s) << "]";
    std::cout << "\n  want:";
    for (auto &s : want) std::cout << " [" << u8(s) << "]";
    std::cout << "\n";
}
static void is(const std::wstring &got, const std::wstring &want, const char *what) {
    if (got == want) return;
    fails++;
    std::cout << "FALLA: " << what << ": [" << u8(got) << "] en vez de [" << u8(want) << "]\n";
}

int main() {
    // what Google answered (format=html), as seen with curl on 26/10/2026
    same(tr_split_markers(L"<a i=0>El</a> <a i=1>gato</a> <a i=2>negro</a> está durmiendo.", 3),
         { L"El ", L"gato ", L"negro está durmiendo." }, "texto entre marcas, al nodo anterior");
    same(tr_split_markers(L"<a i=\"1\">gato</a><a i=\"0\">El </a>", 2), { L"El ", L"gato" }, "marcas con comillas y en otro orden");
    same(tr_split_markers(L"Antes <a i=1>uno</a>", 2), { L"", L"Antes uno" }, "texto antes de la primera marca");
    same(tr_split_markers(L"<a i=0>uno</a>", 3), { L"uno", L"", L"" }, "marcas que faltan: vacías");
    same(tr_split_markers(L"sin marcas", 2), { L"sin marcas", L"" }, "ninguna marca: todo al primero");
    same(tr_split_markers(L"Tom &amp; Jerry &lt;b&gt; &#39;x&#39; &#x1F600;", 1), { L"Tom & Jerry <b> 'x' \U0001F600" }, "entidades");
    same(tr_split_markers(L"<a i=0>a <b>negrita</b></a><a i=7>fuera</a>", 1), { L"a negritafuera" }, "otras etiquetas e índices de más");
    same(tr_split_markers(L"<A I=0>x</A><a  i = 1 >y</a >", 2), { L"x", L"y" }, "mayúsculas y espacios");
    same(tr_split_markers(L"<a i=0>uno <a i=1>dos</a> tres</a> cuatro", 2), { L"uno  tres cuatro", L"dos" }, "marcas anidadas");
    same(tr_split_markers(L"a < b y c > d", 1), { L"a  d" }, "un < suelto no rompe nada");
    is(tr_decode_entities(L"&unknown; &amp &#99999999;"), L"&unknown; &amp &#99999999;", "entidades raras se quedan");

    // the local guess
    is(tr_guess_lang(L"Le chat noir dort sur le canapé pendant que la pluie tombe sur la ville. Il est dans la maison avec les enfants et ce n'est pas un problème pour nous."), L"fr", "francés");
    is(tr_guess_lang(L"El gato negro duerme en el sofá mientras la lluvia cae sobre la ciudad. Los niños están en la casa y no es un problema para nosotros."), L"es", "castellano");
    is(tr_guess_lang(L"The black cat sleeps on the sofa while the rain falls on the city. The children are in the house and that is not a problem for us."), L"en", "inglés");
    is(tr_guess_lang(L"Die schwarze Katze schläft auf dem Sofa, und der Regen fällt auf die Stadt. Die Kinder sind im Haus und das ist nicht ein Problem für uns."), L"de", "alemán");
    is(tr_guess_lang(L"黒い猫はソファで寝ています。雨が街に降っています。子供たちは家にいます。"), L"ja", "japonés");
    is(tr_guess_lang(L"黑猫在沙发上睡觉，雨落在城市上。孩子们在家里，这对我们来说不是问题。"), L"zh", "chino");
    is(tr_guess_lang(L"Чёрный кот спит на диване, пока дождь идёт над городом. Дети дома."), L"ru", "ruso");
    is(tr_guess_lang(L"OK 123 !!"), L"", "muy poco texto");
    bool strong = true;
    tr_guess_lang(L"黒い猫はソファで寝ています。雨が街に降っています。子供たちは家にいます。 YouTube Home", &strong);
    if (!strong) { fails++; std::cout << "FALLA: japonés con unas palabras en inglés debería ser seguro\n"; }

    is(tr_primary(L"fr-FR"), L"fr", "fr-FR"); is(tr_primary(L" EN "), L"en", "EN"); is(tr_primary(L"zh-Hant"), L"zh", "zh-Hant");
    is(tr_primary(L"und"), L"", "und"); is(tr_primary(L""), L"", "vacío");
    std::cout << (fails ? "con fallos: " + std::to_string(fails) : std::string("todo bien")) << "\n";
    return fails ? 1 : 0;
}
