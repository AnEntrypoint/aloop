// Flanger — short modulated delay line mixed with dry signal, feedback path
// for resonant "jet swoosh" character. New guitar-bank stage (see
// guitar-fx-dsp-flanger in .gm/prd.yml): top-row knob 1, "flange amnt".
//
// Classic flange topology: y = x + wet*fb(x), where fb(x) is a comb formed by
// feeding a short (1-10ms) delay, modulated by a low-rate LFO, back into
// itself. The depth range (1-10ms) is the textbook flange range — shorter
// than chorus (typically 10-30ms) and long enough that the swept comb notches
// land in the audible spectrum rather than collapsing into a fixed EQ.
//
// FLANGEAMT is exposed as a single runtime hslider (0..1, default 0) that
// scales ONLY the wet mix — the internal LFO rate/depth/feedback are fixed
// constants tuned once for a good-sounding sweep at any wet amount, per the
// task spec ("meant to sound good at any FLANGEAMT setting, not have a dozen
// exposed knobs").
//
// Hard requirement: at FLANGEAMT=0 the output must be BYTE-IDENTICAL to the
// input (see param_mapping.md's documented passthrough invariant, maxAbs=0).
// This is why the wet path is `x + FLANGEAMT*(delayed - x)`-shaped (an
// explicit crossfade multiplied by the knob) rather than always summing the
// delay line at some fixed ratio — at FLANGEAMT=0 the multiply zeroes the
// entire wet contribution exactly, leaving `x` untouched by any float
// rounding from the delay/LFO/feedback math.

import("stdfaust.lib");

SR = 48000.0;

// ---- Runtime control ----
FLANGEAMT = hslider("FLANGEAMT", 0.0, 0.0, 1.0, 0.01);

// ---- Fixed internal LFO constants ----
// Rate: 0.25Hz — slow enough that the sweep feels like a continuous "whoosh"
// rather than a fast vibrato/warble (fast rates read as tremolo-ish, not
// flange). Classic flange pedals sit in the 0.1-1Hz range; 0.25Hz is a
// well-worn middle choice.
LFO_RATE_HZ = 0.25;

// Depth: modulate between 1ms and 10ms — the canonical flange delay-depth
// window (below ~1ms the comb teeth move out of audible range too fast /
// alias; above ~10ms it starts sounding like chorus/slapback instead of
// flange). Centered at ~5.5ms with +-4.5ms swing.
DEPTH_MIN_MS = 1.0;
DEPTH_MAX_MS = 10.0;
DEPTH_CENTER_MS = (DEPTH_MIN_MS + DEPTH_MAX_MS) / 2.0;
DEPTH_SWING_MS  = (DEPTH_MAX_MS - DEPTH_MIN_MS) / 2.0;

// Feedback amount: fixed moderate resonance for the classic "metallic" flange
// character without runaway self-oscillation (kept well under 1.0).
FEEDBACK = 0.5;

MAXD = 1024; // >= 10ms at 48kHz (480 samples) with margin for the LFO swing

// Delay length in samples, modulated by a bipolar sine LFO (os.osc gives a
// unipolar-range... use os.osc directly: -1..1, so scale by swing and offset
// by center, then convert ms -> samples).
lfo = os.osc(LFO_RATE_HZ); // -1..1
delayMs = DEPTH_CENTER_MS + lfo * DEPTH_SWING_MS;
delaySamples = delayMs * SR / 1000.0;

// Fractional delay read via 2-point linear interpolation (de.fdelay handles
// this internally); feedback closes the loop through the flanged tap itself,
// matching the classic flanger-with-feedback structure (not just a static
// modulated delay).
flangeFC(x) = (loop ~ _) : fracRead
with {
    fracRead(w) = de.fdelay(MAXD, delaySamples, w);
    loop(w) = x + FEEDBACK * fracRead(w);
};

// Wet crossfade: at FLANGEAMT=0 this reduces to exactly `x + 0*(...)`, i.e.
// bit-exact passthrough (the delay/LFO/feedback machinery still runs, but its
// output is multiplied by zero and contributes nothing to the sum).
flangeMix(x) = x + FLANGEAMT * (flangeFC(x) - x);

process = _ <: flangeMix;
