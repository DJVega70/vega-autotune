// Accuracy tests on a synthetic "voice" (harmonic source through vowel formants, with vibrato).
#include "../Source/PluginProcessor.h"
#include <iostream>

static double FS = 48000; static int BLK = 256; static int fails = 0;
static void set (VegaTuneProcessor& p, const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); }
static void check (bool ok, const juce::String& msg) { std::cout << (ok ? "PASS " : "FAIL ") << msg << "\n"; if (! ok) ++fails; }

// voice: glottal‑like pulse train (harmonics 1/n^1.2) shaped by two vowel formants
static std::vector<float> makeVoice (float midi, float seconds, float vibCents = 0, float vibRate = 5.5f, float glideTo = -1)
{
    const int n = (int) (seconds * FS); std::vector<float> x ((size_t) n);
    double ph = 0; juce::Random r (1);
    // simple resonators for formants (~700 Hz and ~1200 Hz, "ah")
    auto reso = [] (double f, double bw) { const double R = std::exp (-juce::MathConstants<double>::pi * bw / FS), th = juce::MathConstants<double>::twoPi * f / FS;
                                          return std::array<double, 2> { 2 * R * std::cos (th), -R * R }; };
    auto r1 = reso (700, 110), r2 = reso (1220, 120); double a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / FS;
        double m = midi + vibCents * 0.01 * std::sin (juce::MathConstants<double>::twoPi * vibRate * t);
        if (glideTo > 0) m += (glideTo - midi) * std::min (1.0, t / seconds);
        const double f = 440.0 * std::pow (2.0, (m - 69) / 12.0);
        ph += f / FS; if (ph >= 1) ph -= 1;
        double s = 0; for (int h = 1; h <= 30 && h * f < FS * 0.45; ++h) s += std::sin (juce::MathConstants<double>::twoPi * h * ph) / std::pow (h, 1.2);
        s += 0.002 * (r.nextFloat() - 0.5f);
        const double y1 = s + r1[0] * a1 + r1[1] * a2; a2 = a1; a1 = y1;
        const double y2 = s + r2[0] * b1 + r2[1] * b2; b2 = b1; b1 = y2;
        x[(size_t) i] = (float) (0.02 * (y1 + 0.7 * y2) + 0.15 * s);
    }
    float pk = 0; for (auto v : x) pk = std::max (pk, std::abs (v)); for (auto& v : x) v *= 0.5f / pk;
    return x;
}

static std::vector<float> run (VegaTuneProcessor& p, const std::vector<float>& in)
{
    p.prepareToPlay (FS, BLK);
    std::vector<float> out (in.size());
    juce::AudioBuffer<float> b (2, BLK); juce::MidiBuffer m;
    for (size_t s = 0; s + BLK <= in.size(); s += BLK)
    {
        for (int i = 0; i < BLK; ++i) { b.setSample (0, i, in[s + (size_t) i]); b.setSample (1, i, in[s + (size_t) i]); }
        p.processBlock (b, m);
        for (int i = 0; i < BLK; ++i) out[s + (size_t) i] = b.getSample (0, i);
    }
    // undo latency
    const int lat = p.getLatencySamples();
    std::vector<float> aligned (out.size(), 0.0f);
    for (size_t i = 0; i + (size_t) lat < out.size(); ++i) aligned[i] = out[i + (size_t) lat];
    return aligned;
}

// median detected pitch (midi) over [t0, t1] using a fresh YIN
static float measure (const std::vector<float>& x, double t0, double t1, float* spreadCents = nullptr)
{
    vat::Yin y; y.prepare (FS, 60, 1200);
    std::vector<float> vals; const int W = y.samplesNeeded();
    for (int s = (int) (t0 * FS); s + W < (int) (t1 * FS); s += 256)
    {
        float conf; const float per = y.detect (x.data() + s, conf);
        if (per > 0 && conf > 0.5f) vals.push_back (69.0f + 12.0f * std::log2 ((float) FS / per / 440.0f));
    }
    if (vals.empty()) return -1;
    std::sort (vals.begin(), vals.end());
    if (spreadCents) *spreadCents = (vals[vals.size() * 9 / 10] - vals[vals.size() / 10]) * 100.0f;
    return vals[vals.size() / 2];
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    std::unique_ptr<juce::AudioProcessor> base (createPluginFilter());
    auto& p = dynamic_cast<VegaTuneProcessor&> (*base);
    p.setPlayConfigDetails (2, 2, FS, BLK);
    std::cout << "latency Studio/Alto-Tenor: " << (p.prepareToPlay (FS, BLK), p.getLatencySamples()) << " samples\n";

    // 1. detector accuracy across registers
    for (float midi : { 40.0f, 48.0f, 57.0f, 64.0f, 72.0f, 81.0f })
    {
        auto v = makeVoice (midi, 0.6f);
        const float m = measure (v, 0.1, 0.55);
        check (std::abs (m - midi) < 0.05f, "YIN detects " + vat::midiName (midi) + " within 5 cents (" + juce::String ((m - midi) * 100, 1) + " ct)");
    }

    set (p, "scale", 0);   // chromatic
    set (p, "range", 3);   // widest range for the low tests

    // 2. hard correction: 35 cents sharp A3 -> A3
    set (p, "retune", 0); set (p, "humanize", 0); set (p, "flex", 0); set (p, "vibKeep", 0);
    {
        auto out = run (p, makeVoice (57.35f, 1.5f));
        const float m = measure (out, 0.5, 1.4);
        check (std::abs (m - 57.0f) < 0.06f, "hard tune: A3 +35 ct corrected to " + juce::String ((m - 57.0f) * 100, 1) + " ct");
    }
    // 3. transparency: amount 0 must leave pitch alone
    set (p, "amount", 0);
    {
        auto out = run (p, makeVoice (57.35f, 1.5f));
        const float m = measure (out, 0.5, 1.4);
        check (std::abs (m - 57.35f) < 0.06f, "amount 0: pitch untouched (" + juce::String ((m - 57.35f) * 100, 1) + " ct off)");
    }
    set (p, "amount", 100);
    // 4. natural vibrato preserved while the centre is corrected
    set (p, "retune", 40); set (p, "vibKeep", 100);
    {
        auto out = run (p, makeVoice (60.3f, 2.0f, 40.0f));
        float spread = 0; const float m = measure (out, 0.8, 1.9, &spread);
        check (std::abs (m - 60.0f) < 0.12f && spread > 40.0f, "vibrato kept: centre " + juce::String ((m - 60) * 100, 1) + " ct, vibrato span " + juce::String (spread, 0) + " ct");
    }
    // 5. scale snapping: A#3 in C major -> A3 or B3
    set (p, "scale", 1); set (p, "key", 0); set (p, "retune", 0); set (p, "vibKeep", 0);
    {
        auto out = run (p, makeVoice (58.2f, 1.5f));
        const float m = measure (out, 0.5, 1.4);
        check (std::abs (m - 59.0f) < 0.08f || std::abs (m - 57.0f) < 0.08f, "C major snap: A#3+20ct -> " + vat::midiName (m) + " (" + juce::String (m, 2) + ")");
    }
    // 6. transpose with formants kept
    set (p, "scale", 0); set (p, "transpose", 5);
    {
        auto out = run (p, makeVoice (55.0f, 1.5f));
        const float m = measure (out, 0.5, 1.4);
        check (std::abs (m - 60.0f) < 0.08f, "transpose +5: G3 -> " + vat::midiName (m) + " (" + juce::String ((m - 60) * 100, 1) + " ct)");
    }
    set (p, "transpose", 0);
    // 7. Live mode latency + still tunes
    set (p, "latency", 1); set (p, "range", 0);
    {
        auto out = run (p, makeVoice (69.4f, 1.5f));
        const float m = measure (out, 0.5, 1.4);
        std::cout << "  Live/Soprano latency " << p.getLatencySamples() << " samples = " << p.getLatencySamples() * 1000.0 / FS << " ms\n";
        check (std::abs (m - 69.0f) < 0.08f, "live mode: A4+40ct -> " + juce::String ((m - 69) * 100, 1) + " ct");
    }
    // 8. level sanity (no blow‑ups / dropouts) with formant shift
    set (p, "formant", 4.0f);
    {
        auto in = makeVoice (64.0f, 1.5f); auto out = run (p, in);
        double ei = 0, eo = 0; bool finite = true;
        for (size_t i = (size_t) (0.4 * FS); i < (size_t) (1.4 * FS); ++i) { ei += in[i] * in[i]; eo += out[i] * out[i]; finite &= std::isfinite (out[i]); }
        const double ratioDb = 10 * std::log10 (eo / ei);
        check (finite && std::abs (ratioDb) < 4.0, "formant +4 st keeps level (" + juce::String (ratioDb, 1) + " dB)");
    }
    // 9. Graph mode: track along a timeline, edit one note +2 semitones, replay
    {
        struct Head : juce::AudioPlayHead
        {
            double t = 0;
            juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setTimeInSeconds (t); i.setIsPlaying (true); return i; }
        } head;
        set (p, "latency", 0); set (p, "range", 1); set (p, "formant", 0); set (p, "scale", 0);
        set (p, "amount", 0);                                           // auto-correction off: only the edit should act
        p.setPlayHead (&head);
        auto voice = makeVoice (57.0f, 2.0f);
        auto play = [&] ()
        {
            p.prepareToPlay (FS, BLK);
            std::vector<float> out (voice.size()); juce::AudioBuffer<float> b (2, BLK); juce::MidiBuffer mm;
            for (size_t s = 0; s + BLK <= voice.size(); s += BLK)
            {
                head.t = (double) s / FS;
                for (int i = 0; i < BLK; ++i) { b.setSample (0, i, voice[s + (size_t) i]); b.setSample (1, i, voice[s + (size_t) i]); }
                p.processBlock (b, mm);
                for (int i = 0; i < BLK; ++i) out[s + (size_t) i] = b.getSample (0, i);
            }
            const int lat = p.getLatencySamples(); std::vector<float> al (out.size(), 0.0f);
            for (size_t i = 0; i + (size_t) lat < out.size(); ++i) al[i] = out[i + (size_t) lat];
            return al;
        };
        p.clearTrack(); p.clearEdits(); p.tracking.store (true);
        play();
        p.tracking.store (false);
        int tracked = 0; for (int i = 0; i < 200; ++i) if (p.trackMidi[(size_t) i].load() > 0) ++tracked;
        check (tracked > 150, "graph tracking recorded " + juce::String (tracked) + " of 200 ticks");
        for (int i = 50; i < 150; ++i) { p.editShift[(size_t) i].store (2.0f); p.hasEdit[(size_t) i].store (true); }   // 0.5 s .. 1.5 s
        auto out = play();
        const float edited = measure (out, 0.7, 1.3), untouched = measure (out, 1.65, 1.95);
        check (std::abs (edited - 59.0f) < 0.1f && std::abs (untouched - 57.0f) < 0.1f,
               "graph edit: edited region " + juce::String (edited, 2) + " (want 59), rest " + juce::String (untouched, 2) + " (want 57)");
        p.setPlayHead (nullptr); set (p, "amount", 100);
    }
    std::cout << (fails == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string (fails)) << std::endl;
    return fails;
}
