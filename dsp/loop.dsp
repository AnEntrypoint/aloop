// aloop loop engine — in Faust. Matches the real hardware setup:
//   * 20 INDEPENDENT loopers (each its own record buffer + play head),
//   * record / play only — NO OVERDUB (the hardware has no overdub control),
//   * each looper's play position tracks the Ableton Link grid (varispeed sync),
//   * the 20 looper outputs sum to the engine output.
//
// WHY Faust: each looper is a cycle-free feedback-delay ring (record replaces the
// loop; play recirculates it; no overdub means no read-modify-write, so it stays
// simple). Feasibility witnessed in loop_min.dsp. The discrete control (which
// looper, record vs play, loop length from Link) comes from the native shell as
// the per-looper param inputs.

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
clearAll = button("clear");
speedMul = hslider("speed", 1.0, 0.25, 4.0, 0.001);   // 0.5 = half, 2.0 = double

oneLooper(in) = loopSig * playN * volN
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
        record  = in * recN;                              // record: capture input
        // else hold/recirculate — UNLESS wiped, which zeroes the loop.
        hold    = delayed * (1.0 - recN) * (1.0 - wipe);
    };
    loopSig = step ~ _;
};

// The engine: live input (thru) + the sum of 20 INDEPENDENT loopers. `par` with
// a "%2i"-labelled vgroup gives each instance its own addressable controls
// (looper00/rec … looper19/rec). All loopers see the same live input; the input
// is fanned to each looper and to the dry thru, then everything sums.
//   in -> [ thru | looper0 | looper1 | … | looper19 ] -> sum
// vgroup label "looper%2i" → Faust substitutes the par index, giving group names
// "looper 0" … "looper19" (a space for single digits). The native shell's
// targetToZone normalizes to this exact form so each looper is addressable.
loopEngine = _ <: (_ , par(i, NLOOPERS, vgroup("looper%2i", oneLooper))) :> _ ;

process(in) = loopEngine(in);
