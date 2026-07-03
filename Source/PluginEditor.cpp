#include "PluginEditor.h"

#include "BinaryData.h"

namespace
{
// Control positions as fractions of the 1024x1536 artwork.
struct Spot { float cx, cy, d; };

constexpr Spot toneSpot   { 0.2656f, 0.2025f, 0.195f };
constexpr Spot levelSpot  { 0.5010f, 0.2031f, 0.195f };
constexpr Spot fuzzSpot   { 0.7422f, 0.1986f, 0.195f };
constexpr Spot switchSpot { 0.5039f, 0.7585f, 0.200f };
constexpr Spot ledSpot    { 0.4970f, 0.8520f, 0.058f };

juce::Rectangle<int> spotBounds (const Spot& s, int w, int h)
{
    const float size = s.d * (float) w;
    return juce::Rectangle<float> (s.cx * (float) w - size * 0.5f,
                                   s.cy * (float) h - size * 0.5f,
                                   size, size).toNearestInt();
}
} // namespace

//==============================================================================
DreamFuzzEditor::DreamFuzzEditor (DreamFuzzProcessor& p)
    : AudioProcessorEditor (p), fuzzProcessor (p)
{
    setLookAndFeel (&lookAndFeel);

    background = juce::ImageCache::getFromMemory (BinaryData::background_png,
                                                  BinaryData::background_pngSize);

    setupKnob (toneKnob,  DreamFuzzProcessor::toneId,  toneAttachment);
    setupKnob (levelKnob, DreamFuzzProcessor::levelId, levelAttachment);
    setupKnob (fuzzKnob,  DreamFuzzProcessor::fuzzId,  fuzzAttachment);

    addAndMakeVisible (footswitch);
    footswitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        fuzzProcessor.apvts, DreamFuzzProcessor::bypassId, footswitch);
    footswitch.setTooltip ("Bypass");

    addAndMakeVisible (led);
    if (auto* bypassParam = fuzzProcessor.apvts.getParameter (DreamFuzzProcessor::bypassId))
    {
        ledAttachment = std::make_unique<juce::ParameterAttachment> (
            *bypassParam,
            [this] (float v) { led.setOn (v < 0.5f); },
            nullptr);
        ledAttachment->sendInitialUpdate();
    }

    setResizable (true, true);
    setResizeLimits (320, 480, 1024, 1536);

    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (1024.0 / 1536.0);

    setSize (512, 768);
}

DreamFuzzEditor::~DreamFuzzEditor()
{
    setLookAndFeel (nullptr);
}

void DreamFuzzEditor::setupKnob (juce::Slider& knob, const juce::String& paramId,
                                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
{
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                              juce::MathConstants<float>::pi * 2.75f, true);
    knob.setMouseDragSensitivity (160);
    knob.setVelocityModeParameters (1.0, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    knob.setPopupDisplayEnabled (true, false, this);

    if (auto* param = fuzzProcessor.apvts.getParameter (paramId))
    {
        knob.setTextValueSuffix ({});
        knob.textFromValueFunction = [param] (double v)
        {
            return param->getText (param->convertTo0to1 ((float) v), 32);
        };
        knob.setDoubleClickReturnValue (true,
            (double) param->convertFrom0to1 (param->getDefaultValue()));
    }

    addAndMakeVisible (knob);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        fuzzProcessor.apvts, paramId, knob);
}

//==============================================================================
void DreamFuzzEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (background, getLocalBounds().toFloat());
}

void DreamFuzzEditor::resized()
{
    const int w = getWidth();
    const int h = getHeight();

    toneKnob.setBounds  (spotBounds (toneSpot, w, h));
    levelKnob.setBounds (spotBounds (levelSpot, w, h));
    fuzzKnob.setBounds  (spotBounds (fuzzSpot, w, h));
    footswitch.setBounds (spotBounds (switchSpot, w, h));
    led.setBounds (spotBounds (ledSpot, w, h).expanded (juce::roundToInt (0.02f * (float) w)));
}
