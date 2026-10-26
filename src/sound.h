// Pluma: sound effects for the video player (mpv's "af" property): presets (bass boost, clear voices, nightcore,
// slowed + reverb, 8D, room...), a 10-band equalizer and a pitch of its own, independent of the speed.
// Plain C++ with no Windows in it, so tools\sonido-test.cpp can check the filter strings anywhere.
#pragma once
#include <string>

static const int kEqBands = 10;
static const int kEqFreq[kEqBands] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

// what a preset (or the person's own) does to the sound
struct SoundFx {
    int eq[kEqBands] = {};   // dB, -12..12
    int bass = 0;            // dB of the low shelf (100 Hz), -12..12
    int reverb = 0;          // 0..100
    double rate = 1;         // nightcore and slowed: faster or slower, and the pitch with it (as a record played faster)
    bool pan = false;        // 8D: the sound going slowly round the head
};
// the setting (ajustes.ini): the chosen preset, the person's own values (Personalizado) and the pitch
struct SoundSettings {
    std::wstring preset = L"no";   // no, graves, graves_fuertes, voces, agudos, nightcore, slowed, 8d, sala, personalizado
    SoundFx custom;
    int pitch = 0;                 // semitones, -6..6, with any preset
};
struct SoundPreset { const wchar_t *id, *name; SoundFx fx; };   // name: in Spanish, for T()
extern const SoundPreset kSoundPresets[];
extern const int kSoundPresetCount;

const SoundPreset *sound_preset(const std::wstring &id);   // null: unknown
SoundFx sound_fx(const SoundSettings &s);                   // what plays: the preset's, or the person's own
void sound_clamp(SoundSettings &s);
// mpv's "af" for it ("" = nothing: no filter at all, no CPU). rubberband: mpv's own pitch filter is there (else the
// pitch goes through lavfi's asetrate + atempo)
std::string sound_af(const SoundFx &fx, int pitch, bool rubberband);
std::wstring sound_eq_text(const int eq[kEqBands]);        // "0,3,-2,..." (ajustes.ini)
void sound_eq_parse(const std::wstring &s, int eq[kEqBands]);
