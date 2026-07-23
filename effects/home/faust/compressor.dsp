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
// dial". Automatic makeup gain (see below) is what actually makes turning the
// dial up produce a LOUDER result rather than just squashed dynamics.
//
// ---- Why hand-rolled instead of compressors.lib ----
// stdfaust's co.compressor_mono(ratio, thresh, att, rel) is a fine building
// block for the gain-reduction curve itself, but it does not compute makeup
// gain at all — that's always left to the caller. Since this task needs
// makeup gain that actually measures and reacts to the compressed signal
// (see Gen6 below), and needs the whole thing to collapse to an exact no-op
// multiply at threshold=infinity, it's simplest and most transparent to
// hand-roll the classic feedforward compressor shape directly: an RMS-style
// envelope follower in dB driving the gain-reduction curve, a SEPARATE fast
// peak follower driving the makeup stage, and a final hard safety ceiling.
// This keeps every stage of the signal path visible and lets the passthrough
// proof at COMPRESSAMT=0 be a literal, inspectable identity rather than
// something buried inside a library black box.
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

// Attack/release are not separately exposed as distinct time constants for
// the GAIN-REDUCTION envelope. A single RMS-style time constant (RMS_MS,
// below) drives that follower instead of a classic asymmetric attack/release
// pair — an attack/release one-pole applied directly to abs(x) was tried
// first and found unstable in verification: at audio-rate zero crossings,
// abs(x) repeatedly flips from rising to falling within a single cycle, so
// the "is it rising" branch toggles every few samples and the envelope never
// settles to the tone's true level, it partially chases the waveform shape
// itself. Smoothing the SQUARED signal (power) with one single one-pole,
// then sqrt (true-RMS-style), avoids that: squaring removes the abs() kink
// entirely, so one time constant tracks a steady tone's level as a genuinely
// steady value. (The MAKEUP stage below uses a second, separate, much faster
// follower — see RELEASE_MS — because it needs to track true PEAKS, not a
// smoothed RMS level; conflating the two was part of why earlier generations
// of this file couldn't hit both the loudness and no-clipping goals at once.)
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

// ---- Automatic makeup gain ----
// WITNESSED BUG (this session, via dsp_cli sweep), FIVE generations before
// this one — all left in place below as diagnosed history for whoever reads
// this file next:
// Gen 1 (fixed makeup gain sized off a worst-case, signal-pinned-at-0dBFS
// headroom gap, no final ceiling): correct only for a signal actually AT
// that worst case. A real sine sitting further below threshold gets a
// SMALLER actual reduction than the fixed makeup was sized to offset, so
// combined gain nets ABOVE 1.0 — sweeping COMPRESSAMT 0->0.3->0.6->1.0 showed
// RMS climbing then COLLAPSING at 1.0 (0.4349->0.2881) with peak clipping to
// 1.409 raw.
// Gen 2 (same worst-case-derived makeupDb, clamped to a fixed +1dB combined-
// gain ceiling): fixed the clipping, but the ceiling disproportionately
// clamped down the LOUD samples relative to quiet ones, so overall RMS still
// fell at the dial extreme (0.3967->0.2817) — the ceiling was masking the
// same mismatch, not fixing it.
// Gen 3 (makeup = exactly half of THIS sample's own grDb): eliminated
// clipping structurally (combined gain reduces to pow(10,-grDb(x)*0.5/20),
// always <=1.0 since grDb(x)>=0 always) but this ALSO eliminated any real
// loudness recovery — combined gain can only ever attenuate less
// aggressively than the raw ratio, never actually raise the signal back up,
// contradicting the "get the loudness blown out" spec entirely.
// Gen 4 (makeup = the FULL theoretical headroom gap (0dB - thresholdDb),
// scaled by the ratio's compensation fraction, min-clamped to a fixed
// combined-gain ceiling of exactly 1.0): looked right on paper, but
// inspecting the generated C++ (dsp_cli's own build output) showed that for
// a normal-level test tone (e.g. a 0.5-peak / -9dBFS-RMS 440Hz sine),
// grLinear(x)*makeupLinear computes to a value ABOVE 1.0 across the entire
// useful COMPRESSAMT range, so min(1.0, ...) clamps it back down to EXACTLY
// 1.0 for every sample at every dial position — meaning `x * combinedGain(x)`
// reduces to `x * 1.0` regardless of COMPRESSAMT. A dsp_cli sweep confirmed
// this directly: out.wav was BYTE-IDENTICAL (fc /b) at COMPRESSAMT=0.0 vs
// 1.0 — no compression, no makeup, nothing audible ever changed. The root
// cause is the same shape as every earlier generation: makeupDb was a number
// computed from where the THRESHOLD sits, not from where the signal's ACTUAL
// peak ends up after gain reduction is applied.
// Gen 5 (routing the linear-domain output through tanh(y/DRIVE)/tanh(1/DRIVE)
// as a soft-saturation stage, so combinedGain could be allowed to exceed 1.0
// before a smooth, non-hard-clipping final stage brought it back down): never
// got as far as a passing dsp_cli sweep — it failed to even COMPILE, because
// `tanh` is not a bare Faust primitive (it needs the `ma.` stdlib prefix,
// e.g. `ma.tanh`), so `build.bat` rejected it with "undefined symbol : tanh"
// before any audio was ever rendered.
//
// REAL FIX (Gen 6, this generation): stop guessing a makeup number from
// thresholdDb and instead MEASURE the post-gain-reduction signal's true peak
// with a fast peak-detector (attack effectively instant — a per-sample max
// envelope — release slow, RELEASE_MS below, so it holds the recent peak
// rather than chasing zero crossings the way reusing the 50ms RMS follower
// above would), then set makeup gain = TARGET_PEAK / trackedPeak — i.e.
// genuine peak-normalizing automatic gain control, the standard mechanism
// real analog bus compressors use to turn "squashed dynamic range" into
// "louder": once the gain-reduction stage has pulled the loud parts of the
// signal down toward/below threshold, the makeup stage measures how far the
// RESULT's own peak now sits below TARGET_PEAK and applies exactly enough
// gain to close that gap — never a fixed guess, always tied to what the
// signal actually did. Because grLinear(x) <= 1.0 always (pure attenuation,
// unchanged from every prior generation), the post-GR peak can only ever be
// <= the input's peak, so this makeup gain is always >= 1.0 — it only ever
// restores headroom that gain reduction removed, never invents extra.
//
// Exact passthrough at COMPRESSAMT=0 without reintroducing Gen 1/2/4's
// fixed-formula mismatch: rather than deriving makeup from thresholdDb (the
// bug's root cause), blend the FINAL peak-normalizing multiplier itself
// toward 1.0 by COMPRESSAMT directly — makeupLinear = 1 + COMPRESSAMT*(raw
// target ratio - 1). This is the exact same "at 0 the knob's own coefficient
// zeroes the wet contribution" idiom this bank already uses everywhere else
// (see tremolo.dsp's `gain = 1.0 - TREMOLOAMT*(1.0-lfoUni)`, flanger.dsp's
// `x + FLANGEAMT*(wet - x)`): the multiply-by-the-dial structurally
// guarantees makeupLinear==1.0 at COMPRESSAMT=0 regardless of what the peak
// tracker measures, so passthrough is a Faust-visible identity again, not an
// emergent coincidence of a formula happening to hit zero (Gen 4's failure
// mode was exactly this kind of coincidence-based reasoning, just inverted —
// it happened to clamp to 1.0 everywhere instead of only at the dial's zero).
RELEASE_MS   = 5.0; // fast enough to actually track a steady tone's real
                     // peak within a couple of cycles at guitar-range
                     // frequencies (unlike the 50ms RMS_MS follower above,
                     // which is deliberately slow/smooth for the gain-
                     // reduction envelope — this second, separate follower
                     // exists specifically to feed the makeup stage a true
                     // peak reading, not a smoothed RMS-ish one).
releaseCoeff = exp(-1.0 / (RELEASE_MS * 0.001 * SR));

// True (attack-instant / release-smoothed) peak follower: on each sample the
// tracked value jumps UP immediately to a new, higher |post-GR sample|, and
// decays back down at releaseCoeff otherwise — this is what makes it a peak
// detector rather than an RMS-style follower (RMS would still be tracking
// average power here, which is exactly the mismatch every prior generation
// suffered from when trying to predict a PEAK from a level formula derived
// from thresholdDb instead of measuring one).
peakFollow(x) = loop ~ _
with {
    loop(prev) = max(abs(x), prev * releaseCoeff);
};

// IMPORTANT: two intermediate correction attempts made during verification
// of this same generation, both left visible here since each taught
// something the final design (below) depends on:
//
// Correction attempt A: normalized trackedPeak against a FIXED absolute
// target (TARGET_PEAK = 0.98 regardless of the input signal), reasoning
// "bring the compressed peak back up near 0dBFS". Broke monotonicity's
// PRECONDITION -- a dsp_cli sweep showed RMS jumping to its final plateau by
// COMPRESSAMT=0.1 and staying completely flat all the way to 1.0, because on
// a signal whose own peak is well below 0.98 (e.g. this task's own
// 0.5-amplitude test tone), the makeup stage wanted to boost gain to reach
// 0.98 even at dial settings where grLinear was STILL EXACTLY 1.0 (threshold
// hadn't been reached yet, no compression had happened at all) -- blending
// that huge premature ratio by COMPRESSAMT alone (see makeupLinear below)
// still hit the HEADROOM_LIN safety ceiling almost immediately, and a hard
// ceiling, once hit, produces the same clamped output regardless of how much
// MORE compression the dial dials in afterward.
// Correction attempt B: normalized against the INPUT's OWN tracked peak
// instead of a fixed constant (ratio = inputPeak/trackedPeak), reasoning
// "only restore headroom that gain reduction actually removed". This fixed
// the premature-ceiling problem but overcorrected: it caps makeup at
// exactly undoing the PEAK loss, which is systematically less than what's
// needed to undo the RMS loss (compression reduces the whole envelope, not
// just its peak) -- a dsp_cli sweep showed RMS actually DIPPING at
// mid-dial (0.3535->0.3286 around COMPRESSAMT=0.8) before only partially
// recovering at 1.0, i.e. non-monotonic, and never once exceeding the
// COMPRESSAMT=0 baseline -- restoring the input's own peak can never make a
// signal LOUDER than its original self, only ever at-best-equal, which
// contradicts the "get the loudness blown out" spec just as much as Gen 3's
// half-grDb makeup did.
//
// REAL FIX for this generation: target an ABSOLUTE ceiling (TARGET_PEAK,
// near but under full scale) same as attempt A, but GATE how hard the
// makeup stage chases that ceiling by HOW MUCH GAIN REDUCTION HAS ACTUALLY
// HAPPENED (compressionAmount, derived straight from grLinear, not from
// COMPRESSAMT directly) instead of gating by the dial. This is the missing
// piece both earlier attempts lacked: attempt A gated by COMPRESSAMT (wrong
// signal -- COMPRESSAMT can be high while grLinear is still exactly 1.0,
// i.e. threshold not yet reached by this particular envelope value);
// attempt B gated correctly (via grLinear) but aimed at the wrong target
// (input's own peak, which can only ever restore, never exceed). Aiming at
// TARGET_PEAK while gating on compressionAmount gets both right at once: no
// gain reduction yet => compressionAmount=0 => makeup=1.0 (no premature
// ceiling-chasing); once the envelope crosses threshold and grLinear
// actually drops, compressionAmount rises and the makeup stage chases
// TARGET_PEAK proportionally -- so by the time COMPRESSAMT nears 1 and
// grLinear is heavily reducing the loud parts, makeup pushes hard toward
// TARGET_PEAK, raising RMS well past the original baseline (the genuine
// "blown out" loudness increase) while the HEADROOM_LIN ceiling (below)
// remains a rare last-resort clamp rather than the everyday operating point.
PEAK_FLOOR = 0.01;  // silence guard: never divide by a near-zero tracked
                     // peak (which would blow makeup gain up to a huge
                     // number on silence/near-silence).
TARGET_PEAK = 0.98; // just under unity: the absolute ceiling the makeup
                     // stage chases once compression has actually engaged,
                     // leaving a hair of margin under HEADROOM_LIN for the
                     // peak follower's release lag.

postGR(x)      = x * grLinear(x); // gain-reduced signal, still <=|x| always
trackedPeak(x) = max(PEAK_FLOOR, peakFollow(postGR(x))); // peak AFTER gain reduction

// How much actual gain reduction is happening right now, as a 0..1 fraction
// (0 = grLinear still exactly 1.0, no compression yet; ->1 as grLinear->0,
// heavy compression). This is the correct gate signal for the makeup stage
// -- unlike COMPRESSAMT itself, it is zero exactly when there is nothing yet
// to make up for, regardless of where the dial sits.
compressionAmount(x) = 1.0 - grLinear(x);

// Raw peak-normalizing target ratio: how much gain would bring the
// gain-reduced signal's own tracked peak up to the absolute TARGET_PEAK
// ceiling. Always >= 1.0 since trackedPeak <= 1.0 <= TARGET_PEAK's
// neighborhood -- max()-clamp defensively anyway so a pathological input can
// never make this dip below 1.0 and start ADDING extra attenuation on top of
// grLinear (that's not this stage's job).
rawMakeupRatio(x) = max(1.0, TARGET_PEAK / trackedPeak(x));

// Gated makeup: blended toward the full peak-normalizing target by
// compressionAmount (NOT by COMPRESSAMT directly) -- exactly 1.0 whenever no
// real gain reduction has happened yet (compressionAmount=0), regardless of
// dial position, which is what stops this generation from prematurely
// hitting HEADROOM_LIN the way attempt A did; and unlike attempt B, the
// target itself (TARGET_PEAK) is an absolute ceiling near full scale, not
// the input's own (possibly much lower) peak, so makeup can genuinely raise
// RMS above the COMPRESSAMT=0 baseline as more of the dial's range
// compresses harder.
makeupLinear(x) = 1.0 + compressionAmount(x) * (rawMakeupRatio(x) - 1.0);

// ---- Final hard safety ceiling ----
// Standard on every real hardware bus compressor's output stage: normal
// operation targets TARGET_PEAK (0.98), leaving HEADROOM_LIN as a rare
// last-resort clamp rather than the everyday operating point (unlike Gen 4's
// ceiling, which was hit on EVERY sample and flattened the entire waveform
// to a no-op) -- it exists purely to catch whatever a peak-detector-with-lag
// can never fully rule out (e.g. a sudden new transient arriving the very
// sample after the tracker last measured a much quieter peak).
HEADROOM_LIN = 1.05;
combinedGain(x) = min(HEADROOM_LIN, grLinear(x) * makeupLinear(x));

// At COMPRESSAMT=0: thresholdDb=THRESH_MAX_DB=24 (>= any |x|<=1 sample's
// envelope, which is <=0dBFS), so overDb(x)=0 for every sample => grDb=0 =>
// grLinear=1.0 EXACTLY (pow(10,0)=1). makeupLinear(x) = 1.0 + 0*(...) = 1.0
// EXACTLY regardless of rawMakeupRatio's value (the peak follower and
// log/pow machinery still run every sample "for free" in the background,
// matching this bank's established flanger.dsp/vinyl.dsp/flutter.dsp
// convention of computing the wet path unconditionally and relying on an
// exact-identity multiply at the default, rather than a Faust-level bypass
// branch). combinedGain = min(1.05, 1.0*1.0) = 1.0 exactly, so
// `x*combinedGain` is bit-for-bit `x` — the required byte-exact passthrough.
//
// Verified via dsp_cli (steady 440Hz sine, --gen sine:440:1.0, full-file
// stats). See this generation's commit message / task report for the exact
// sweep numbers this build produced (RMS rising monotonically with the
// dial, peak never exceeding HEADROOM_LIN's 1.05 bound, COMPRESSAMT=0 an
// exact byte-for-byte passthrough).
compress(x) = x * combinedGain(x);

process = _ <: compress;
