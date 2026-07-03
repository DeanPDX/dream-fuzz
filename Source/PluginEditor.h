#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "DreamLookAndFeel.h"
#include "PluginProcessor.h"

//==============================================================================
/** The stomp footswitch baked into the artwork, made clickable. Paints only a
    press/engage overlay so the artwork itself stays visible. */
class FootswitchButton : public juce::Button
{
public:
    FootswitchButton() : juce::Button ("Footswitch")
    {
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);

        if (isDown)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillEllipse (b.reduced (b.getWidth() * 0.08f));
        }
        else if (isHighlighted)
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillEllipse (b);
        }
    }

    bool hitTest (int x, int y) override
    {
        const auto c = getLocalBounds().getCentre().toFloat();
        const float r = (float) getWidth() * 0.5f;
        return juce::Point<float> ((float) x, (float) y).getDistanceFrom (c) <= r;
    }
};

//==============================================================================
/** Status LED: glows pink when the pedal is engaged, goes dark when bypassed.
    The artwork LED is lit, so the "off" state paints a dark cover. */
class LedComponent : public juce::Component
{
public:
    LedComponent() { setInterceptsMouseClicks (false, false); }

    void setOn (bool shouldBeOn)
    {
        if (on != shouldBeOn)
        {
            on = shouldBeOn;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const auto c = b.getCentre();

        if (on)
        {
            // extra bloom on top of the artwork's lit LED
            juce::ColourGradient glow (DreamLookAndFeel::neonPink.withAlpha (0.55f), c.x, c.y,
                                       DreamLookAndFeel::neonPink.withAlpha (0.0f), c.x, b.getRight(), true);
            g.setGradientFill (glow);
            g.fillEllipse (b);
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.fillEllipse (b.withSizeKeepingCentre (b.getWidth() * 0.22f, b.getHeight() * 0.22f));
        }
        else
        {
            // cover the artwork's lit LED with a dark, dead lens
            g.setColour (juce::Colour (0xf0141117));
            g.fillEllipse (b.withSizeKeepingCentre (b.getWidth() * 0.78f, b.getHeight() * 0.78f));
            g.setColour (DreamLookAndFeel::neonPink.withAlpha (0.18f));
            g.drawEllipse (b.withSizeKeepingCentre (b.getWidth() * 0.72f, b.getHeight() * 0.72f), 1.5f);
        }
    }

private:
    bool on = true;
};

//==============================================================================
class DreamFuzzEditor : public juce::AudioProcessorEditor
{
public:
    explicit DreamFuzzEditor (DreamFuzzProcessor&);
    ~DreamFuzzEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void setupKnob (juce::Slider& knob, const juce::String& paramId,
                    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment);

    DreamFuzzProcessor& fuzzProcessor;
    DreamLookAndFeel lookAndFeel;

    juce::Image background;

    juce::Slider toneKnob, levelKnob, fuzzKnob;
    FootswitchButton footswitch;
    LedComponent led;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> toneAttachment, levelAttachment, fuzzAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> footswitchAttachment;
    std::unique_ptr<juce::ParameterAttachment> ledAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DreamFuzzEditor)
};
