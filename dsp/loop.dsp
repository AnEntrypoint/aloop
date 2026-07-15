// aloop loop engine — in Faust. Matches the real hardware setup:
//   * 20 INDEPENDENT loopers (each its own record buffer + play head),
//   * record / play only — NO OVERDUB (the hardware has no overdub control),
//   * each looper's play position tracks the Ableton Link grid (varispeed sync),
//   * the 20 looper outputs sum to the engine output.
//
// WHY a feedback-delay ring (not a buffer+playhead): each looper is a cycle-free
// feedback-delay ring — record replaces the loop, play recirculates it, no overdub
// means no read-modify-write, so it stays simple AND compiles. A buffer+playhead
// (rwtable) model was attempted to gain an addressable read position for the
// hardware's mark-point / loop-immediate commands, but a preserve-on-hold playhead
// requires reading a table and writing the read-back to the SAME table — a
// read-modify-write that Faust's pure-signal evaluator rejects ("endless evaluation
// cycle" / "stack overflow in eval", witnessed across 4 CI codegen attempts, see
// docs/DECISIONS.md ADR + .wfgy/lessons.md). The delay ring sidesteps RMW by
// construction, so it is the correct Faust looper. CONSEQUENCE: mark-point restart
// (SET/CLEAR_LOOP_START) and immediate re-trigger (LOOP_IMMEDIATE) — which need an
// addressable read head — are a deliberate model difference, documented in
// docs/COMMAND-SURFACE.md, not a silently dropped feature. Every OTHER loop command
// (record/play/stop/erase/clear/half-double-speed/Link-varispeed) maps 1:1.
// The discrete control (which looper, record vs play, loop length from Link) comes
// from the native shell as the per-looper param inputs.

import("stdfaust.lib");

SR       = 48000.0;
MAXLEN   = 48000 * 60;    // 60 s max loop per looper
NLOOPERS = 20;            // 20 independent loopers (the hardware setup)

// ---- one independent looper ----
// Controls (native shell drives these from MIDI + Link), indexed per looper i:
//   rec[i]   : 1 while recording looper i (replaces its loop; NO overdub)
//   play[i]  : 1 = looper i playing
//   len[i]   : looper i loop length in samples (from Link tempo for varispeed)
//   vol[i]   : looper i output level
// A looper is a one-loop-length feedback delay: while recording, write the live
// input; else recirculate (hold) the loop. There is no overdub path.
// One looper. The control labels use Faust's "[N]" group-index substitution so
// each of the 20 instances gets its own rec/play/len/vol control ("looper0/rec"
// … "looper19/rec"). Record replaces the loop; else it holds — NO overdub.
// Global controls (shared across all loopers): clear wipes every loop; speedMul
// scales the effective loop length for the momentary half/double-speed commands.
//
// ROOT CAUSE (2nd generation, THE REAL ONE): 382e775 hoisted clearAllGlobal/
// speedMulGlobal's button()/hslider() DECLARATIONS to file scope, outside the
// par(i, NLOOPERS, vgroup(...)) textually, and threaded them into oneLooper
// as ordinary function parameters -- but this did NOT stop Faust's compiler
// from still emitting 20 separate UI zones. WITNESSED via inspecting the
// generated C++ (build/loop.cpp): `grep -c '"speed"'` and `grep -c '"clear"'`
// each returned 20, and every occurrence is INSIDE its own "looper N" vgroup
// (`openVerticalBox("looper 0"); ... addHorizontalSlider("speed", ...)`),
// exactly like before the "fix". Root cause: Faust's par(...) combinator is
// a CODE-GENERATING combinator, not a runtime loop -- passing an expression
// (clearAllGlobal, itself a button() box) as an argument to oneLooper, which
// par() instantiates 20 times, means that expression is substituted/inlined
// at each of the 20 call sites. Each inlined copy of the button()/hslider()
// box gets its own UI declaration, and since the call site is lexically
// inside vgroup("looper%2i"), each inlined declaration lands inside that
// specific looper's group too. Hoisting the TEXT of the declaration outside
// the par loop doesn't matter: what determines UI-zone identity in Faust is
// where the box is ELABORATED (i.e. every place the expression is used),
// not where it is textually written once in the source. There is no Faust
// mechanism for "declare this UI control once, reference the same zone from
// many call sites" when the reference crosses a par() replication boundary --
// the language does not have that concept; par() genuinely duplicates
// whatever signal graph (including UI primitives) sits in its body.
//
// REAL FIX: stop declaring clear/speed as Faust UI controls (hslider/button)
// at all. Make them plain SIGNAL inputs to process() instead -- exactly the
// same technique already proven for prevFiltIn below (a native block-rate
// value pushed in from audio_thread.cpp's fins[] array, never a UI zone).
// A signal input threaded through par() is NOT re-elaborated per instance
// the way a UI-primitive box is -- it's just a wire, so every oneLooper
// instance genuinely reads the exact same sample-accurate value every block,
// with no zone-lookup, no FaustUI::set() suffix-matching, and no possibility
// of the compiler duplicating it. audio_thread.cpp now writes cmd/clearall
// and the computed speed multiplier directly into fins[2]/fins[3] (constant
// across the block) instead of calling fui.set("clear"/"speed", ...).

// prevFiltIn: previous block's FULLY-EFFECTED mix output (audio_thread.cpp's
// prevFiltOut, dsp/aloop.dsp's recordTap/4th process() output), native
// one-block-lag fold-in -- the same proven technique as the old glitchIn tap
// (see dsp/aloop.dsp's top-of-file comment for why this is a DEDICATED
// record-only input rather than being folded into `in`/`fin`: adding it to
// the engine's live/dry input would make it flow into `fx` again next block,
// re-entering every stage -- the WITNESSED feedback whine, see
// audio_thread.cpp's "REVERTED here" comment, now would apply to the WHOLE
// fx chain not just microStage). Routed ONLY into the record capture term
// below: recording now ALWAYS captures the fully-effected signal
// (pitch/delay/reverb/microrepeat/filters), matching the user's explicit
// requirement ("all effects should record"), not just raw dry input.
// REPLACES the old (in + glitchIn) composition entirely -- `in` (raw pre-fx
// input) is no longer part of the record term at all, since prevFiltIn one
// block later already contains everything `in` would have contributed (this
// block's `in` becomes part of next block's filtOut via the normal fx path),
// and the old glitchIn term is redundant now that prevFiltIn already carries
// post-glitch content (effects_runtime.dsp: microStage feeds both
// filterStage and rawGlitchTap, so microStage's output is upstream of and
// already baked into filtOut/recordTap).
oneLooper(in, prevFiltIn, clearAll, speedMul) = out : attachLevel
with {
    recN  = button("rec");
    playN = checkbox("play");
    lenN  = hslider("len", 48000, 64, MAXLEN, 1);
    volN  = hslider("vol", 1.0, 0.0, 1.0, 0.001);
    eraseN = button("erase");   // per-looper wipe (hardware ERASE_TRACK 0x60)
    // effective length obeys the global speed multiplier (varispeed half/double).
    // halfspeed (0.5) LENGTHENS the delay (slower/lower); doublespeed (2.0) shortens
    // it. Clamp to [1, MAXLEN] so the fdelay stays in range even at 0.5× (2×len).
    effLen = min(MAXLEN, max(1.0, lenN / speedMul));
    // wipe this loop when EITHER the global clear or this looper's erase is held.
    wipe   = max(clearAll, eraseN);
    step(loop) = record + hold
    with {
        delayed = de.fdelay(MAXLEN, effLen, loop);
        // record: capture the PREVIOUS block's fully-effected mix
        // (prevFiltIn), one-block-lag, so every recording is ALWAYS
        // effected (pitch/delay/reverb/microrepeat/filters) -- matching the
        // user's explicit requirement, not just live input passed through
        // raw. This also captures glitch/microrepeat content (matching
        // looper's "stutter becomes ... the record source",
        // loopMachine.cpp:806-833) since prevFiltIn already contains
        // microStage's output one block later, and captures SHIFT/glitch
        // -held loop content too, since the native fold already routes that
        // content through `fx` into filtOut before this tap is taken.
        // prevFiltIn never touches `in`/dry, only this record term, so it
        // structurally cannot re-enter `fx` on any later block.
        record  = prevFiltIn * recN;
        // else hold/recirculate — UNLESS wiped, which zeroes the loop.
        hold    = delayed * (1.0 - recN) * (1.0 - wipe);
    };
    loopSig = step ~ _;
    out = loopSig * playN * volN;
    // LEVEL meter: an hbargraph UI OUTPUT (never fui.set() from ParamStore --
    // read-only via fui.get(), same pattern as the existing rec/play/vol
    // telemetry reads in audio_thread.cpp), fed via Faust's attach() idiom so
    // the meter signal rides along the real audio signal without adding a
    // second audible output channel. Raw abs-peak magnitude in the looper's
    // own [0,1] float range (not dB, not looper's raw s32 scale) -- matching
    // looper's vuLow/vuMid/vuHigh thresholds means the C++ side documents its
    // own equivalent thresholds in aloop's normalized range (see apc_leds.cpp).
    // ba.slidingMax gives a fast-rise/slow-decay envelope (a real "peak
    // meter" shape rather than a raw instantaneous sample, which would
    // flicker the LED color every block) over a ~4096-sample (~85ms @48kHz)
    // window.
    levelMeter = hbargraph("level", 0.0, 1.0);
    // ba.slidingMax(n, maxN): sliding window max over the last n samples,
    // maxN is the compile-time buffer-size bound (n <= maxN). Window ~4096
    // samples (~85ms @48kHz), bound equal to the window since it's a fixed
    // compile-time constant here, not runtime-variable.
    attachLevel(x) = attach(x, abs(x) : ba.slidingMax(4096, 4096) : levelMeter);
};

// The engine outputs (dry-thru, loop-sum) SEPARATELY rather than pre-summed, so
// the caller (aloop.dsp) can implement looper's SHIFT-held monitor-fold: fold
// the loop-sum into the effect chain's input while complementarily suppressing
// the dry loop contribution at the final mix (loopMachine.cpp:709-730's
// g_fold/g_dry crossfade). `par` with a "%2i"-labelled vgroup gives each
// instance its own addressable controls (looper00/rec … looper19/rec).
//   in -> [ thru , (looper0 + looper1 + … + looper19) ]
// vgroup label "looper%2i" → Faust substitutes the par index, giving group names
// "looper 0" … "looper19" (a space for single digits). The native shell's
// targetToZone normalizes to this exact form so each looper is addressable.
// Second process() input (prevFiltIn, previous block's fully-effected mix
// output) is broadcast to every looper's record-only tap (see oneLooper's
// comment) -- it never appears in the dry-thru output (only `in` does), so
// the fully-effected signal is recordable but never re-enters the live/dry
// path (which would flow back into `fx` next block).
// clearAllGlobal/speedMulGlobal are evaluated ONCE here, OUTSIDE any vgroup,
// then passed as ordinary parameters into every oneLooper instance -- this is
// what actually makes them single shared zones (see oneLooper's comment above
// for why referencing them by bare name from INSIDE the vgroup-wrapped par
// failed to do this).
// clearAll/speedMul are now genuine process() SIGNAL inputs (see the ROOT
// CAUSE comment above oneLooper) -- plain wires, not UI-declared button()/
// hslider() boxes, so par()'s per-instance code generation cannot duplicate
// them into 20 separate zones. They are broadcast identically to every
// oneLooper instance the same way prevFiltIn already is.
loopEngine(in, prevFiltIn, clearAll, speedMul) = in, (par(i, NLOOPERS, vgroup("looper%2i", oneLooper(in, prevFiltIn, clearAll, speedMul))) :> _);

process(in, prevFiltIn, clearAll, speedMul) = loopEngine(in, prevFiltIn, clearAll, speedMul);   // (dry, loopSum) — two outputs, see aloop.dsp's fold mix
