// aloop home stack — the loop engine + the dubfx effect chain as ONE Faust
// program (both are Faust, so they compose directly). This is the fixed home
// stack that runs on Core 1; the user's swappable effect is a separate LV2 on
// Core 3 (loaded by the in-process host). See docs/ARCHITECTURE.md.
//
// RE-ARCHITECTED (was: loop.dsp -> fx, i.e. effects ran on the loop engine's
// OUTPUT). WITNESSED via direct comparison against ../looper's real block
// order (loopMachine.cpp:697-901): SHIFT-fold (709-741) -> live-pitch
// (743-786) -> effects sends (788-804) -> microrepeat (806-833) -> filters
// (835-850) -> cbWriteBlock (901, the RECORD tap) -- filters and every other
// effect stage run BEFORE the record tap, on the SAME mutated m_input_buffer
// a new recording captures ("record the WET block... so loops record exactly
// what the musician hears," loopMachine.cpp:852-854's own comment). aloop's
// prior order (loop.dsp's record tap INSIDE loop, then fx running on loop's
// OUTPUT afterward) meant NOTHING in fx was ever pre-record: microrepeat could
// never be captured into a new loop, and SHIFT-fold could never be recorded
// either -- exactly the two bugs reported ("shift routing... it should record
// that", "glitch effects... it should record glitches"). Fixed by moving `fx`
// BEFORE `loop`, with the fold computed here (not inside loop.dsp) using the
// PREVIOUS block's already-computed loopSum (one-block-delayed feedback, `~`),
// matching looper's own one-block-lag fold (m_input_buffer mutated THIS block,
// consumed by update() NEXT block) rather than requiring an unrealizable
// same-sample Faust cycle across component() boundaries.
//
// Signal: input -> SHIFT-fold (with prior loopSum) -> the dubfx effect chain
// (pitch/delay/reverb/microrepeat/filters) -> loop.dsp (20 independent
// record/play loopers, no overdub; records this fully-processed WET signal,
// matching looper's cbWriteBlock semantics) -> output (wet mix + dry-loop
// complement, same complementary crossfade as before).
//
// Composing this way means the ENTIRE home audio path is one Faust compile — the
// maintainability win: change a knob mapping or a stage in Faust, rebuild, done.
// No hand-written C++ DSP anywhere in the home stack.

import("stdfaust.lib");

// The loop engine (20 independent record/play loopers, no overdub). Outputs
// (dry-of-what-it-was-given, loopSum) — see loop.dsp. `loop`'s `in` argument
// is now the WET (fold+effects-processed) signal, not raw input, so its
// record path captures exactly what looper's cbWriteBlock does.
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

// Fold + effects run ONCE per sample, BEFORE `loop`, on a feedback loop of
// the PRIOR sample's loopSum (a one-sample delay via `~`, tighter than
// looper's one-BLOCK lag but the same causal shape: this instant's fold input
// is last instant's loop output, never a same-instant self-reference).
// `~` requires the recursed signal's arity to match; `preLoop` takes `in`
// (fresh audio) and feeds back its own prior loopSum-fold contribution.
preLoop(in, priorLoopSum) = (in + priorLoopSum*monitorFold) : fx;

// Home stack: `loop` is called ONCE (it is stateful -- a feedback ring per
// looper -- so two separate calls would desync into two independent
// instances). `loopStage` outputs (finalOut, loopSum); Faust's `~` feeds
// ONLY the signal(s) matching the recursed side's declared width back into
// the SAME block's second input on the NEXT sample -- here that's `loopSum`
// alone (1 signal), so `loopStage ~ _` recurses just that one output back as
// `priorLoopSum`, leaving `finalOut` free to become the block's real (single)
// external output. The trailing `!` on the with-block drops loopSum from the
// EXTERNAL output (it still reaches the feedback tap via `~`, it just isn't
// also exposed as a second top-level process() output -- process() must stay
// mono in/out to match how audio_thread.cpp wires this component).
process(in) = in : loopStage ~ _ : (_, !)
with {
    // loopStage(in, priorLoopSum) -> (finalOut, loopSum): runs preLoop (fold+fx)
    // on `in` using the PRIOR loopSum, feeds that WET signal into `loop`
    // (which now records it), and complementarily adds the un-folded portion
    // of loopSum back in at the end. Returns (finalOut, loopSum) so `~`'s
    // feedback carries loopSum forward to the next sample's preLoop.
    loopStage(in, priorLoopSum) = wetIn : loop : finalMix
    with {
        wetIn = preLoop(in, priorLoopSum);
        finalMix(dry, loopSum) = (dry + loopSum*(1.0-monitorFold)), loopSum;
    };
};
