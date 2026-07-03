#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
DreamFuzzProcessor::DreamFuzzProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    fuzzParamValue  = apvts.getRawParameterValue (fuzzId);
    toneParamValue  = apvts.getRawParameterValue (toneId);
    levelParamValue = apvts.getRawParameterValue (levelId);
    bypassParam     = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (bypassId));
}

juce::AudioProcessorValueTreeState::ParameterLayout DreamFuzzProcessor::createLayout()
{
    using namespace juce;
    ParameterID fuzzPid   { fuzzId,   1 };
    ParameterID tonePid   { toneId,   1 };
    ParameterID levelPid  { levelId,  1 };
    ParameterID bypassPid { bypassId, 1 };

    auto knobText = [] (float v, int) { return String (v, 1); };

    NormalisableRange<float> levelRange (-24.0f, 12.0f, 0.0f);
    levelRange.setSkewForCentre (0.0f);

    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    params.push_back (std::make_unique<AudioParameterFloat> (
        tonePid, "Tone", NormalisableRange<float> (0.0f, 10.0f, 0.0f), 5.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (knobText)));

    params.push_back (std::make_unique<AudioParameterFloat> (
        levelPid, "Level", levelRange, 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })
            .withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        fuzzPid, "Fuzz", NormalisableRange<float> (0.0f, 10.0f, 0.0f), 6.5f,
        AudioParameterFloatAttributes().withStringFromValueFunction (knobText)));

    params.push_back (std::make_unique<AudioParameterBool> (
        bypassPid, "Bypass", false,
        AudioParameterBoolAttributes().withLabel ("Bypass")));

    return { params.begin(), params.end() };
}

//==============================================================================
bool DreamFuzzProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    return in == out
        && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void DreamFuzzProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // 8x oversampling at 44.1/48k, 4x at 88.2/96k+ — the nonlinear core always
    // runs at ~350-400 kHz so the fuzz spectrum stays alias-free.
    const size_t numStages = sampleRate > 50000.0 ? 2 : 3;

    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        2, numStages,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
        true, /* isMaximumQuality */
        true  /* useIntegerLatency */);

    oversampling->initProcessing ((size_t) samplesPerBlock);

    const double osRate = sampleRate * std::pow (2.0, (double) numStages);

    for (int ch = 0; ch < 2; ++ch)
    {
        core[ch].prepare (osRate);
        base[ch].prepare ((float) sampleRate);
    }

    const int latency = (int) oversampling->getLatencyInSamples();
    setLatencySamples (latency);

    dryDelay.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, 2 });
    dryDelay.setDelay ((float) latency);

    wetBuffer.setSize (2, samplesPerBlock);
    lowGainRamp.resize ((size_t) samplesPerBlock);
    highGainRamp.resize ((size_t) samplesPerBlock);
    levelRamp.resize ((size_t) samplesPerBlock);
    mixRamp.resize ((size_t) samplesPerBlock);

    lowGainSmooth.reset (sampleRate, 0.02);
    highGainSmooth.reset (sampleRate, 0.02);
    levelSmooth.reset (sampleRate, 0.02);
    mixSmooth.reset (sampleRate, 0.03);

    reset();
}

void DreamFuzzProcessor::reset()
{
    if (oversampling != nullptr)
        oversampling->reset();

    for (int ch = 0; ch < 2; ++ch)
    {
        core[ch].reset();
        base[ch].reset();
    }

    dryDelay.reset();

    const float tone01 = toneParamValue->load() * 0.1f;
    const float tilt   = (tone01 - 0.5f) * 2.0f;
    lowGainSmooth.setCurrentAndTargetValue (df::dbToLin (-6.5f * tilt));
    highGainSmooth.setCurrentAndTargetValue (df::dbToLin (6.5f * tilt));
    levelSmooth.setCurrentAndTargetValue (df::dbToLin (levelParamValue->load()));
    mixSmooth.setCurrentAndTargetValue (bypassParam->get() ? 0.0f : 1.0f);
}

//==============================================================================
void DreamFuzzProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn      = getTotalNumInputChannels();
    const int numOut     = getTotalNumOutputChannels();
    const int numCh      = juce::jmin (numIn, numOut, 2);

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    if (numSamples == 0 || numCh == 0)
        return;

    // --- parameter targets for this block ------------------------------------
    const float fuzz01 = fuzzParamValue->load() * 0.1f;
    const float tone01 = toneParamValue->load() * 0.1f;
    const float tilt   = (tone01 - 0.5f) * 2.0f;   // -1 .. +1

    lowGainSmooth.setTargetValue (df::dbToLin (-6.5f * tilt));
    highGainSmooth.setTargetValue (df::dbToLin (6.5f * tilt));
    levelSmooth.setTargetValue (df::dbToLin (levelParamValue->load()));
    mixSmooth.setTargetValue (bypassParam->get() ? 0.0f : 1.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        lowGainRamp[(size_t) i]  = lowGainSmooth.getNextValue();
        highGainRamp[(size_t) i] = highGainSmooth.getNextValue();
        levelRamp[(size_t) i]    = levelSmooth.getNextValue();
        mixRamp[(size_t) i]      = mixSmooth.getNextValue();
    }

    for (int ch = 0; ch < 2; ++ch)
        core[ch].setParams (fuzz01);

    // --- wet path: pre-filters -> 8x oversampled core -------------------------
    // The oversampler is built for 2 channels, so always feed it 2 (mono input
    // is duplicated; only channel 0 is used on the way out).
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = buffer.getReadPointer (juce::jmin (ch, numCh - 1));
        float* wet = wetBuffer.getWritePointer (ch);

        for (int i = 0; i < numSamples; ++i)
            wet[i] = base[ch].processPre (src[i]);
    }

    {
        juce::dsp::AudioBlock<float> wetBlock (wetBuffer.getArrayOfWritePointers(), 2, (size_t) numSamples);
        auto osBlock = oversampling->processSamplesUp (wetBlock);

        for (int ch = 0; ch < 2; ++ch)
        {
            float* data = osBlock.getChannelPointer ((size_t) ch);
            const int n = (int) osBlock.getNumSamples();

            for (int i = 0; i < n; ++i)
                data[i] = core[ch].processSample (data[i]);
        }

        oversampling->processSamplesDown (wetBlock);
    }

    // --- post filters, level, latency-matched bypass crossfade ----------------
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* in = buffer.getReadPointer (ch);
        float* wet = wetBuffer.getWritePointer (ch);
        float* out = buffer.getWritePointer (ch);

        for (int i = 0; i < numSamples; ++i)
        {
            dryDelay.pushSample (ch, in[i]);
            const float dry = dryDelay.popSample (ch);

            float w = base[ch].processPost (wet[i], lowGainRamp[(size_t) i], highGainRamp[(size_t) i]);
            w *= levelRamp[(size_t) i];

            const float mix = mixRamp[(size_t) i];
            out[i] = w * mix + dry * (1.0f - mix);
        }
    }
}

//==============================================================================
void DreamFuzzProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DreamFuzzProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* DreamFuzzProcessor::createEditor()
{
    return new DreamFuzzEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DreamFuzzProcessor();
}
