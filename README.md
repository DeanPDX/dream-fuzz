# Dream Fuzz

A synthwave fuzz pedal plugin (VST3 / AU / Standalone) inspired by the
Softube Amp Room *Filbyter* fuzz — a vintage two-transistor,
Fuzz Face-family circuit — with an analog-modelled DSP core.

![UI](ui.png)

## Controls

| Control | Range | What it does |
|---|---|---|
| **TONE** | 0–10 | Treble shelf at ~4.3 kHz (dark ↔ bright), ±16 dB |
| **LEVEL** | −24…+12 dB | Output volume |
| **FUZZ** | 0–10 | Gain of both transistor stages, plus bias/sag intensity |
| **Footswitch** | on/bypass | Click-free, latency-matched bypass (LED shows state) |

## The analog model

The DSP core (`Source/FuzzDSP.h`) models the behaviours that make the
Fuzz Face family sound alive, rather than just waveshaping:

- **Two cascaded asymmetric stages** — tanh on the saturation side, a much
  softer algebraic curve on the cutoff side (germanium-style knees), with
  opposite polarity per stage for strong even-harmonic content.
- **Low-frequency bias feedback** (stage 2 → stage 1) — the mechanism behind
  the famous touch-sensitive compression and volume-knob cleanup. (The
  reference fit below drives this to ~0; re-enable via `Tuning::fbAmtMin/Max`.)
- **Dynamic bias shift** ("blocking distortion") — sustained drive pushes
  stage 2 toward cutoff, giving duty-cycle modulation, sputter and the
  gated decay at high fuzz settings.
- **Gain-dependent Miller lowpass** in each stage — more fuzz = darker core,
  like a real over-driven transistor's shrinking bandwidth.
- **Guitar-loading pre-filter** — models the treble absorbed by the
  pedal's low input impedance.
- **8× oversampling** (4× at 88.2 kHz+) around the nonlinear core, so the
  fuzz spectrum stays alias-free. Latency is reported to the host, and the
  bypass dry path is delay-matched so toggling never clicks or smears.

## Reference-matched voicing

Every voicing constant of the model lives in `df::Tuning`
(`Source/FuzzDSP.h`). The current defaults were fitted against the
recordings in `reference/`: the dry DI track is rendered through the real
processor offline and compared to the reference-fuzz tracks — banded
spectrum shape, loudness, frame-envelope dynamics and crest factor, at
both documented knob settings — and CMA-ES minimizes the combined error
(`Tools/tune_voicing.py`). The fit brought the combined error from 40.1 dB
to 4.9 dB; spectral shape now matches the reference within ~1 dB RMSE at
both settings.

`Tools/OfflineRender.cpp` (target `DreamFuzzRender`) renders a WAV through
the processor and accepts `name=value` overrides for every `df::Tuning`
field (run with `-list` to see them), so refitting after adding new
reference tracks needs no recompiles:

```sh
./build/DreamFuzzRender_artefacts/Release/DreamFuzzRender in.wav out.wav \
    <tone 0-10> <level dB> <fuzz 0-10> [gainBMaxDb=40 fizzHz=9000 ...]
```

## Building

Requires CMake ≥ 3.22 and Xcode command-line tools on macOS, or Visual
Studio 2022 on Windows (JUCE 8 is fetched automatically on first configure).

Note for Windows: configure applies a small workaround
(`cmake/PatchJUCE.cmake`) for an MSVC name-lookup bug (C2327/C2065) in
JUCE 8.0.14's `juce_UMPDispatcher.h`; without it some VS 2022 toolsets fail
to compile `juce_audio_devices`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Built plugins are copied into `~/Library/Audio/Plug-Ins/{VST3,Components}`
automatically.

## Verification

An offline test harness runs the real processor headlessly and checks
numerical health, quiescent stability, gain staging, bypass integrity,
chunk-size invariance and aliasing, and renders a UI snapshot:

```sh
./build/DreamFuzzTests_artefacts/Release/DreamFuzzTests snapshot.png
```

The harness also loads the installed `Dream Fuzz.vst3` through JUCE's VST3
hosting layer — the same ABI a DAW uses — and verifies discovery,
instantiation, parameters, latency reporting, audio rendering and state
round-tripping.

### Note on AU registration (macOS)

The `.component` has been verified end-to-end (load → register →
instantiate → initialize → render). However, macOS's AudioComponent
registrar can fail to index the *first* third-party AU installed for a user
account until the next logout/login or reboot. If `auval -v aufx Drfz Smbt`
reports "didn't find the component", log out and back in (or reboot), then
re-run it.
