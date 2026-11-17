#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "VegaUI.h"

class VegaTuneEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VegaTuneEditor (VegaTuneProcessor&);
    ~VegaTuneEditor() override;
    void paint (juce::Graphics&) override {}
    void resized() override;
    static constexpr int W = 1300, H = 720;
    class Main;
private:
    void timerCallback() override;
    VegaTuneProcessor& proc;
    VegaLookAndFeel lnf;
    std::unique_ptr<Main> main;
    juce::ComponentBoundsConstrainer constrainer;
};
