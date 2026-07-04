#pragma once

#include <algorithm>
#include <cmath>

//==============================================================================
// Dream Fuzz — analog-modelled two-transistor fuzz core.
//
// The model follows the classic Fuzz Face-family topology (the circuit family
// Softube's Filbyter belongs to): two directly-coupled gain stages wrapped in
// a bias feedback loop. The behaviours that make those pedals sound "alive"
// are modelled explicitly:
//
//   * Asymmetric, germanium-soft clipping in both stages (different knees on
//     the cutoff and saturation sides -> strong even-harmonic content).
//   * Low-frequency bias feedback from stage 2 back to stage 1, which is what
//     makes a real Fuzz Face compress and clean up with playing dynamics.
//   * Dynamic bias shift ("blocking distortion"): sustained drive charges the
//     coupling network and pushes stage 2 toward cutoff, giving the classic
//     sputtery, gated decay at high fuzz settings.
//   * Gain-dependent Miller lowpass in each stage: more fuzz = darker core,
//     exactly like the shrinking bandwidth of an over-driven transistor stage.
//
// The core is meant to run oversampled (8x at 44.1/48k); everything here is
// plain C++ with no library dependencies so it can be unit-tested in
// isolation.
//==============================================================================
namespace df
{

constexpr float kPi = 3.14159265358979f;

inline float dbToLin (float dB) noexcept { return std::pow (10.0f, dB * 0.05f); }
inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }

//==============================================================================
/** Every voicing constant of the model in one place.

    The defaults are the shipped sound; they were fitted against the
    reference recordings in reference/ (see reference/README.md). The offline
    render tool (Tools/OfflineRender.cpp) can override any field from the
    command line, which is how the fitting is done. */
struct Tuning
{
    // --- FuzzCore: fuzz-knob endpoints (value at fuzz=0 .. value at fuzz=1)
    float gainAMinDb  = 6.0f,     gainAMaxDb  = 19.0f;    // Q1 gain, dB
    float gainBMinDb  = 12.0f,    gainBMaxDb  = 34.0f;    // Q2 gain, dB
    float gainBShape  = 0.85f;                            // pow() curve for Q2
    float fbAmtMin    = 0.085f,   fbAmtMax    = 0.020f;   // bias feedback amount
    float sagAmtMin   = 0.05f,    sagAmtMax   = 0.40f;    // blocking distortion
    float makeupMinDb = -13.0f,   makeupMaxDb = -10.5f;   // output makeup, dB
    float miller1MaxHz = 10000.0f, miller1MinHz = 4500.0f; // Q1 Miller LP, Hz
    float miller2MaxHz = 9000.0f,  miller2MinHz = 3200.0f; // Q2 Miller LP, Hz

    // --- FuzzCore: static voicing
    float kPos1 = 0.85f, kNeg1 = 0.45f;   // stage 1 clipping knees
    float kPos2 = 0.70f, kNeg2 = 0.50f;   // stage 2 clipping knees
    float bias1 = 0.06f, bias2 = 0.12f;   // static bias offsets
    float coupleHz = 30.0f;               // interstage coupling HP
    float fbHz = 120.0f;                  // bias-feedback lowpass
    float sagAttMs = 3.0f, sagRelMs = 90.0f;

    // --- BaseChain
    float inputHPHz  = 20.0f;    // input coupling cap
    float loadLPHz   = 4000.0f;  // guitar loading by the input impedance
    float outputHPHz = 25.0f;    // output coupling cap
    float tiltHz     = 800.0f;   // TONE pivot
    float fizzHz     = 7800.0f;  // post "fizz" lowpass
    float fizzQ      = 0.707f;

    // --- TONE shelf gains at full tilt (tone = 10), dB
    float tiltLowFullDb = -6.5f, tiltHighFullDb = 6.5f;
};

//==============================================================================
/** Zero-delay-feedback (TPT) one-pole. Stable under fast cutoff modulation. */
struct OnePole
{
    void prepare (float fs) noexcept { sampleRate = fs; }

    void setCutoff (float fc) noexcept
    {
        fc = std::clamp (fc, 1.0f, sampleRate * 0.47f);
        const float g = std::tan (kPi * fc / sampleRate);
        G = g / (1.0f + g);
    }

    float processLP (float x) noexcept
    {
        const float v = (x - s) * G;
        const float y = v + s;
        s = y + v;
        return y;
    }

    float processHP (float x) noexcept { return x - processLP (x); }

    void reset() noexcept { s = 0.0f; }

    float sampleRate = 44100.0f, G = 0.0f, s = 0.0f;
};

//==============================================================================
/** RBJ biquad (TDF2) — used for the post "fizz" lowpass. */
struct BiquadLP
{
    void prepare (float fs) noexcept { sampleRate = fs; }

    void setLowpass (float fc, float q) noexcept
    {
        fc = std::clamp (fc, 10.0f, sampleRate * 0.45f);
        const float w0 = 2.0f * kPi * fc / sampleRate;
        const float cw = std::cos (w0), sw = std::sin (w0);
        const float alpha = sw / (2.0f * q);
        const float a0 = 1.0f + alpha;

        b0 = ((1.0f - cw) * 0.5f) / a0;
        b1 = (1.0f - cw) / a0;
        b2 = b0;
        a1 = (-2.0f * cw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0f; }

    float sampleRate = 44100.0f;
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
};

//==============================================================================
/** Peak envelope follower with independent attack/release. */
struct EnvFollower
{
    void prepare (float fs, float attackMs, float releaseMs) noexcept
    {
        aAtt = std::exp (-1.0f / (fs * attackMs * 0.001f));
        aRel = std::exp (-1.0f / (fs * releaseMs * 0.001f));
    }

    float process (float x) noexcept
    {
        const float r = std::abs (x);
        const float a = (r > env) ? aAtt : aRel;
        env = a * env + (1.0f - a) * r;
        return env;
    }

    void reset() noexcept { env = 0.0f; }

    float aAtt = 0.0f, aRel = 0.0f, env = 0.0f;
};

//==============================================================================
/** Asymmetric transistor-style soft clipper.
    Positive half: tanh (saturation side, firmer knee).
    Negative half: algebraic (cutoff side, very gradual, germanium-like).
    Continuous value and slope (f'(0) = 1) at the origin. */
inline float satAsym (float v, float kPos, float kNeg) noexcept
{
    if (v >= 0.0f)
        return kPos * std::tanh (v / kPos);

    const float u = v / kNeg;
    return v / std::sqrt (1.0f + u * u);
}

//==============================================================================
/** The oversampled nonlinear core. One instance per audio channel. */
class FuzzCore
{
public:
    void prepare (double osRate, const Tuning& t) noexcept
    {
        fs = (float) osRate;
        tuning = t;

        miller1.prepare (fs);
        miller2.prepare (fs);
        couple.prepare (fs);
        couple.setCutoff (tuning.coupleHz);
        fbFilter.prepare (fs);
        fbFilter.setCutoff (tuning.fbHz);  // bias feedback is a low-frequency effect
        sagEnv.prepare (fs, tuning.sagAttMs, tuning.sagRelMs);

        // ~8 ms parameter smoothing inside the core (runs at the OS rate)
        smooth = 1.0f - std::exp (-1.0f / (fs * 0.008f));

        setParams (0.65f);
        reset();
    }

    /** fuzz01 in [0, 1]. Call once per block (values are smoothed per sample). */
    void setParams (float fuzz01) noexcept
    {
        const float f = std::clamp (fuzz01, 0.0f, 1.0f);
        const Tuning& t = tuning;

        tGainA  = dbToLin (lerp (t.gainAMinDb, t.gainAMaxDb, f));                       // Q1 gain
        tGainB  = dbToLin (lerp (t.gainBMinDb, t.gainBMaxDb, std::pow (f, t.gainBShape))); // Q2 gain
        tFbAmt  = lerp (t.fbAmtMin, t.fbAmtMax, f);     // more cleanup headroom at low fuzz
        tSagAmt = lerp (t.sagAmtMin, t.sagAmtMax, f * f); // gating/sputter grows with fuzz
        tMakeup = dbToLin (lerp (t.makeupMinDb, t.makeupMaxDb, f));

        miller1.setCutoff (lerp (t.miller1MaxHz, t.miller1MinHz, f));
        miller2.setCutoff (lerp (t.miller2MaxHz, t.miller2MinHz, f));
    }

    void reset() noexcept
    {
        miller1.reset();
        miller2.reset();
        couple.reset();
        fbFilter.reset();
        sagEnv.reset();
        fb = 0.0f;

        // snap smoothed values to their targets
        gainA = tGainA; gainB = tGainB;
        fbAmt = tFbAmt; sagAmt = tSagAmt; makeup = tMakeup;
    }

    float processSample (float x) noexcept
    {
        // per-sample parameter smoothing (zipper-free automation)
        gainA  += (tGainA  - gainA)  * smooth;
        gainB  += (tGainB  - gainB)  * smooth;
        fbAmt  += (tFbAmt  - fbAmt)  * smooth;
        sagAmt += (tSagAmt - sagAmt) * smooth;
        makeup += (tMakeup - makeup) * smooth;

        // --- Stage 1 (Q1) ---------------------------------------------------
        // Bias feedback from stage 2 output opposes the input (y2 is inverted
        // w.r.t. x, so '+' here is negative feedback): this is the touch-
        // sensitive compression / volume-knob cleanup of the real circuit.
        const float in1 = gainA * (x + fb * fbAmt) + tuning.bias1;
        float y1 = satAsym (in1, tuning.kPos1, tuning.kNeg1);
        y1 = miller1.processLP (y1);
        y1 = couple.processHP (y1);

        // --- Stage 2 (Q2, inverting) ----------------------------------------
        // Static bias offset for idle asymmetry, plus a dynamic shift driven
        // by the output envelope: sustained drive pushes the stage toward
        // cutoff -> duty-cycle modulation, sputter and gated decay.
        const float bias2 = tuning.bias2 - sagAmt * sagEnv.env;
        float y2 = satAsym (-gainB * y1 + bias2, tuning.kPos2, tuning.kNeg2);
        y2 = miller2.processLP (y2);

        sagEnv.process (y2);
        fb = fbFilter.processLP (y2);

        return -y2 * makeup;   // re-invert: net output in phase with input
    }

private:
    float fs = 44100.0f, smooth = 0.01f;
    Tuning tuning;

    OnePole miller1, miller2, couple, fbFilter;
    EnvFollower sagEnv;
    float fb = 0.0f;

    float tGainA = 4.0f, tGainB = 12.0f, tFbAmt = 0.05f, tSagAmt = 0.1f, tMakeup = 0.4f;
    float gainA = 4.0f, gainB = 12.0f, fbAmt = 0.05f, sagAmt = 0.1f, makeup = 0.4f;
};

//==============================================================================
/** Base-rate (non-oversampled) conditioning around the core.
    One instance per channel. */
class BaseChain
{
public:
    void prepare (float fs, const Tuning& t) noexcept
    {
        inputHP.prepare (fs);
        inputHP.setCutoff (t.inputHPHz);   // input coupling cap
        loadLP.prepare (fs);
        loadLP.setCutoff (t.loadLPHz);     // guitar loading by the low input impedance
        outputHP.prepare (fs);
        outputHP.setCutoff (t.outputHPHz); // output coupling cap
        tiltXover.prepare (fs);
        tiltXover.setCutoff (t.tiltHz);    // TONE pivot
        fizzLP.prepare (fs);
        fizzLP.setLowpass (t.fizzHz, t.fizzQ);
        reset();
    }

    void reset() noexcept
    {
        inputHP.reset();
        loadLP.reset();
        outputHP.reset();
        tiltXover.reset();
        fizzLP.reset();
    }

    float processPre (float x) noexcept
    {
        return loadLP.processLP (inputHP.processHP (x));
    }

    /** lowGain/highGain: tilt-EQ shelf gains (linear). */
    float processPost (float x, float lowGain, float highGain) noexcept
    {
        x = outputHP.processHP (x);
        const float lp = tiltXover.processLP (x);
        const float hp = x - lp;
        return fizzLP.process (lp * lowGain + hp * highGain);
    }

private:
    OnePole inputHP, loadLP, outputHP, tiltXover;
    BiquadLP fizzLP;
};

} // namespace df
