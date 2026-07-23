// Vinyl surface noise — filtered noise bed mixed under the dry signal. New
// lofi-fx-bank stage (see .gm/prd.yml lofi-fx-vinyl): NOISE ONLY, by explicit
// design-session decision — this stage carries no pitch/speed "wow" wobble at
// all; that's flutter.dsp's job (a separate, faster-rate modulated-delay
// stage). Keeping the two concerns in separate files means either can be
// dialed in/out independently on the lofi bank.
//
// Real vinyl surface noise (dust/groove-wear crackle) is not flat white noise:
// it reads as a low-mid "hiss bed" with a crackly high-frequency texture
// riding on top, roughly like pink-ish noise run through a broad low-mid bump
// plus a bit of top-end air for the crackle transients. We approximate that
// shape with two one-pole filters on a white noise source: a lowpass to tame
// harsh top end into a duller "surface hum", and a highpass to carve out
// sub-bass rumble that real vinyl noise doesn't carry (turntable rumble is a
// separate, much lower-level phenomenon we're not modeling here) — leaving a
// band-limited noise bed centered in the low-mids with just enough high
// content left through to read as "crackle" rather than "hum".
//
// VINYLAMT is the single runtime control (0..1, default 0), scaling ONLY the
// noise bed's contribution to the output — the noise generator and its EQ
// shape are fixed constants tuned once for a good-sounding bed at any amount,
// same "one knob, no dozen sub-params" convention as flanger.dsp's FLANGEAMT.
//
// Hard requirement: at VINYLAMT=0 output must be BYTE-IDENTICAL to input (see
// param_mapping.md's documented passthrough invariant, maxAbs=0). The noise
// generator and filters still run underneath, but `x + VINYLAMT*noiseBed` means
// the multiply zeroes the entire contribution exactly at amt=0, leaving `x`
// untouched by any float rounding from the noise/filter math.

import("stdfaust.lib");

SR = 48000.0;

// ---- Runtime control ----
VINYLAMT = hslider("VINYLAMT", 0.0, 0.0, 1.0, 0.01);

// ---- Fixed noise-bed shaping constants ----
// Lowpass at ~3.5kHz: keeps the bed from reading as harsh/hissy broadband
// white noise, giving it the duller "surface hum" character real vinyl noise
// has (most of a record's audible surface noise energy sits well below the
// top octave of full-bandwidth white noise).
LP_CUTOFF_HZ = 3500.0;

// Highpass at ~150Hz: carves out sub-bass rumble/DC-ish content a pure noise
// generator otherwise dumps into the low end — real groove-wear crackle and
// dust noise doesn't carry meaningful sub-150Hz energy, so leaving it in
// would just read as a muddy low-end hum rather than "vinyl".
HP_CUTOFF_HZ = 150.0;

// Bed level: white noise is full-scale; a noise bed mixed at unity would
// swamp the dry signal even at low VINYLAMT. This trims the bed to a
// realistic "surface noise under the music" level before the runtime
// crossfade scales it further by VINYLAMT.
BED_LEVEL = 0.12;

// Band-limited noise bed: white noise -> one-pole LP (dulls the hiss) ->
// one-pole HP (removes rumble) -> trimmed to bed level.
noiseBed = no.noise : fi.lowpass(1, LP_CUTOFF_HZ) : fi.highpass(1, HP_CUTOFF_HZ) : *(BED_LEVEL);

// Wet crossfade: at VINYLAMT=0 this reduces to exactly `x + 0*noiseBed`, i.e.
// bit-exact passthrough (the noise/filter chain still runs, but its output is
// multiplied by zero and contributes nothing to the sum).
vinylMix(x) = x + VINYLAMT * noiseBed;

process = _ <: vinylMix;
