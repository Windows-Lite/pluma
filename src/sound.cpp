// Pluma: the player's sound effects as mpv audio filters (see sound.h).
// The pitch goes through mpv's own rubberband filter, which then also does the speed (as scaletempo2 would): the
// pitch moves with no change of speed, and nightcore and slowed are the speed and the pitch moved together, like a
// record played faster or slower. The rest is one libavfilter graph: the equalizer (one "equalizer" peak per band
// that is not at 0), "bass" (a low shelf), "apulsator" (8D), "aecho" (the reverb) and "alimiter" at the end, so a
// boost never clips. Nothing at all when nothing is on: mpv then runs no filter of its own.
#include "sound.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cwchar>

static SoundFx fx_eq(std::initializer_list<int> eq, int bass = 0) {
    SoundFx f;
    int i = 0;
    for (int g : eq) if (i < kEqBands) f.eq[i++] = g;
    f.bass = bass;
    return f;
}
static SoundFx fx_bass(int bass, int low = 0, int low2 = 0) { SoundFx f; f.bass = bass; f.eq[0] = low; f.eq[1] = low2; return f; }
static SoundFx fx_rate(double rate, int reverb, int bass) { SoundFx f; f.rate = rate; f.reverb = reverb; f.bass = bass; return f; }
static SoundFx fx_room(int reverb, bool pan) { SoundFx f; f.reverb = reverb; f.pan = pan; return f; }

const SoundPreset kSoundPresets[] = {
    { L"no", L"Desactivado", SoundFx() },
    { L"graves", L"Más graves", fx_bass(6) },
    { L"graves_fuertes", L"Graves fuertes", fx_bass(10, 4, 2) },
    { L"voces", L"Voces claras", fx_eq({ -3, -2, -1, 0, 1, 3, 4, 3, 1, 0 }) },
    { L"agudos", L"Más agudos", fx_eq({ 0, 0, 0, 0, 0, 0, 1, 3, 5, 6 }) },
    { L"nightcore", L"Nightcore", fx_rate(1.25, 0, 0) },
    { L"slowed", L"Slowed + reverb", fx_rate(0.85, 45, 2) },
    { L"8d", L"8D", fx_room(20, true) },
    { L"sala", L"Sala", fx_room(60, false) },
    { L"personalizado", L"Personalizado", SoundFx() },
};
const int kSoundPresetCount = (int)(sizeof(kSoundPresets) / sizeof(kSoundPresets[0]));

const SoundPreset *sound_preset(const std::wstring &id) {
    for (int i = 0; i < kSoundPresetCount; i++) if (id == kSoundPresets[i].id) return &kSoundPresets[i];
    return nullptr;
}
static int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
void sound_clamp(SoundSettings &s) {
    if (!sound_preset(s.preset)) s.preset = L"no";
    for (int &g : s.custom.eq) g = clampi(g, -12, 12);
    s.custom.bass = clampi(s.custom.bass, -12, 12);
    s.custom.reverb = clampi(s.custom.reverb, 0, 100);
    s.custom.rate = 1;
    s.custom.pan = false;
    s.pitch = clampi(s.pitch, -6, 6);
}
SoundFx sound_fx(const SoundSettings &s) {
    if (s.preset == L"personalizado") return s.custom;
    const SoundPreset *p = sound_preset(s.preset);
    return p ? p->fx : SoundFx();
}

// (numbers always with a dot: printf's "C" locale, as Pluma never sets another)
static std::string num(const char *fmt, double v) { char b[48]; snprintf(b, sizeof(b), fmt, v); return b; }
std::string sound_af(const SoundFx &fx, int pitch, bool rubberband) {
    double scale = fx.rate * pow(2.0, clampi(pitch, -6, 6) / 12.0);
    bool shift = fabs(scale - 1) > 0.0005;
    std::string out, graph;
    auto add = [&](const std::string &f) { graph += (graph.empty() ? "" : ",") + f; };
    if (shift) {
        if (rubberband) out = "rubberband=pitch-scale=" + num("%.5f", scale) + ":engine=faster";
        // no rubberband: the pitch as a faster (slower) sample rate, and the tempo back to what it was
        else add("aresample=48000,asetrate=" + num("%.0f", 48000 * scale) + ",aresample=48000,atempo=" + num("%.5f", 1 / scale));
    }
    int boost = std::max(0, fx.bass);
    for (int i = 0; i < kEqBands; i++) boost = std::max(boost, fx.eq[i]);
    // half of the biggest boost taken off first: the limiter then only catches the peaks
    if (boost >= 3) add("volume=" + num("%.1f", -boost / 2.0) + "dB");
    for (int i = 0; i < kEqBands; i++)
        if (fx.eq[i]) add("equalizer=f=" + std::to_string(kEqFreq[i]) + ":t=o:w=1:g=" + std::to_string(clampi(fx.eq[i], -12, 12)));
    if (fx.bass) add("bass=g=" + std::to_string(clampi(fx.bass, -12, 12)) + ":f=100:w=0.6");
    if (fx.pan) add("aformat=channel_layouts=stereo,apulsator=hz=0.08:amount=0.8");
    if (fx.reverb > 0) {
        double k = clampi(fx.reverb, 0, 100) / 100.0;
        // its output gain so that the dry sound and every echo together can never pass full scale (aecho warns
        // otherwise), then half of what that took back: the limiter catches the rare peak
        double in = 1 - 0.3 * k, sum = in + (0.5 + 0.38 + 0.28 + 0.2) * k, out = floor(1000 / sum) / 1000;
        add("aecho=" + num("%.3f", in) + ":" + num("%.3f", out) + ":50|79|113|149:" + num("%.3f", 0.5 * k) + "|" + num("%.3f", 0.38 * k) + "|" +
            num("%.3f", 0.28 * k) + "|" + num("%.3f", 0.2 * k) + ",volume=" + num("%.1f", 10 * log10(sum)) + "dB");
    }
    if (boost > 0 || fx.pan || fx.reverb > 0) add("alimiter=limit=0.95:attack=5:release=50:level=0");
    // (in brackets: mpv's list would split the graph at its commas)
    if (!graph.empty()) out += (out.empty() ? "" : ",") + std::string("lavfi=[") + graph + "]";
    return out;
}
std::wstring sound_eq_text(const int eq[kEqBands]) {
    std::wstring s;
    for (int i = 0; i < kEqBands; i++) s += (i ? L"," : L"") + std::to_wstring(eq[i]);
    return s;
}
void sound_eq_parse(const std::wstring &s, int eq[kEqBands]) {
    size_t pos = 0;
    for (int i = 0; i < kEqBands; i++) {
        eq[i] = 0;
        if (pos > s.size()) continue;
        size_t c = s.find(L',', pos);
        std::wstring one = s.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos);
        eq[i] = clampi((int)wcstol(one.c_str(), nullptr, 10), -12, 12);
        pos = c == std::wstring::npos ? s.size() + 1 : c + 1;
    }
}
