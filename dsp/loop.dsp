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
// ROOT CAUSE FIXED HERE: these MUST be computed ONCE, outside the par(i,
// NLOOPERS, vgroup(...)) instantiation of oneLooper below, and threaded IN as
// ordinary function parameters -- NOT referenced by bare name from inside
// oneLooper's own body. WITNESSED via a native startup diagnostic (dumping
// every FaustUI zone containing "speed"/"clear"): declaring clearAll/speedMul
// at file scope but only ever REFERENCING them (not passing them as
// parameters) inside oneLooper, which par(...) instantiates 20 times each
// wrapped in its own vgroup("looper%2i"), does NOT produce one shared zone --
// Faust has no notion of "the same UI element" reused across separate
// expression instances; each textual button()/hslider() occurrence is its
// own box, so the vgroup wrapping produced 20 SEPARATE zones ("looper
// 0/clear" .. "looper19/clear", "looper 0/speed" .. "looper19/speed").
// audio_thread.cpp's fui.set("clear", ...) / fui.set("speed", ...) (a bare,
// unqualified name) resolved via FaustUI::set's first-match suffix search,
// which only ever found ONE of the 20 duplicated zones -- so a "half/double
// speed" button press only ever changed ONE looper's playback rate (silently
// inaudible unless that one specific looper happened to be playing), and
// cmd/clearall's DSP-side wipe only ever hit ONE looper's ring (the C++ side
// had to separately erase every looper by its own unambiguous "looperN/erase"
// name to compensate -- see apc_grid.cpp's onClearAll). Passing clearAll/
// speedMul as parameters (computed once, above/outside the par) means every
// oneLooper instance receives the SAME already-evaluated signal, with no
// per-instance re-declaration -- exactly one real UI zone each, addressable
// by its plain top-level name, matching docs/COMMAND-SURFACE.md's documented
// "engine-global" intent.
clearAllGlobal = button("clear");
speedMulGlobal = hslider("speed", 1.0, 0.25, 4.0, 0.001);   // 0.5 = half, 2.0 = double

// glitchIn: previous block's post-glitch (microrepeat) tap, native one-block-lag
// fold-in (see audio_thread.cpp's prevGlitchTap + dsp/aloop.dsp's top-of-file
// comment on why this is a DEDICATED record-only input rather than being
// folded into `in`/`fin`: adding it to the engine's live/dry input would make
// it flow into `fx` again next block (re-entering microStage → the WITNESSED
// feedback whine, see audio_thread.cpp's "REVERTED here" comment). Routed
// ONLY into the record capture term below, so glitch content becomes
// recordable into a new loop without ever being reprocessed by microStage.
oneLooper(in, glitchIn, clearAll, speedMul) = out : attachLevel
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
        // record: capture the live input PLUS the previous block's glitch tap,
        // so a loop armed while microrepeat is engaged captures the STUTTERED
        // audio (matching looper's "stutter becomes ... the record source",
        // loopMachine.cpp:806-833) without microStage ever seeing its own
        // output again (glitchIn never touches `in`/dry, only this record sum).
        record  = (in + glitchIn) * recN;
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
// Second process() input (glitchIn) is broadcast to every looper's record-only
// tap (see oneLooper's comment) -- it never appears in the dry-thru output
// (only `in` does), so the glitch content is recordable but never re-enters
// the live/dry path (which would flow back into `fx`/microStage next block).
// clearAllGlobal/speedMulGlobal are evaluated ONCE here, OUTSIDE any vgroup,
// then passed as ordinary parameters into every oneLooper instance -- this is
// what actually makes them single shared zones (see oneLooper's comment above
// for why referencing them by bare name from INSIDE the vgroup-wrapped par
// failed to do this).
loopEngine(in, glitchIn) = in, (par(i, NLOOPERS, vgroup("looper%2i", oneLooper(in, glitchIn, clearAllGlobal, speedMulGlobal))) :> _);

process(in, glitchIn) = loopEngine(in, glitchIn);   // (dry, loopSum) — two outputs, see aloop.dsp's fold mix
