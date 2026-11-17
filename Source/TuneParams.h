#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// =====================================================================
//  Vega Productions Autotune — parameters, scales, voice ranges
// =====================================================================
namespace vat
{
inline const juce::StringArray& keyNames()   { static juce::StringArray a { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; return a; }
inline const juce::StringArray& scaleNames() { static juce::StringArray a { "Chromatic", "Major", "Minor", "Harmonic Minor", "Melodic Minor", "Dorian", "Mixolydian",
                                                                           "Major Pentatonic", "Minor Pentatonic", "Blues", "Custom" }; return a; }
constexpr int kCustomScale = 10;
/** 12‑bit masks relative to the key (bit 0 = root). */
inline int scaleMask (int scale)
{
    static const int m[] { 0xFFF, 0b101010110101, 0b010110101101, 0b100110101101, 0b101010101101, 0b011010101101, 0b011010110101,
                           0b001010010101, 0b010010101001, 0b010011101001, 0xFFF };
    return m[juce::jlimit (0, 10, scale)];
}
/** Absolute (C = bit 0) mask of allowed pitch classes. */
inline int absoluteMask (int key, int scale, int customMask)
{
    if (scale == kCustomScale) return customMask & 0xFFF;
    const int rel = scaleMask (scale); int abs = 0;
    for (int i = 0; i < 12; ++i) if (rel & (1 << i)) abs |= 1 << ((i + key) % 12);
    return abs;
}

inline const juce::StringArray& rangeNames() { static juce::StringArray a { "Soprano", "Alto / Tenor", "Low Male", "Bass / Instrument" }; return a; }
inline constexpr float rangeMinHz[] { 170.0f, 100.0f, 75.0f, 55.0f };
inline constexpr float rangeMaxHz[] { 1200.0f, 900.0f, 650.0f, 450.0f };

inline juce::String midiName (float m)
{
    if (m < 0) return "--";
    const int n = juce::roundToInt (m);
    return keyNames()[(n % 12 + 12) % 12] + juce::String (n / 12 - 1);
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto flt = [&] (const String& id, const String& name, NormalisableRange<float> r, float def, std::function<String (float, int)> f)
    { l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def, AudioParameterFloatAttributes().withStringFromValueFunction (f))); };
    auto boo = [&] (const String& id, const String& name, bool def) { l.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def)); };
    auto cho = [&] (const String& id, const String& name, const StringArray& c, int def) { l.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, c, def)); };
    auto pct = [] (float v, int) { return String (roundToInt (v)) + "%"; };

    cho ("key", "Key", keyNames(), 0);
    cho ("scale", "Scale", scaleNames(), 2);
    for (int i = 0; i < 12; ++i) boo ("note" + String (i), "Note " + keyNames()[i], true);
    boo ("midiMode", "MIDI Notes", false);
    cho ("range", "Voice Range", rangeNames(), 1);
    cho ("latency", "Mode", StringArray { "Studio", "Live" }, 0);
    cho ("style", "Style", StringArray { "Natural", "Hard" }, 0);

    NormalisableRange<float> retune (0.0f, 400.0f); retune.setSkewForCentre (40.0f);
    flt ("retune", "Retune Speed", retune, 25.0f, [] (float v, int) { return v < 0.5f ? String ("0 ms") : String (roundToInt (v)) + " ms"; });
    flt ("humanize", "Humanize", { 0.0f, 100.0f, 0.1f }, 30.0f, pct);
    flt ("flex", "Flex-Tune", { 0.0f, 100.0f, 0.1f }, 20.0f, pct);
    flt ("vibKeep", "Natural Vibrato", { 0.0f, 100.0f, 0.1f }, 100.0f, pct);
    flt ("amount", "Amount", { 0.0f, 100.0f, 0.1f }, 100.0f, pct);

    flt ("vibDepth", "Add Vibrato", { 0.0f, 100.0f, 0.1f }, 0.0f, [] (float v, int) { return String (roundToInt (v)) + " ct"; });
    flt ("vibRate", "Vibrato Rate", { 2.0f, 9.0f, 0.01f }, 5.5f, [] (float v, int) { return String (v, 1) + " Hz"; });
    flt ("vibDelay", "Vibrato Onset", { 0.0f, 1500.0f, 1.0f }, 300.0f, [] (float v, int) { return String (roundToInt (v)) + " ms"; });

    flt ("transpose", "Transpose", { -12.0f, 12.0f, 1.0f }, 0.0f, [] (float v, int) { const int s = roundToInt (v); return (s > 0 ? "+" : "") + String (s) + " st"; });
    flt ("formant", "Formant", { -6.0f, 6.0f, 0.01f }, 0.0f, [] (float v, int) { return (v > 0.005f ? "+" : "") + String (v, 2) + " st"; });
    flt ("mix", "Mix", { 0.0f, 100.0f, 0.1f }, 100.0f, pct);
    flt ("output", "Output", { -24.0f, 12.0f, 0.01f }, 0.0f, [] (float v, int) { return String (v, 1) + " dB"; });
    boo ("graphAuto", "Graph: Auto-Correct Unedited", true);
    boo ("bypass", "Bypass", false);
    return l;
}
} // namespace vat
