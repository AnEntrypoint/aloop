// aloop effects bridge — makes looper's inline effect calls drive the LV2 host.
//
// THE 100%-CLONE-VIA-LV2 MECHANISM (docs/CLONE-PARITY.md):
// The vendored loopMachine.cpp calls the effects inline at four points:
//   pLivePitchWrapper->feedAudio/retrieveAudio   (pitch)     loopMachine.cpp:758-779
//   pEffectsProcessor->processSends              (delay+rev) loopMachine.cpp:801
//   pMicroRepeat->process                        (beat-rep)  loopMachine.cpp:831
//   pEffectsProcessor->processFilters            (HP/LP)     loopMachine.cpp:847
// We compile loopMachine UNCHANGED, but provide these globals as thin shims that
// forward to the in-process home-FX LV2 (which runs exactly pitch->sends->micro->
// filters, proven sample-identical to the C++ by the dubfx A/B). So the loop
// engine is looper's exact code, and the effects are a swappable LV2 plugin —
// with byte-identical audio.
//
// Enabled by -DALOOP_EFFECTS_VIA_LV2 in the aloop build. Without it, loopMachine
// links looper's original inline effect classes (the two builds are A/B-comparable
// — that's the parity harness).

#ifndef ALOOP_EFFECTS_BRIDGE_H
#define ALOOP_EFFECTS_BRIDGE_H

#ifdef ALOOP_EFFECTS_VIA_LV2

#include "../host/lv2_host.h"
#include <cstdint>

namespace aloop {

// The single home-FX LV2 host instance the shims forward to. Created at startup
// with the /effects/home/chain.lv2 bundle loaded on the home-FX core.
Lv2Host& homeFxHost();

// Set the LV2 control ports from the looper param values before a block, so the
// plugin sees the same knob state the inline effects would have. Driven from the
// same LiveParams the inline setters used (setHighpassCutoff, setReverbAmount,
// setPitchScale, setFormantDepth, microRepeatDiv, ...).
void syncLv2Params(const struct LiveParamsView& lp);

} // namespace aloop

// --- The shim globals loopMachine.cpp references, forwarding to the LV2 host ---
//
// These match looper's apcEffectsProcessor / RubberBandWrapper / microRepeat
// signatures exactly, so loopMachine.cpp compiles unchanged. Each forwards its
// stage into the home-FX LV2's port buffers and runs that portion of the chain.
// Because the LV2 runs the WHOLE chain in one pass, the shims stage the buffer at
// the right point: pitch fills the LV2 input, sends/micro/filters advance the LV2
// through its stages. In practice the cleanest wiring runs the full LV2 chain once
// per block at the microrepeat/filters point and makes the earlier per-stage shims
// no-ops that just stage the buffer — the audio result is identical because the
// chain order is identical. See CLONE-PARITY.md for the exact mapping.

struct AloopEffectsProcessor {
    void setHighpassCutoff(float v);
    void setLowpassCutoff(float v);
    void setLowpassResonance(float v);
    void setReverbAmount(float v);
    void setDelayAmount(float v);
    void setTime(float v);
    void processSends   (float* l, float* r, unsigned n, unsigned sr);   // -> LV2 delay+reverb
    void processFilters (float* l, float* r, unsigned n, unsigned sr);   // -> LV2 HP/LP (+ runs the block)
};

struct AloopPitchWrapper {
    bool isEngaged() const;
    void setPitchScale(float s);
    void setEngaged(bool on);
    void setFormantDepth(float d);
    void feedAudio    (const int16_t* l, const int16_t* r, unsigned n);   // stage LV2 input
    unsigned retrieveAudio(int16_t* l, int16_t* r, unsigned n);           // -> LV2 pitch out
    int  latencySamples() const;
};

struct AloopMicroRepeat {
    void process(int* inout, uint32_t masterPhase, uint32_t masterLoopBlocks,
                 uint8_t div, int n);                                     // -> LV2 microrepeat
};

#endif // ALOOP_EFFECTS_VIA_LV2
#endif // ALOOP_EFFECTS_BRIDGE_H
