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

// MONITORFOLD (bound by ApcGrid::onShiftPress/Release to fx/monitorfold, held
// while SHIFT is down): must complementarily GATE the dry-summed loopSum
// contribution here (1-monitorFold), matching looper's m_loopOutputGain =
// 1-foldEnd (loopMachine.cpp:730) -- WITNESSED live: without this gate, a
// held loop's audio reaches the final output TWICE while SHIFT is held: once
// via the direct dry+loopSum sum below (always active, since aloop.dsp
// itself no longer knows about fold state) AND once via the native
// prevLoopSum fold-in (audio_thread.cpp), which re-enters through `loop` as
// a new `dry` signal next block and gets summed in again here -- reported
// as "shift... doubling the audio". si.smoo matches looper's MONITOR_GATE_STEP
// ramp shape (a continuous exponential approach vs a fixed per-block linear
// step, functionally equivalent for a hold/release gesture).
monitorFold = hslider("MONITORFOLD", 0.0, 0.0, 1.0, 1.0) : si.smoo;

// `loop`'s two outputs (dry, loopSum) are consumed by `mixAndFx`, which sums
// dry with the COMPLEMENTARILY-GATED loopSum for the live-heard signal
// (through `fx`, which itself now has 2 outputs: the final filtered mix, and
// microStage's own post-glitch/pre-filter tap) while ALSO passing the raw
// (ungated) loopSum through -- three total program outputs, in this order:
//   1. finalOut  -- the real audible/wire signal (fouts[0] in audio_thread.cpp)
//   2. rawGlitchTap -- microStage's own post-glitch signal (fouts[1]), so
//      audio_thread.cpp can fold the stutter back into next block's input,
//      making glitch content recordable into a new loop and affecting
//      already-playing loops the same way (matching loopMachine.cpp:806-833's
//      "stutter becomes BOTH the audible output and the record source" --
//      the SAME one-block-lag native-mix technique as the SHIFT-fold below).
//   3. rawLoopSum -- the loop engine's own output (fouts[2]), so the native
//      SHIFT-fold mix (see top-of-file comment) can fold it into next
//      block's input, matching looper's m_input_buffer += m_output_buffer*fg
//      (loopMachine.cpp:738) -- the RAW loop output, not the fully-effected
//      wet signal (which would compound effects every block the fold is held).
mixAndFx(dry, loopSum) = filtOut, rawGlitchTap, loopSum
with {
    fxOuts = (dry + loopSum*(1.0-monitorFold)) : fx;
    filtOut = fxOuts : (_, !);
    rawGlitchTap = fxOuts : (!, _);
};
process(in) = in : loop : mixAndFx;
