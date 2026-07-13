// aloop home stack — the loop engine + the dubfx effect chain as ONE Faust
// program (both are Faust, so they compose directly). This is the fixed home
// stack that runs on Core 1; the user's swappable effect is a separate LV2 on
// Core 3 (loaded by the in-process host). See docs/ARCHITECTURE.md.
//
// Signal: input -> loop.dsp (20 independent record/play loopers, no overdub) -> the dubfx
// effect chain (pitch/delay/reverb/microrepeat/filters) -> output.
//
// Composing this way means the ENTIRE home audio path is one Faust compile — the
// maintainability win: change a knob mapping or a stage in Faust, rebuild, done.
// No hand-written C++ DSP anywhere in the home stack.

import("stdfaust.lib");

// The loop engine (20 independent record/play loopers, no overdub). Outputs
// (dry, loopSum) separately — see loop.dsp — so the SHIFT-held monitor-fold
// below can route the loop sum into the effect input while complementarily
// suppressing the dry loop contribution, matching looper's loopMachine.cpp:
// 709-730 fold/dry crossfade (folded loops are heard once, through the
// effects, with no loudness jump; released, they resume normal dry output).
loop = component("loop.dsp");
// The RUNTIME effects chain — the verified dubfx stages, but with the params as
// live UI controls (dsp/effects_runtime.dsp) so the remappable control map can
// set the knobs at runtime (the dubfx chain.dsp bakes them as constants; that
// stays the A/B reference, this is the runtime variant).
fx   = component("effects_runtime.dsp");

// MONITORFOLD (bound by ApcGrid::onShiftPress/Release to fx/monitorfold, held
// while SHIFT is down): 0 = normal (loops dry, no fold), 1 = fold loops into
// the effect input. si.smoo is Faust's standard one-pole smoother, giving the
// same click-free ramp looper's MONITOR_GATE_STEP (1/16 per block) achieves by
// a different means -- a continuous exponential approach instead of a fixed
// per-block linear step, functionally equivalent for a hold/release gesture.
monitorFold = hslider("MONITORFOLD", 0.0, 0.0, 1.0, 1.0) : si.smoo;

// Home stack: `loop` is called ONCE (it is stateful -- a feedback ring per
// looper -- so two separate calls would desync into two independent
// instances) and its two outputs (dry, loopSum) are consumed by a single
// downstream block using Faust's `_,_` fan-out over both signals at once.
// Fold loopSum into the effect input (mirrors loopMachine.cpp:738's
// m_input_buffer += loopOut*fg) while complementarily gating the dry loop
// contribution into the final mix (m_loopOutputGain = 1-fold); fx runs on the
// folded signal; the gated dry loop sums back in afterward, matching looper's
// final mix.
foldMix(dry, loopSum) = (dry + loopSum*monitorFold : fx) + loopSum*(1.0-monitorFold);
process = loop : foldMix;
