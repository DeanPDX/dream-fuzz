// Offline renderer for Dream Fuzz.
//
// Runs a WAV file through the real plugin processor headlessly — the same
// code path a DAW uses minus the plugin ABI — so renders can be compared
// against reference recordings (see reference/README.md) when tuning the
// DSP model.
//
// Usage: DreamFuzzRender in.wav out.wav <tone 0-10> <level dB> <fuzz 0-10> [name=value ...]
//
// Trailing name=value pairs override df::Tuning voicing constants (e.g.
// gainBMaxDb=36 fizzHz=9000), which lets an external optimizer explore the
// voicing space without recompiling. Run with -list to see all names.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"

#include <cstdio>
#include <map>
#include <string>

static std::map<std::string, float df::Tuning::*> tuningFields()
{
    using T = df::Tuning;
    return {
        { "gainAMinDb", &T::gainAMinDb },   { "gainAMaxDb", &T::gainAMaxDb },
        { "gainBMinDb", &T::gainBMinDb },   { "gainBMaxDb", &T::gainBMaxDb },
        { "gainBShape", &T::gainBShape },
        { "fbAmtMin", &T::fbAmtMin },       { "fbAmtMax", &T::fbAmtMax },
        { "sagAmtMin", &T::sagAmtMin },     { "sagAmtMax", &T::sagAmtMax },
        { "makeupMinDb", &T::makeupMinDb }, { "makeupMaxDb", &T::makeupMaxDb },
        { "miller1MaxHz", &T::miller1MaxHz }, { "miller1MinHz", &T::miller1MinHz },
        { "miller2MaxHz", &T::miller2MaxHz }, { "miller2MinHz", &T::miller2MinHz },
        { "kPos1", &T::kPos1 }, { "kNeg1", &T::kNeg1 },
        { "kPos2", &T::kPos2 }, { "kNeg2", &T::kNeg2 },
        { "bias1", &T::bias1 }, { "bias2", &T::bias2 },
        { "coupleHz", &T::coupleHz },       { "fbHz", &T::fbHz },
        { "sagAttMs", &T::sagAttMs },       { "sagRelMs", &T::sagRelMs },
        { "inputHPHz", &T::inputHPHz },     { "loadLPHz", &T::loadLPHz },
        { "outputHPHz", &T::outputHPHz },   { "tiltHz", &T::tiltHz },
        { "fizzHz", &T::fizzHz },           { "fizzQ", &T::fizzQ },
        { "tiltLowFullDb", &T::tiltLowFullDb }, { "tiltHighFullDb", &T::tiltHighFullDb },
    };
}

int main (int argc, char* argv[])
{
    if (argc == 2 && std::string (argv[1]) == "-list")
    {
        df::Tuning defaults;
        for (const auto& [name, member] : tuningFields())
            std::printf ("%s=%g\n", name.c_str(), (double) (defaults.*member));
        return 0;
    }

    if (argc < 6)
    {
        std::printf ("Usage: %s in.wav out.wav <tone 0-10> <level dB> <fuzz 0-10> [name=value ...]\n"
                     "       %s -list   (print tunable voicing constants)\n",
                     argv[0], argv[0]);
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File inFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const float tone  = (float) juce::String (argv[3]).getDoubleValue();
    const float level = (float) juce::String (argv[4]).getDoubleValue();
    const float fuzz  = (float) juce::String (argv[5]).getDoubleValue();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (inFile));
    if (reader == nullptr)
    {
        std::printf ("Could not read %s\n", inFile.getFullPathName().toRawUTF8());
        return 1;
    }

    const double fs = reader->sampleRate;
    const int numSamples = (int) reader->lengthInSamples;

    juce::AudioBuffer<float> audio (2, numSamples);
    reader->read (&audio, 0, numSamples, 0, true, true);
    if (reader->numChannels == 1)
        audio.copyFrom (1, 0, audio, 0, 0, numSamples);

    df::Tuning tuning;
    const auto fields = tuningFields();

    for (int i = 6; i < argc; ++i)
    {
        const std::string arg (argv[i]);
        const auto eq = arg.find ('=');
        const auto it = eq == std::string::npos ? fields.end() : fields.find (arg.substr (0, eq));

        if (it == fields.end())
        {
            std::printf ("Unknown tuning override '%s' (see -list)\n", arg.c_str());
            return 1;
        }

        tuning.*(it->second) = std::stof (arg.substr (eq + 1));
    }

    DreamFuzzProcessor p;
    p.setTuning (tuning);
    auto setParam = [&p] (const char* id, float plainValue)
    {
        auto* param = p.apvts.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    };
    setParam (DreamFuzzProcessor::toneId, tone);
    setParam (DreamFuzzProcessor::levelId, level);
    setParam (DreamFuzzProcessor::fuzzId, fuzz);

    constexpr int blockSize = 512;
    p.setPlayConfigDetails (2, 2, fs, blockSize);
    p.prepareToPlay (fs, blockSize);
    const int latency = p.getLatencySamples();

    // process with `latency` extra samples so the output can be realigned
    juce::AudioBuffer<float> out (2, numSamples + latency);
    juce::MidiBuffer midi;

    for (int pos = 0; pos < numSamples + latency; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, numSamples + latency - pos);
        juce::AudioBuffer<float> block (2, n);
        block.clear();

        const int avail = juce::jmax (0, juce::jmin (n, numSamples - pos));
        for (int ch = 0; ch < 2; ++ch)
            if (avail > 0)
                block.copyFrom (ch, 0, audio, ch, pos, avail);

        p.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, n);
    }

    // drop the latency head so output aligns with the input file
    juce::AudioBuffer<float> aligned (2, numSamples);
    for (int ch = 0; ch < 2; ++ch)
        aligned.copyFrom (ch, 0, out, ch, latency, numSamples);

    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (new juce::FileOutputStream (outFile),
                             fs, 2, 32, {}, 0));
    if (writer == nullptr)
    {
        std::printf ("Could not write %s\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }

    writer->writeFromAudioSampleBuffer (aligned, 0, numSamples);
    writer->flush();

    std::printf ("Rendered %d samples @ %.0f Hz (tone %.1f, level %.1f dB, fuzz %.1f, latency %d) -> %s\n",
                 numSamples, fs, tone, level, fuzz, latency,
                 outFile.getFullPathName().toRawUTF8());
    return 0;
}
