// Bus compressor — single-knob "blown out loudness" compressor. New
// guitar-fx-bank stage: one dial, inverse-threshold mapping, automatic makeup
// gain, everything else (attack/release/ratio) a fixed internal sweetspot.
//
// COMPRESSAMT (0..1, default 0) is the ONLY exposed control, per the task's
// single-knob spec. It does NOT scale a wet/dry crossfade like the other new
// effect files in this directory (flanger.dsp/tremolo.dsp/vinyl.dsp/etc) —
// there is no "dry compressor signal" to crossfade against; instead the dial
// directly sets the compressor's threshold, INVERTED: dial=0 -> threshold is
// pinned above any signal this chain can produce (no gain reduction ever
// triggers, so gain=1.0 for every sample => byte-exact passthrough), dial=1
// -> threshold all the way down at THRESH_MIN_DB, i.e. maximum compression.
// This matches the user's own framing: "the higher you dial it the lower the
// threshold compressing the audio to get the loudness blown out with a single
// dial". Automatic makeup gain (see below) is derived FROM the threshold, so
// turning the dial up doesn't just squash dynamics — it makes the result
// louder, which is the whole point of this stage.
//
// ---- Why hand-rolled instead of compressors.lib ----
// stdfaust's co.compressor_mono(ratio, thresh, att, rel) is a fine building
// block for the gain-reduction curve itself, but it does not compute makeup
// gain at all — that's always left to the caller. Since this task couples
// makeup gain arithmetically to the threshold (not just "add a second
// exposed makeup knob"), and needs the whole thing to collapse to an exact
// no-op multiply at threshold=infinity, it's simplest and most transparent to
// hand-roll the classic feedforward compressor shape directly: an RMS-style
// envelope follower in dB, a soft-ratio gain-reduction curve above threshold,
// converted back to a linear multiplier, times a makeup-gain multiplier
// derived from the same threshold value, with a final hard safety ceiling.
// This keeps every stage of the signal path visible and lets the passthrough
// proof at COMPRESSAMT=0 be a literal, inspectable identity rather than
// something buried inside a library black box.
//
// ---- Verification history ----
// Verification used TWO tools: dsp_cli (--list-zones, --gen + --stats over
// the WAV round-trip) AND a standalone raw-float probe that instantiates the
// generated Faust C++ class directly and inspects samples before they pass
// through dsp_cli's WAV writer. The second tool mattered: that writer clamps
// every sample to [-1,1] before quantizing to int16, so any intermediate
// value outside that range (e.g. a raw dB reading, or a gain ratio > 1)
// silently saturates in the WAV file and its --stats output — several
// attempts below LOOKED broken purely from that clamp artifact before the
// raw-float probe confirmed the underlying Faust math was actually fine (or,
// in other cases, genuinely was broken, which the probe also confirmed).
// Attempt 1: fixed makeup gain sized off a worst-case (signal pinned at
// 0dBFS) headroom gap, no final ceiling. RMS climbed with COMPRESSAMT then
// COLLAPSED near the top of the dial (0.4349->0.2881) while peak clipped to
// 1.409 raw — the fixed makeup overshot on a real (not worst-case) signal.
// Attempt 2: makeup tied to THIS sample's own grDb, scaled by 0.5, no
// ceiling needed since combinedGain = pow(10,-grDb(x)*0.5/20) is
// mathematically bounded <=1.0 for every sample. This eliminated clipping
// completely but ALSO eliminated any loudness gain at all — combined gain
// can only ever attenuate less aggressively than the raw ratio, never
// actually exceed unity, so RMS never rises above the COMPRESSAMT=0 baseline.
// Attempt 3: fixed (dial-dependent, not per-sample) makeup gain recovering
// the full threshold-to-0dB gap, combined-gain clamped with a hard
// min(1.0, ...) ceiling. This defeated the feature a different way: for
// every sample where the raw grLinear(x)*makeupLinear already exceeded 1.0
// (the intended "louder" case), clamping it back down to EXACTLY 1.0 just
// reproduces that sample's original, UNCOMPRESSED value — so on a two-level
// (loud/quiet burst) test signal, RMS stayed completely flat (0.4404) across
// the entire COMPRESSAMT sweep. A ceiling pinned at exactly unity gain can
// never make anything louder, only ever leave loud parts unchanged.
// Attempt 4: same fixed makeup gain, but the final stage replaced the hard
// ceiling with a GLOBAL tanh(y/DRIVE)/tanh(1/DRIVE) soft-clip blended in via
// a COMPRESSAMT-scaled crossfade (this bank's usual "x + AMT*(wet-x)" idiom).
// This fixed the loudness problem but broke byte-exact passthrough: the
// crossfade linearly blends an UNBOUNDED raw-gain value against a bounded
// saturated one, so at any COMPRESSAMT strictly between 0 and 1 some of that
// unbounded excess still leaks through un-saturated — peak still reached
// ~2.0 raw on the loud/quiet-burst signal at COMPRESSAMT=0.85.
// Shipped design (this generation): same fixed, threshold-derived makeup
// gain as Attempt 3/4, but the final stage only saturates the part of a
// sample that's actually ABOVE the +-1 rail (`excess = v - clampUnit(v)`,
// exactly 0 whenever |v|<=1) and passes anything already in-range through
// completely untouched — see softLimit below. This keeps COMPRESSAMT=0 an
// exact identity (no crossfade needed at all: the excess is structurally 0
// whenever the input is already in-range, which it always is at the
// default) while still letting genuinely out-of-range (compressed +
// made-up) samples round off smoothly instead of either (a) hard-clipping
// back to a no-op ceiling or (b) leaking unbounded excess through a linear
// blend. Verified via the standalone raw-float probe:
//   Steady 440Hz, 0.5-amplitude sine (stats over samples 10000..48000, past
//   the envelope follower's startup transient):
//     COMPRESSAMT=0.0  peak=0.5000  rms=0.3536  (byte-exact passthrough)
//     COMPRESSAMT=0.5  peak=0.5000  rms=0.3536  (still below threshold)
//     COMPRESSAMT=0.7  peak=1.0322  rms=0.7587  (compressing + genuinely louder)
//     COMPRESSAMT=1.0  peak=1.0322  rms=0.7587  (max dial, bounded peak)
//   Two-level loud(0.9)/quiet(0.1) 440Hz burst signal (real dynamic range):
//     COMPRESSAMT=0.0  peak=0.9000  rms=0.4404  (byte-exact passthrough)
//     COMPRESSAMT=0.7  peak=1.0478  rms=0.5877  (compressing + louder)
//     COMPRESSAMT=1.0  peak=1.0493  rms=0.6115  (peak bounded ~1.05, RMS up
//                                                ~39% over the default)
// RMS is monotonic non-decreasing with COMPRESSAMT on both test signals, and
// peak stays close to the +-1 rail (never far past ~1.05) despite makeup
// gain being sized aggressively enough to genuinely raise loudness — a
// standalone byte-exact-passthrough probe (0 mismatches across 48000 samples
// at COMPRESSAMT=0, comparing raw floats, not the WAV-quantized round-trip)
// confirms the passthrough guarantee holds at the bit level, not just in
// peak/RMS summary stats.
//
// (A separate approach was also tried using a fast peak-follower-driven
// automatic-gain-control makeup stage, blended toward a peak-normalizing
// target ratio by COMPRESSAMT, with a HEADROOM_LIN=1.05 ceiling. It passed
// the passthrough check but was too conservative to satisfy the "blown out"
// spec: RMS barely moved [+5% at most] and stopped increasing well before
// COMPRESSAMT reached 1.0 on both test signals, so it was not the design
// shipped here.)
import("stdfaust.lib");

SR = 48000.0;

// ---- Runtime control ----
// Single user-facing dial. Named COMPRESSAMT to match this directory's
// established <EFFECT>AMT convention (see FLANGEAMT/TREMOLOAMT/VINYLAMT/
// FLUTTERAMT/SRRAMT/BITCRUSHAMT — every new-effect file in this bank exposes
// exactly one bare hslider named "<NAME>AMT", 0..1 range, 0.0 default,
// 0.01 step, no group()/nested label).
COMPRESSAMT = hslider("COMPRESSAMT", 0.0, 0.0, 1.0, 0.01);

// ---- Fixed internal sweetspot constants (not exposed) ----
// Ratio: 4:1 is the classic "bus compressor glue" ratio — enough to visibly
// tame peaks and (combined with makeup gain) read as pumped/loud, without
// tipping into the near-brickwall slam of 8:1+ that would sound like a
// limiter rather than a compressor. A single fixed ratio keeps this a true
// one-knob control per the task spec.
RATIO = 4.0;

// Attack/release are not separately exposed as distinct time constants. A
// single RMS-style time constant (RMS_MS, below) drives the envelope
// follower instead of a classic asymmetric attack/release pair — an
// attack/release one-pole applied directly to abs(x) was tried first and
// found unstable in verification: at audio-rate zero crossings, abs(x)
// repeatedly flips from rising to falling within a single cycle, so the
// "is it rising" branch toggles every few samples and the envelope never
// settles to the tone's true level, it partially chases the waveform shape
// itself. Smoothing the SQUARED signal (power) with one single one-pole,
// then sqrt (true-RMS-style), avoids that: squaring removes the abs() kink
// entirely, so one time constant tracks a steady tone's level as a genuinely
// steady value.
RMS_MS = 50.0;
rmsCoeff = exp(-1.0 / (RMS_MS * 0.001 * SR));

FLOOR_LIN = 0.000001; // -120dB floor, silence guard for the log

envFollow(x) = sqrt(max(FLOOR_LIN2, powLP))
with {
    powLP = (x*x) : si.smooth(rmsCoeff);
    FLOOR_LIN2 = FLOOR_LIN * FLOOR_LIN;
};

envDb(x) = 20.0 * log10(max(FLOOR_LIN, envFollow(x)));

// Threshold range: at COMPRESSAMT=0, threshold is pinned to THRESH_MAX_DB,
// chosen far above any signal level this chain can ever present (signals
// here are normalized floats in [-1,1], i.e. <=0dBFS) so gain reduction never
// engages — this is what makes gain=1.0 an exact identity, not merely a very
// high threshold that happens to rarely trigger. At COMPRESSAMT=1, threshold
// bottoms out at THRESH_MIN_DB, deep enough below typical program level that
// most of the signal sits above threshold and gets compressed hard, giving
// the "blown out" extreme the task asks for.
THRESH_MAX_DB = 24.0;   // effectively "infinite" for signals capped at 0dBFS
THRESH_MIN_DB = -24.0;  // aggressive, most-of-the-signal-compressed extreme

// Inverse mapping: dial UP => threshold DOWN => MORE compression, exactly as
// specified. Linear in dB (i.e. exponential in linear gain) because dB is
// already the ear's perceptual loudness scale — a linear-in-dB threshold
// sweep spends the knob's travel evenly across perceived "how much louder"
// rather than crowding all the audible change into one end.
thresholdDb = THRESH_MAX_DB - COMPRESSAMT * (THRESH_MAX_DB - THRESH_MIN_DB);

// ---- Gain reduction (dB domain), feedforward, standard hard-knee shape ----
// Below threshold: 0dB reduction. Above threshold: reduction grows at
// (1 - 1/RATIO) dB per dB the envelope sits above threshold — the textbook
// compressor transfer function overDb -> overDb/RATIO, expressed here as the
// GR amount subtracted (overDb - overDb/RATIO = overDb*(1-1/RATIO)).
overDb(x)   = max(0.0, envDb(x) - thresholdDb);
grDb(x)     = overDb(x) * (1.0 - 1.0 / RATIO);
grLinear(x) = pow(10.0, -grDb(x) / 20.0); // <= 1.0 always (pure attenuation)

// ---- Automatic makeup gain, tied to the threshold (i.e. to COMPRESSAMT) ----
// This is a FIXED (per-dial-setting, not per-sample) makeup multiplier,
// deliberately sized aggressively: makeupDb recovers the FULL dB gap between
// 0dBFS and the current threshold (not half, not a per-sample fraction — see
// the file-header verification history for why the gentler variants tried
// first failed to produce any real loudness gain). Tying it to the SAME
// thresholdDb the dial sets means turning the dial up simultaneously lowers
// the threshold (more of the signal gets compressed) AND raises makeup gain
// (the compressed result is pushed harder back up) — both effects compound
// in the same direction, which is exactly the "the higher you dial it... the
// loudness blown out" behavior specified.
makeupDb     = max(0.0, 0.0 - thresholdDb) * (1.0 - 1.0 / RATIO);
makeupLinear = pow(10.0, makeupDb / 20.0);

// A hard linear ceiling on combinedGain (min(1.0, ...)) was tried first and
// found to defeat the entire feature: for any sample where the compressor's
// raw grLinear(x)*makeupLinear already exceeds 1.0 (the intended "louder"
// case), clamping it back down to EXACTLY 1.0 just reproduces that sample's
// original, uncompressed value — i.e. loud passages become bit-identical to
// their input and only quiet passages (never reaching the ceiling) end up
// attenuated, so nothing gets louder anywhere. A GLOBAL tanh soft-clip
// (applied to every sample, scaled by DRIVE) was tried second and fixed the
// loudness problem but broke the byte-exact passthrough requirement at
// COMPRESSAMT=0 in two ways: (a) tanh(x) is not itself an identity function
// even for small x, so warping every sample through it is never bit-exact,
// and (b) even after wrapping that in a COMPRESSAMT-scaled crossfade (this
// bank's usual "x + AMT*(wet-x)" passthrough pattern), the crossfade is a
// LINEAR blend of an UNBOUNDED raw-gain value against a bounded saturated
// one — at any COMPRESSAMT strictly between 0 and 1, part of that unbounded
// excess still leaks through un-saturated, so output peak still exceeded 1.0
// (up to ~2.0 raw, verified via the standalone probe on a loud/quiet-burst
// signal at COMPRESSAMT=0.85).
//
// REAL FIX: only saturate the part of the signal that's actually ABOVE the
// +-1 rail, and leave anything already inside [-1,1] completely untouched.
// `excess(v) = v - clampUnit(v)` is exactly 0 whenever |v|<=1 (clampUnit is
// then the identity) and is the signed overshoot whenever |v|>1. Passing
// ONLY that overshoot through a saturating shape (here, a simple bounded
// `overshoot / (1+|overshoot|)` soft-knee -- monotonic, maps any overshoot
// smoothly into (-1,1) with slope 1 at zero-overshoot so the transition into
// saturation has no audible corner) and adding it back to the clamped value
// means: whenever the raw gained sample is already in range, the output is
// EXACTLY that sample (clampUnit(v)=v, excess=0, saturatedExcess=0) -- so at
// COMPRESSAMT=0, where combinedGain(x)=1.0 exactly and every |x|<=1 already,
// this whole stage is a literal no-op, satisfying the byte-exact passthrough
// requirement WITHOUT needing any separate crossfade at all. Whenever the
// raw gained sample genuinely exceeds unity (the makeup-boosted, heavily
// compressed case this feature exists for), the overshoot gets rounded off
// smoothly instead of hard-clipped, and the final result is mathematically
// guaranteed to stay within (-2, 2) (clampUnit in [-1,1] plus a saturated
// excess strictly inside (-1,1)) -- comfortably bounded, no runaway, and in
// practice sits much closer to unity since the excess term itself compresses
// large overshoots down toward (but never reaching) +-1.
combinedGain(x) = grLinear(x) * makeupLinear;

// EXCESS_DRIVE steepens the excess-saturation curve: e/(1+|e|) alone
// approaches +-1 too slowly for large e (an excess of 0.8 only saturates to
// ~0.44, letting the final output reach ~1.44 — verified via the standalone
// probe, peak up to 1.79 raw on a loud/quiet burst signal at COMPRESSAMT=1).
// Scaling e up by EXCESS_DRIVE before the division, then the whole
// saturated result back down by the same factor, makes e/(1+|e|)-shaped
// saturation kick in much sooner (a smaller excess already gets pulled
// close to the +-(1/EXCESS_DRIVE) sub-ceiling), while preserving the
// identity-at-zero-excess property (e=0 -> 0 either way) that keeps
// COMPRESSAMT=0 an exact passthrough.
EXCESS_DRIVE = 20.0;
clampUnit(v) = max(-1.0, min(1.0, v));
saturateExcess(e) = (EXCESS_DRIVE * e / (1.0 + abs(EXCESS_DRIVE * e))) / EXCESS_DRIVE;
softLimit(v) = clampUnit(v) + saturateExcess(v - clampUnit(v));

// At COMPRESSAMT=0: thresholdDb=THRESH_MAX_DB=24 (>= any |x|<=1 sample's
// envelope, which is <=0dBFS), so overDb(x)=0 for every sample => grDb=0 =>
// grLinear=1.0 EXACTLY (pow(10,0)=1), and makeupDb=0 => makeupLinear=1.0
// EXACTLY. combinedGain = 1.0*1.0 = 1.0 exactly, so `linear` is bit-for-bit
// `x`; since the DSP's own valid input range is |x|<=1, clampUnit(x)=x
// exactly and the excess term is exactly 0, so softLimit(x)=x exactly too —
// the required byte-exact passthrough, with no separate bypass branch or
// crossfade needed (the whole stage degenerates to true identity by
// construction whenever nothing is actually out of range).
compress(x) = softLimit(x * combinedGain(x));

process = _ <: compress;
