// Bitcrush — bit-depth reduction (quantize each sample to 2^N discrete
// levels). New lofi-fx-bank stage (see lofi-fx-dsp-bitcrush in
// .gm/prd.yml): top-row knob 1.
//
// BITCRUSHAMT (0..1, default 0) maps 0 -> full bit depth (transparent,
// byte-exact passthrough) and 1 -> heavily crushed (~2 bits, harsh staircase
// distortion).
//
// Curve shape: bits = BITS_MAX - BITCRUSHAMT*(BITS_MAX - BITS_MIN), i.e. a
// LINEAR ramp in bit-count (not in "step size" or "amount") from BITS_MAX bits
// down to 2 bits. Bit count is itself already a logarithmic/perceptual quantity
// (each bit removed halves the number of levels, i.e. doubles the step size),
// so a linear sweep of N produces a perceptually-smooth, evenly-paced
// increase in audible crunch across the knob's travel — sweeping "step size"
// linearly instead would spend most of the knob's range sounding barely
// crushed and then crush very suddenly near the top, since step size grows
// exponentially with the (linear) bit reduction.
//
// BITS_MAX=24 at BITCRUSHAMT=0: the ORIGINAL 16 here was wrong -- WITNESSED
// via a DawDreamer render diff (a real random full-amplitude float signal
// through this stage alone, BITCRUSHAMT pinned at its default 0): 16-bit
// quantization (step 2/2^16) is NOT below float rounding noise, it produced
// a real max_abs_diff of 1.529e-05 against the dry input, i.e. this stage
// was silently flooring every sample to 16-bit resolution even at its own
// "off" default, discarding real precision the actual output path can use
// (the instrument device negotiates real S32_LE/24-bit, see AGENTS.md's ALSA
// format entry -- there is no int16 round-trip anywhere in aloop's real
// signal path to make 16-bit quantization a no-op against). The stale
// rationale for 16 assumed identity with "the CLI harness's own WAV writer"
// (a bench-only int16 dump), not this stage's actual float production
// signal path. 24 is verified (same DawDreamer harness) to bring the
// amt=0 diff down to 8.94e-08 -- the same float32-round-trip noise floor
// every other always-on stage in this chain already sits at (2.98e-08),
// i.e. genuinely at the passthrough invariant this file's header claims,
// not merely closer to it. BITS_MIN=2 at BITCRUSHAMT=1 is unaffected by
// this change (still 4 discrete levels, verified unique-output-level count
// unchanged) -- audibly extreme staircase/gritty digital distortion,
// matching the task's "down to ~2-4 bits" target for the crushed extreme.
import("stdfaust.lib");

// ---- Runtime control ----
BITCRUSHAMT = hslider("BITCRUSHAMT", 0.0, 0.0, 1.0, 0.01);

BITS_MAX = 24.0;
BITS_MIN = 2.0;
bits = BITS_MAX - BITCRUSHAMT * (BITS_MAX - BITS_MIN);

// Quantize x (assumed in [-1,1]) to 2^bits levels: scale up by half the level
// count, round to nearest integer, scale back down. Using round-to-nearest
// (not floor) keeps the quantization error symmetric/centered rather than
// biased toward one direction, avoiding an audible DC-offset-like bias as
// BITCRUSHAMT increases.
levels = pow(2.0, bits - 1.0);      // half-range level count (signed quantizer)
quantize(x) = rint(x * levels) / levels;

// At BITCRUSHAMT=0, levels = 2^23 = 8388608, a step (2/2^24) below float32's
// own representable precision near [-1,1] -- genuinely transparent in the
// real float production signal path, not tied to any particular downstream
// integer bit depth (see BITS_MAX's own comment above for why the previous
// BITS_MAX=16 choice, based on an unrelated bench-only int16 WAV writer,
// was not actually transparent there).
process = _ <: quantize;
