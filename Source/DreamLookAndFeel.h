#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
/** Vector knob styled to match the synthwave pedal artwork: dark brushed cap,
    neon-pink pointer with glow, subtle value arc. Drawn fully opaque so it
    covers the static knob baked into the background image. */
class DreamLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static inline const juce::Colour neonPink   { 0xffff2fa6 };
    static inline const juce::Colour neonViolet { 0xff8a2be2 };
    static inline const juce::Colour capDark    { 0xff0c0b10 };
    static inline const juce::Colour capMid     { 0xff1b1a22 };

    DreamLookAndFeel()
    {
        setColour (juce::BubbleComponent::backgroundColourId, juce::Colour (0xee0c0b12));
        setColour (juce::BubbleComponent::outlineColourId, neonPink.withAlpha (0.8f));
        setColour (juce::TooltipWindow::textColourId, juce::Colours::white);
        setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
        const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
        auto square = bounds.withSizeKeepingCentre (size, size);

        const auto centre = square.getCentre();
        const float radius = size * 0.5f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // drop shadow
        {
            auto shadow = square.reduced (size * 0.04f).translated (0.0f, size * 0.03f);
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillEllipse (shadow);
        }

        // skirt: dark metal ring with a pink under-glow from below (matches art)
        {
            auto skirt = square.reduced (size * 0.02f);
            juce::ColourGradient grad (capMid.brighter (0.15f), centre.x, skirt.getY(),
                                       juce::Colour (0xff08070b), centre.x, skirt.getBottom(), false);
            g.setGradientFill (grad);
            g.fillEllipse (skirt);

            // rim light
            g.setColour (neonPink.withAlpha (0.35f));
            g.drawEllipse (skirt.reduced (0.5f), 1.2f);

            juce::Path lowerArc;
            lowerArc.addCentredArc (centre.x, centre.y, radius * 0.96f, radius * 0.96f,
                                    0.0f, juce::MathConstants<float>::pi * 0.6f,
                                    juce::MathConstants<float>::pi * 1.4f, true);
            g.setColour (neonPink.withAlpha (0.45f));
            g.strokePath (lowerArc, juce::PathStrokeType (size * 0.02f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
        }

        // knurled edge suggestion
        {
            const int teeth = 24;
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            for (int i = 0; i < teeth; ++i)
            {
                const float a = (float) i / (float) teeth * juce::MathConstants<float>::twoPi + angle;
                const float r0 = radius * 0.88f, r1 = radius * 0.96f;
                g.drawLine (centre.x + r0 * std::sin (a), centre.y - r0 * std::cos (a),
                            centre.x + r1 * std::sin (a), centre.y - r1 * std::cos (a),
                            size * 0.012f);
            }
        }

        // cap
        {
            auto cap = square.reduced (size * 0.14f);
            juce::ColourGradient grad (capMid.brighter (0.35f), centre.x, cap.getY(),
                                       capDark, centre.x, cap.getBottom(), false);
            grad.addColour (0.35, capMid);
            g.setGradientFill (grad);
            g.fillEllipse (cap);

            // concentric machining rings
            g.setColour (juce::Colours::white.withAlpha (0.045f));
            for (float r = 0.18f; r < 0.34f; r += 0.055f)
                g.drawEllipse (square.reduced (size * r), 1.0f);

            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawEllipse (cap, 1.0f);
        }

        // value arc (subtle, synthwave accent)
        {
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, radius * 0.99f, radius * 0.99f,
                               0.0f, rotaryStartAngle, angle, true);
            g.setColour (neonPink.withAlpha (0.16f));
            g.strokePath (arc, juce::PathStrokeType (size * 0.05f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
            g.setColour (neonPink.withAlpha (0.55f));
            g.strokePath (arc, juce::PathStrokeType (size * 0.018f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        // pointer with neon glow
        {
            const float r0 = radius * 0.30f, r1 = radius * 0.78f;
            const juce::Point<float> p0 (centre.x + r0 * std::sin (angle), centre.y - r0 * std::cos (angle));
            const juce::Point<float> p1 (centre.x + r1 * std::sin (angle), centre.y - r1 * std::cos (angle));

            const float w = juce::jmax (2.0f, size * 0.045f);
            g.setColour (neonPink.withAlpha (0.10f));
            g.drawLine ({ p0, p1 }, w * 3.2f);
            g.setColour (neonPink.withAlpha (0.28f));
            g.drawLine ({ p0, p1 }, w * 1.9f);
            g.setColour (neonPink);
            g.drawLine ({ p0, p1 }, w);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawLine ({ p0, p1 }, w * 0.35f);
        }
    }

    juce::Font getSliderPopupFont (juce::Slider&) override
    {
        return juce::Font (juce::FontOptions (15.0f, juce::Font::bold));
    }

    int getSliderPopupPlacement (juce::Slider&) override
    {
        return juce::BubbleComponent::below;
    }
};
