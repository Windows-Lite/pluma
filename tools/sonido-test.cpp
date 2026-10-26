// Pluma: checks the sound effects' filter strings (src/sound.cpp), the one place where a slip would leave the
// player mute or with the wrong sound.
//   c++ -std=c++17 -o sonido-test tools/sonido-test.cpp && ./sonido-test
//   ./sonido-test --grafos     prints each preset's libavfilter graph, one per line, to try with ffmpeg -af
#include "../src/sound.cpp"
#include <cstring>
#include <iostream>

static int fails = 0;
static void check(bool ok, const char *what, const std::string &got) {
    if (!ok) { fails++; std::cout << "FALLA: " << what << "\n   " << got << "\n"; }
}
static bool has(const std::string &s, const char *x) { return s.find(x) != std::string::npos; }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--grafos")) {
        // the lavfi part of each one, as ffmpeg takes it (mpv's rubberband is not ffmpeg's: the fallback pitch here)
        for (int i = 0; i < kSoundPresetCount; i++)
            for (int pitch : { 0, 3 }) {
                std::string af = sound_af(kSoundPresets[i].fx, pitch, false);
                size_t a = af.find("lavfi=[");
                if (a != std::string::npos) std::cout << af.substr(a + 7, af.size() - a - 8) << "\n";
            }
        return 0;
    }
    SoundSettings s;
    // off: nothing at all (no filter, no CPU)
    check(sound_af(sound_fx(s), 0, true).empty(), "Desactivado sin filtros", sound_af(sound_fx(s), 0, true));
    // every preset: the graph in brackets, nothing that ends mpv's quoting inside it, numbers with a dot
    for (int i = 0; i < kSoundPresetCount; i++) {
        std::string af = sound_af(kSoundPresets[i].fx, 2, true);
        size_t a = af.find("lavfi=[");
        if (a != std::string::npos) {
            std::string g = af.substr(a + 7);
            check(g.size() > 1 && g.back() == ']' && g.find(']') == g.size() - 1, "el grafo entre corchetes", af);
        }
        // (a digit, a comma, a digit is a decimal comma: no filter here starts with a digit)
        bool comma = false;
        for (size_t k = 1; k + 1 < af.size(); k++) comma |= af[k] == ',' && isdigit((unsigned char)af[k - 1]) && isdigit((unsigned char)af[k + 1]);
        check(!comma, "números con punto", af);
        check(!has(af, ",,") && af.find(',') != 0, "sin comas de más", af);
    }
    // the pitch alone: rubberband, nothing else
    std::string p = sound_af(SoundFx(), 6, true);
    check(p == "rubberband=pitch-scale=1.41421:engine=faster", "tono +6 con rubberband", p);
    p = sound_af(SoundFx(), -99, true);
    check(has(p, "pitch-scale=0.70711"), "tono fuera de rango, a -6", p);
    // no rubberband: lavfi's sample rate and tempo, the same length
    p = sound_af(SoundFx(), 6, false);
    check(p == "lavfi=[aresample=48000,asetrate=67882,aresample=48000,atempo=0.70711]", "tono sin rubberband", p);
    // nightcore: faster and higher together; with +2 semitones on top
    const SoundPreset *nc = sound_preset(L"nightcore");
    check(nc && sound_af(nc->fx, 0, true) == "rubberband=pitch-scale=1.25000:engine=faster", "nightcore", nc ? sound_af(nc->fx, 0, true) : "");
    check(nc && has(sound_af(nc->fx, 2, true), "pitch-scale=1.40308"), "nightcore +2", nc ? sound_af(nc->fx, 2, true) : "");
    // a boost: headroom first, the limiter last, without its auto level (it would make everything louder)
    p = sound_af(sound_preset(L"graves_fuertes")->fx, 0, true);
    check(p.rfind("lavfi=[volume=-5.0dB,", 0) == 0 && has(p, "bass=g=10:f=100") && has(p, "equalizer=f=31:t=o:w=1:g=4") &&
          p.size() > 2 && has(p.substr(p.rfind(',')), "alimiter=") && has(p, "level=0]"), "graves fuertes", p);
    // a cut alone does not need the limiter
    SoundFx cut; cut.eq[9] = -6;
    p = sound_af(cut, 0, true);
    check(p == "lavfi=[equalizer=f=16000:t=o:w=1:g=-6]", "solo un corte", p);
    // 8D: stereo first (a mono video would not pan), and the reverb
    p = sound_af(sound_preset(L"8d")->fx, 0, true);
    check(has(p, "aformat=channel_layouts=stereo,apulsator=") && has(p, "aecho=0.940:0.825:50|79|113|149:0.100|0.076|0.056|0.040,volume=0.8dB"), "8D", p);
    // Personalizado plays the person's own values; the others ignore them
    s.custom.eq[3] = 40; s.custom.bass = -20; s.custom.reverb = 300; s.preset = L"xx"; s.pitch = 9;
    sound_clamp(s);
    check(s.preset == L"no" && s.custom.eq[3] == 12 && s.custom.bass == -12 && s.custom.reverb == 100 && s.pitch == 6, "límites", "");
    s.preset = L"personalizado";
    p = sound_af(sound_fx(s), 0, true);
    check(has(p, "equalizer=f=250:t=o:w=1:g=12") && has(p, "bass=g=-12") && has(p, "aecho=0.700:"), "personalizado", p);
    s.preset = L"sala";
    check(!has(sound_af(sound_fx(s), 0, true), "equalizer"), "un preset no usa lo personalizado", sound_af(sound_fx(s), 0, true));
    // ajustes.ini's eq=
    int eq[kEqBands];
    sound_eq_parse(L"1,-2,3,,40,x", eq);
    check(eq[0] == 1 && eq[1] == -2 && eq[2] == 3 && eq[3] == 0 && eq[4] == 12 && eq[5] == 0 && eq[9] == 0, "eq= leído", "");
    check(sound_eq_text(eq) == L"1,-2,3,0,12,0,0,0,0,0", "eq= escrito", "");
    std::cout << (fails ? "con fallos: " : "todo bien: ") << fails << "\n";
    return fails ? 1 : 0;
}
