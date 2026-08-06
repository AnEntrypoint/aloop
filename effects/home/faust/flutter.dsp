// Tape flutter — fast speed/pitch wobble via a modulated fractional delay
// line. New lofi-fx-bank stage (see .gm/prd.yml lofi-fx-flutter).
//
// "Flutter" and "wow" are the two classic tape speed-instability artifacts,
// distinguished by LFO rate: wow is slow (~0.5-2Hz, a lazy pitch drift), while
// flutter is fast (~5-15Hz, a quicker warble/shimmer). This lofi bank has NO
// wow component anywhere — vinyl.dsp was explicitly scoped as noise-only in
// its own design session — so flutter.dsp is the ONLY speed-wobble effect in
// the whole bank. Its rate therefore sits squarely in the flutter register
// (not drifted down toward wow) so the bank still has a wobble character
// available, just the fast one.
//
// Implemented the standard cheap real-time way: a small modulated delay line
// with fractional-sample interpolated read (de.fdelay) and an LFO driving the
// read-tap position. Modulating a short delay's length at audio-adjacent rates
// produces the same Doppler-ish pitch wobble true varispeed resampling would,
// without needing an actual resampler.
//
// FLUTTERAMT is the single runtime control (0..1, default 0), scaling the
// modulation depth (LFO swing) directly — deeper FLUTTERAMT means a wider
// pitch excursion, matching how a real degraded transport's speed error grows.
// The LFO rate is a fixed constant (varying it isn't part of this task's
// spec); FLUTTERAMT only touches depth.
//
// Hard requirement: at FLUTTERAMT=0 output must be BYTE-IDENTICAL to input
// (see param_mapping.md's documented passthrough invariant, maxAbs=0). At
// depth=0 the delay length collapses to a fixed constant CENTER_MS with zero
// LFO swing — de.fdelay at a compile-time-equal, per-sample-constant fractional
// length still passes samples through with interpolation math in play, which
// is NOT guaranteed bit-exact against raw input. So passthrough is enforced
// the same way as flanger.dsp: an explicit `x + FLUTTERAMT*(wet - x)` crossfade,
// whose multiply-by-zero exactly zeroes the wet contribution at amt=0,
// leaving `x` untouched by any delay-line float rounding.

import("stdfaust.lib");

SR = 48000.0;

// ---- Runtime control ----
FLUTTERAMT = hslider("FLUTTERAMT", 0.0, 0.0, 1.0, 0.01);

// ---- Fixed internal LFO rate ----
// 9Hz sits in the middle of the classic 5-15Hz flutter register — fast enough
// to read as a warble/shimmer rather than a slow pitch drift (which would be
// wow, deliberately not modeled here), slow enough not to alias into an
// audible sideband/ring-mod buzz the way a much-higher rate would.
LFO_RATE_HZ = 9.0;

// ---- Depth mapping ----
// Center delay of 8ms gives enough headroom on both sides of the swing for
// the modulated tap to stay comfortably away from 0 (which would clip the
// fractional read against the block edge) while remaining short enough that
// the modulation reads as "wobble" rather than a discrete slapback echo.
// Max swing of +-3ms at FLUTTERAMT=1 is a deep-but-still-musical flutter depth
// (a real worn/damaged tape transport can wobble roughly this much); it scales
// linearly down to 0 swing as FLUTTERAMT -> 0.
CENTER_MS = 8.0;
MAX_SWING_MS = 3.0;

MAXD = 1024; // >= (CENTER_MS+MAX_SWING_MS) worth of samples at 48kHz, with margin

lfo = os.osc(LFO_RATE_HZ); // -1..1
swingMs = FLUTTERAMT * MAX_SWING_MS;
delayMs = CENTER_MS + lfo * swingMs;
delaySamples = delayMs * SR / 1000.0;

// Fractional-sample modulated delay read (no feedback — flutter is a direct
// speed-wobble artifact on the signal itself, not a resonant comb like the
// flanger's feedback path).
flutterWet(x) = de.fdelay(MAXD, delaySamples, x);

// Wet crossfade: at FLUTTERAMT=0, swingMs=0 so the delay length is the fixed
// CENTER_MS constant with no modulation, AND the crossfade multiply zeroes the
// (wet - x) difference exactly, guaranteeing bit-exact passthrough regardless
// of any residual float rounding in the fdelay interpolation math.
flutterMix(x) = x + FLUTTERAMT * (flutterWet(x) - x);

process = _ <: flutterMix;
