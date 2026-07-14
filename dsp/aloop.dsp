// aloop home stack — the loop engine + the dubfx effect chain as ONE Faust
// program (both are Faust, so they compose directly). This is the fixed home
// stack that runs on Core 1; the user's swappable effect is a separate LV2 on
// Core 3 (loaded by the in-process host). See docs/ARCHITECTURE.md.
//
// Signal: input -> loop.dsp (20 independent record/play loopers, no overdub) -> the dubfx
// effect chain (pitch/delay/reverb/microrepeat/filters) -> output.
//
// SHIFT-held monitor-fold moved OUT of this Faust graph and into
// audio_thread.cpp's worker() (native, block-rate): folding loopSum into `fx`'s
// input HERE (same-block, live-only) could never be recorded, since loop.dsp's
// record path runs BEFORE this fold -- a Faust `~`-recursion fix for that was
// tried and WITNESSED to silently break basic dry passthrough live on real
// hardware (git history: 90083c4 -> 3b8bd5e revert). The native fix instead
// feeds the PREVIOUS block's RAW loop-engine output (this program's second
// output, below) back into `fin` (the engine's own input) before
// faustHome.compute() runs, so THIS Faust graph stays exactly as
// simple/proven as before -- loop.dsp's record path sees the fold because
// it's baked into the input signal itself, one block later, matching
// looper's real one-block-lag fold (loopMachine.cpp:709-741) exactly.
//
// Composing this way means the ENTIRE home audio path is one Faust compile — the
// maintainability win: change a knob mapping or a stage in Faust, rebuild, done.
// No hand-written C++ DSP anywhere in the home stack.

import("stdfaust.lib");

// The loop engine (20 independent record/play loopers, no overdub).
loop = component("loop.dsp");
// The RUNTIME effects chain — the verified dubfx stages, but with the params as
// live UI controls (dsp/effects_runtime.dsp) so the remappable control map can
// set the knobs at runtime (the dubfx chain.dsp bakes them as constants; that
// stays the A/B reference, this is the runtime variant).
fx   = component("effects_runtime.dsp");

// `loop`'s two outputs (dry, loopSum) are consumed by `mixAndFx`, which sums
// them for the live-heard signal (through `fx`) while ALSO passing loopSum
// through UNCHANGED as this program's SECOND output -- a raw audio tap read
// back natively via AloopLoopDsp's fouts[1] in audio_thread.cpp, so the
// native monitor-fold mix (see top-of-file comment) can fold the loop's OWN
// raw output into next block's input, matching looper's m_input_buffer +=
// m_output_buffer*fg (loopMachine.cpp:738) -- folding the RAW loop output,
// not the fully-effected wet signal (which would compound effects every
// block the fold is held, a real difference from looper this avoids).
mixAndFx(dry, loopSum) = (dry+loopSum : fx), loopSum;
process(in) = in : loop : mixAndFx;
