#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "TuneParams.h"
#include "PitchDSP.h"

class VegaTuneProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    VegaTuneProcessor();
    ~VegaTuneProcessor() override;
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Vega Productions Autotune"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter ("bypass"); }

    juce::AudioProcessorValueTreeState apvts;

    // ---- live read‑outs for the UI
    std::atomic<float> inMidi { -1 }, outMidi { -1 }, targetNote { -1 }, confidence { 0 };
    std::atomic<float> inPeak { 0 }, outPeak { 0 };
    std::atomic<int> latencyNow { 0 };
    struct HistoryPoint { float in, out; };
    juce::AbstractFifo historyFifo { 4096 }; std::vector<HistoryPoint> historyBuf = std::vector<HistoryPoint> (4096);

    // ---- Auto‑Key: pitch‑class histogram (duration weighted, slowly decaying)
    std::array<std::atomic<float>, 12> pcHist;
    void resetHistogram() { for (auto& h : pcHist) h.store (0.0f); }
    /** Krumhansl‑Schmuckler key finding on the histogram. returns key (0..11) and scale (1 major / 2 minor), or -1. */
    std::pair<int, int> detectKey (float& strength) const;

    // ---- Graph mode (timeline‑locked tracking + per‑tick edits, 100 ticks / s, 10 minutes)
    static constexpr int kTicksPerSecond = 100, kMaxTicks = 100 * 600;
    std::vector<std::atomic<float>> trackMidi;     // detected pitch (-1 = none)
    std::vector<std::atomic<float>> editShift;     // semitones to add (only if hasEdit)
    std::vector<std::atomic<bool>>  hasEdit;
    std::atomic<bool> tracking { false };
    std::atomic<double> playheadSeconds { -1.0 };
    std::atomic<bool> hostPlaying { false };
    std::atomic<int> uiMode { 0 };                  // 0 = Auto view, 1 = Graph view (graph edits apply in both)
    std::atomic<float> uiScale { 1.0f };
    void clearTrack();
    void clearEdits();

private:
    void timerCallback() override;
    void configure();
    void analyseHop (double hostSeconds, bool playing);
    float p (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    vat::Yin yin; vat::Psola psola;
    double fs = 48000; int hop = 128, curRange = -1, curLatencyMode = -1, channels = 2;
    std::vector<float> mono; int monoPos = 0; std::vector<float> window;
    int hopCounter = 0;
    // correction state
    float center = -1, corr = 0, curNote = -1, heldSeconds = 0, vibPhase = 0, noteSeconds = 0, lastMidi = -1;
    std::array<bool, 128> midiHeld {};
    std::array<std::atomic<float>*, 12> noteParams {};
    juce::SmoothedValue<float> outGain, mixS, bypassS;
    std::vector<float> dryRing[2]; int dryPos = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VegaTuneProcessor)
};
