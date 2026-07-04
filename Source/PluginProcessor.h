#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "FuzzDSP.h"

//==============================================================================
class DreamFuzzProcessor : public juce::AudioProcessor
{
public:
    DreamFuzzProcessor();
    ~DreamFuzzProcessor() override = default;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    const juce::String getName() const override { return "Dream Fuzz"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }

    //==========================================================================
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    /** Replaces the DSP voicing constants. Offline tooling only (not
        real-time safe): call before prepareToPlay(). */
    void setTuning (const df::Tuning& t) noexcept { tuning = t; }

    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;

    static constexpr const char* fuzzId   = "fuzz";
    static constexpr const char* toneId   = "tone";
    static constexpr const char* levelId  = "level";
    static constexpr const char* bypassId = "bypass";

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::atomic<float>* fuzzParamValue   = nullptr;
    std::atomic<float>* toneParamValue   = nullptr;
    std::atomic<float>* levelParamValue  = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    df::Tuning    tuning;
    df::FuzzCore  core[2];
    df::BaseChain base[2];

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay { 8192 };

    juce::AudioBuffer<float> wetBuffer;
    std::vector<float> lowGainRamp, highGainRamp, levelRamp, mixRamp;

    juce::SmoothedValue<float> lowGainSmooth, highGainSmooth, levelSmooth, mixSmooth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DreamFuzzProcessor)
};
