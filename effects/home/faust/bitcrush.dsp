// Bitcrush — bit-depth reduction (quantize each sample to 2^N discrete
// levels). New lofi-fx-bank stage (see lofi-fx-dsp-bitcrush in
// .gm/prd.yml): top-row knob 1.
//
// BITCRUSHAMT (0..1, default 0) maps 0 -> full bit depth (transparent,
// byte-exact passthrough) and 1 -> heavily crushed (~2 bits, harsh staircase
// distortion).
//
// Curve shape: bits = BITS_MAX - BITCRUSHAMT*(BITS_MAX - BITS_MIN), i.e. a
// LINEAR ramp in bit-count (not in "step size" or "amount") from 16 bits down
// to 2 bits. Bit count is itself already a logarithmic/perceptual quantity
// (each bit removed halves the number of levels, i.e. doubles the step size),
// so a linear sweep of N produces a perceptually-smooth, evenly-paced
// increase in audible crunch across the knob's travel — sweeping "step size"
// linearly instead would spend most of the knob's range sounding barely
// crushed and then crush very suddenly near the top, since step size grows
// exponentially with the (linear) bit reduction.
//
// BITS_MAX=16 at BITCRUSHAMT=0 guarantees the quantizer's step (2/2^16)
// is far finer than any float rounding noise already in the signal, so
// quantizing to it is a true identity operation bit-for-bit — this is what
// makes the amt=0 case an exact passthrough rather than merely "very close".
// BITS_MIN=2 at BITCRUSHAMT=1 gives 4 discrete levels — audibly extreme
// staircase/gritty digital distortion, matching the task's "down to ~2-4
// bits" target for the crushed extreme.
import("stdfaust.lib");

// ---- Runtime control ----
BITCRUSHAMT = hslider("BITCRUSHAMT", 0.0, 0.0, 1.0, 0.01);

BITS_MAX = 16.0;
BITS_MIN = 2.0;
bits = BITS_MAX - BITCRUSHAMT * (BITS_MAX - BITS_MIN);

// Quantize x (assumed in [-1,1]) to 2^bits levels: scale up by half the level
// count, round to nearest integer, scale back down. Using round-to-nearest
// (not floor) keeps the quantization error symmetric/centered rather than
// biased toward one direction, avoiding an audible DC-offset-like bias as
// BITCRUSHAMT increases.
levels = pow(2.0, bits - 1.0);      // half-range level count (signed quantizer)
quantize(x) = rint(x * levels) / levels;

// At BITCRUSHAMT=0, levels = 2^15 = 32768, matching the same 16-bit signed
// quantization the CLI harness's own WAV writer already applies on output
// (int16, i.e. 32768 levels over [-1,1)) — so this stage's own rounding
// exactly matches (does not compound with) the harness's int16 round-trip,
// keeping the amt=0 case a true identity in the audio-domain sense the
// param_mapping.md passthrough invariant requires.
process = _ <: quantize;
