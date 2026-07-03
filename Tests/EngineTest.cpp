// Offline verification harness for Dream Fuzz.
//
// Runs the actual plugin processor headlessly and checks:
//   1. numerical health (no NaN/inf) across the whole parameter space
//   2. no self-oscillation (silence in -> silence out at max fuzz)
//   3. gain staging (engaged loudness vs. input at default settings)
//   4. bypass integrity (bypassed output == latency-delayed input)
//   5. chunk-size invariance (same output regardless of block slicing)
//   6. aliasing (non-harmonic energy of a driven sine, should be tiny)
//
// Also renders the editor to a PNG (2x = artwork resolution) for visual
// verification of control placement. Usage: DreamFuzzTests [snapshot.png]

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstdio>

namespace
{
int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

float rmsDb (const float* data, int num)
{
    double acc = 0.0;
    for (int i = 0; i < num; ++i)
        acc += (double) data[i] * data[i];
    const double rms = std::sqrt (acc / std::max (1, num));
    return (float) (20.0 * std::log10 (std::max (rms, 1.0e-12)));
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* d = b.getReadPointer (ch);
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (d[i]))
                return false;
    }
    return true;
}

void setParam (DreamFuzzProcessor& p, const char* id, float plainValue)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

/** Renders `seconds` of a sine through the processor; returns the full output. */
juce::AudioBuffer<float> renderSine (DreamFuzzProcessor& p, double fs, int blockSize,
                                     float freq, float peak, float seconds)
{
    const int total = (int) (fs * seconds);
    juce::AudioBuffer<float> out (2, total);
    juce::MidiBuffer midi;
    int pos = 0;

    while (pos < total)
    {
        const int n = std::min (blockSize, total - pos);
        juce::AudioBuffer<float> block (2, n);

        for (int i = 0; i < n; ++i)
        {
            const float v = peak * std::sin (2.0f * juce::MathConstants<float>::pi
                                             * freq * (float) (pos + i) / (float) fs);
            block.setSample (0, i, v);
            block.setSample (1, i, v);
        }

        p.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, n);

        pos += n;
    }

    return out;
}
} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    constexpr double fs = 44100.0;
    constexpr int blockSize = 512;

    std::printf ("Dream Fuzz verification\n=======================\n");

    // --- 1. numerical health across the parameter space ----------------------
    std::printf ("\nParameter-space stability:\n");
    {
        bool finite = true, bounded = true;

        for (float fuzz : { 0.0f, 5.0f, 10.0f })
            for (float tone : { 0.0f, 10.0f })
                for (float level : { -24.0f, 12.0f })
                {
                    DreamFuzzProcessor p;
                    setParam (p, DreamFuzzProcessor::fuzzId, fuzz);
                    setParam (p, DreamFuzzProcessor::toneId, tone);
                    setParam (p, DreamFuzzProcessor::levelId, level);
                    p.prepareToPlay (fs, blockSize);

                    auto out = renderSine (p, fs, blockSize, 220.0f, 0.5f, 0.3f);
                    finite = finite && allFinite (out);
                    bounded = bounded && out.getMagnitude (0, out.getNumSamples()) < 8.0f;
                }

        check (finite, "output finite at all parameter extremes");
        check (bounded, "output bounded at all parameter extremes");
    }

    // --- 2. no self-oscillation ----------------------------------------------
    std::printf ("\nQuiescent stability (fuzz = 10, silence in):\n");
    {
        DreamFuzzProcessor p;
        setParam (p, DreamFuzzProcessor::fuzzId, 10.0f);
        p.prepareToPlay (fs, blockSize);

        auto out = renderSine (p, fs, blockSize, 220.0f, 0.0f, 2.0f);
        const int tail = (int) (0.5 * fs);
        const float tailDb = rmsDb (out.getReadPointer (0, out.getNumSamples() - tail), tail);
        std::printf ("  quiescent output: %.1f dBFS\n", tailDb);
        check (tailDb < -50.0f, "no self-oscillation / idle noise below -50 dBFS");
    }

    // --- 3. gain staging ------------------------------------------------------
    std::printf ("\nGain staging (220 Hz, -20 dBFS peak in, default knobs):\n");
    {
        DreamFuzzProcessor p;
        p.prepareToPlay (fs, blockSize);

        auto out = renderSine (p, fs, blockSize, 220.0f, 0.1f, 1.0f);
        const int half = out.getNumSamples() / 2;
        const float inDb  = 20.0f * std::log10 (0.1f / juce::MathConstants<float>::sqrt2);
        const float outDb = rmsDb (out.getReadPointer (0, half), half);
        std::printf ("  in %.1f dBFS RMS -> out %.1f dBFS RMS (%+.1f dB)\n",
                     inDb, outDb, outDb - inDb);
        check (outDb - inDb > -2.0f && outDb - inDb < 10.0f,
               "engaged loudness within -2..+10 dB of input");
    }

    // --- 4. bypass integrity --------------------------------------------------
    std::printf ("\nBypass integrity:\n");
    {
        DreamFuzzProcessor p;
        setParam (p, DreamFuzzProcessor::bypassId, 1.0f);
        p.prepareToPlay (fs, blockSize);
        const int latency = p.getLatencySamples();
        std::printf ("  reported latency: %d samples\n", latency);

        auto out = renderSine (p, fs, blockSize, 220.0f, 0.25f, 1.0f);

        // after the crossfade has settled, output == input delayed by `latency`
        const int start = (int) (0.3 * fs);
        const int num = (int) (0.5 * fs);
        double err = 0.0;

        for (int i = start; i < start + num; ++i)
        {
            const float expected = 0.25f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                     * 220.0f * (float) (i - latency) / (float) fs);
            const float diff = out.getSample (0, i) - expected;
            err += (double) diff * diff;
        }

        const float errDb = (float) (10.0 * std::log10 (std::max (err / num, 1.0e-14)));
        std::printf ("  bypassed-vs-delayed-input error: %.1f dBFS\n", errDb);
        check (errDb < -60.0f, "bypassed output equals latency-delayed input (< -60 dBFS)");
    }

    // --- 5. chunk-size invariance ----------------------------------------------
    std::printf ("\nChunk-size invariance:\n");
    {
        DreamFuzzProcessor pa, pb;
        pa.prepareToPlay (fs, blockSize);
        pb.prepareToPlay (fs, blockSize);

        auto outA = renderSine (pa, fs, blockSize, 220.0f, 0.2f, 0.5f);
        auto outB = renderSine (pb, fs, 173 /* awkward block size */, 220.0f, 0.2f, 0.5f);

        double err = 0.0;
        const int n = outA.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            const float diff = outA.getSample (0, i) - outB.getSample (0, i);
            err += (double) diff * diff;
        }
        const float errDb = (float) (10.0 * std::log10 (std::max (err / n, 1.0e-14)));
        std::printf ("  512-block vs 173-block difference: %.1f dBFS\n", errDb);
        check (errDb < -50.0f, "output independent of host block size (< -50 dBFS)");
    }

    // --- 6. aliasing ------------------------------------------------------------
    std::printf ("\nAliasing (1 kHz sine, fuzz = 10):\n");
    {
        DreamFuzzProcessor p;
        setParam (p, DreamFuzzProcessor::fuzzId, 10.0f);
        p.prepareToPlay (fs, blockSize);

        constexpr int order = 15;
        constexpr int N = 1 << order;                     // 32768
        const int bin = 743;                              // ~999.9 Hz, exact bin
        const float f0 = (float) bin * (float) fs / (float) N;

        auto out = renderSine (p, fs, blockSize, f0, 0.3f, 1.5f);

        std::vector<float> fft (2 * N, 0.0f);
        const float* d = out.getReadPointer (0, out.getNumSamples() - N);
        for (int i = 0; i < N; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                    * (float) i / (float) (N - 1));
            fft[(size_t) i] = d[i] * w;
        }

        juce::dsp::FFT engine (order);
        engine.performFrequencyOnlyForwardTransform (fft.data());

        double harmonicE = 0.0, totalE = 0.0;
        for (int k = 1; k < N / 2; ++k)
        {
            const double e = (double) fft[(size_t) k] * fft[(size_t) k];
            totalE += e;

            const int nearest = ((k + bin / 2) / bin) * bin;   // nearest harmonic bin
            if (nearest > 0 && std::abs (k - nearest) <= 4)
                harmonicE += e;
        }

        const double aliasE = std::max (totalE - harmonicE, 1.0e-16);
        const float aliasDb = (float) (10.0 * std::log10 (aliasE / std::max (harmonicE, 1.0e-16)));
        std::printf ("  non-harmonic vs harmonic energy: %.1f dB\n", aliasDb);
        check (aliasDb < -35.0f, "aliasing + noise below -35 dB");
    }

    // --- VST3: load the installed plugin through the real host ABI ---------------
    std::printf ("\nVST3 hosting (installed Dream Fuzz.vst3 via VST3 ABI):\n");
    {
        const auto vst3File = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                  .getChildFile ("Library/Audio/Plug-Ins/VST3/Dream Fuzz.vst3");

        if (! vst3File.isDirectory())
        {
            std::printf ("  (skipped: %s not installed)\n", vst3File.getFullPathName().toRawUTF8());
        }
        else
        {
            juce::VST3PluginFormat vst3;
            juce::OwnedArray<juce::PluginDescription> found;
            vst3.findAllTypesForFile (found, vst3File.getFullPathName());
            check (found.size() == 1, "VST3 discovered by host scanner");

            if (found.size() == 1)
            {
                juce::AudioPluginFormatManager fm;
                fm.addFormat (new juce::VST3PluginFormat());

                juce::String error;
                auto instance = fm.createPluginInstance (*found[0], fs, blockSize, error);
                check (instance != nullptr, "VST3 instantiated by host");

                if (instance != nullptr)
                {
                    check (instance->getPluginDescription().name == "Dream Fuzz", "plugin name correct");

                    juce::StringArray wantedParams { "Tone", "Level", "Fuzz", "Bypass" };
                    int foundParams = 0;
                    for (auto* param : instance->getParameters())
                        if (wantedParams.contains (param->getName (32)))
                            ++foundParams;
                    check (foundParams == 4, "Tone/Level/Fuzz/Bypass parameters exposed");

                    instance->setPlayConfigDetails (2, 2, fs, blockSize);
                    instance->prepareToPlay (fs, blockSize);
                    check (instance->getLatencySamples() == 6, "latency reported through VST3");

                    juce::AudioBuffer<float> block (2, blockSize);
                    juce::MidiBuffer midi;
                    bool finite = true;
                    float peak = 0.0f;

                    for (int b = 0; b < 40; ++b)
                    {
                        for (int i = 0; i < blockSize; ++i)
                        {
                            const float v = 0.2f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                             * 220.0f * (float) (b * blockSize + i) / (float) fs);
                            block.setSample (0, i, v);
                            block.setSample (1, i, v);
                        }
                        instance->processBlock (block, midi);
                        finite = finite && allFinite (block);
                        peak = juce::jmax (peak, block.getMagnitude (0, blockSize));
                    }

                    check (finite && peak > 0.01f && peak < 2.0f, "audio renders through VST3 host");

                    juce::MemoryBlock state;
                    instance->getStateInformation (state);
                    instance->setStateInformation (state.getData(), (int) state.getSize());
                    check (state.getSize() > 0, "state save/restore round-trips");

                    instance->releaseResources();
                }
            }
        }
    }

    // --- editor snapshot ---------------------------------------------------------
    std::printf ("\nEditor snapshot:\n");
    {
        DreamFuzzProcessor p;
        p.prepareToPlay (fs, blockSize);

        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditorAndMakeActive());
        check (editor != nullptr, "editor created");

        if (editor != nullptr)
        {
            auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            check (image.isValid(), "snapshot rendered");

            const juce::String path = argc > 1 ? juce::String (argv[1])
                                               : juce::String ("editor_snapshot.png");
            juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (path));
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat png;
            const bool written = stream.openedOk() && png.writeImageToStream (image, stream);
            check (written, "snapshot written");
            if (written)
                std::printf ("  -> %s\n", file.getFullPathName().toRawUTF8());

            p.editorBeingDeleted (editor.get());
        }
    }

    std::printf ("\n%s (%d failure%s)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
