// Sample-rate reduction (sample-and-hold decimation) — new lofi-fx-bank stage
// (see .gm/prd.yml lofi-fx-samplerate). Deliberately NOT bit-depth reduction
// (that's a separate stage, bitcrush.dsp, being built by another agent) — this
// is specifically sample-and-hold: the output holds the last sampled value
// constant for N input samples before updating, which is what a real cheap
// digital-lofi sample-rate reducer does. That "staircasing" is what produces
// the classic aliased/crunchy lofi character, distinct from bit-crushing's
// quantization-noise character.
//
// Implemented via a free-running sample counter and a hold register: every N
// samples the register is refreshed from the live input, otherwise it repeats
// its last-held value. N (the hold length, in samples) scales with SRRAMT.
//
// SRRAMT is the single runtime control (0..1, default 0):
//   0 -> N=1 (every sample updates -> normal, no reduction, byte-identical
//        passthrough)
//   1 -> N=~32 (heavy reduction -> holding for ~32 input samples between
//        updates, deep in "aliased/crunchy" territory)
// N is mapped exponentially rather than linearly across SRRAMT so the low end
// of the knob still gives a subtle, usable amount of grit (a linear map would
// spend most of the useful knob travel between N=1 and N=2-3, then rocket
// straight to extreme territory) — exponential curves give a much more even
// perceptual spread of "a little crunchy" through "a lot crunchy" across the
// slider's full range, matching how ear-perceived aliasing severity scales.
//
// Hard requirement: at SRRAMT=0 output must be BYTE-IDENTICAL to input (see
// param_mapping.md's documented passthrough invariant, maxAbs=0). N is an
// integer (int() floors the exponential map), so at SRRAMT=0, N=1 EXACTLY
// (not just approximately close to 1) — with N=1 the "every N samples"
// condition is true every sample, so the hold register refreshes from live
// input on every single sample and the held value IS the input, unmodified.

import("stdfaust.lib");

// ---- Runtime control ----
SRRAMT = hslider("SRRAMT", 0.0, 0.0, 1.0, 0.01);

// ---- Hold-length mapping ----
// N_MAX chosen at the upper end of the task's suggested "up to ~20-40x"
// decimation range — deep enough to be unmistakably a lofi sample-rate crush
// at SRRAMT=1, without going so extreme the signal turns into pure noise.
N_MAX = 32.0;

// Exponential curve: N = 1 * N_MAX^SRRAMT, i.e. N=1 at amt=0 (pow(N_MAX,0)=1
// exactly) and N=N_MAX at amt=1 (pow(N_MAX,1)=N_MAX exactly), floored to an
// integer hold length since the sample counter below counts whole samples.
holdN = int(pow(N_MAX, SRRAMT));

// Free-running sample counter, wrapping at holdN: counts 0,1,...,holdN-1,
// 0,1,... . `refresh` (counter==0) marks the samples where the hold register
// takes a fresh reading from live input; every other sample repeats the held
// value. At holdN=1, `cnt mod 1` is always 0, so refresh is true EVERY sample.
cnt = counter ~ _
with {
    counter(prev) = (prev + 1) % max(1, holdN);
};
refresh = (cnt == 0);

// Sample-and-hold register: on a refresh sample the held value becomes the
// live input; otherwise it keeps its previous value. `held` is expressed as a
// one-sample feedback loop so the "previous value" is genuinely the register's
// prior state, not a recomputation.
srHold(x) = held
with {
    held = (loop ~ _)
    with {
        loop(prev) = ba.if(refresh, x, prev);
    };
};

// At SRRAMT=0, holdN=1 so refresh is true on every sample, and srHold's
// `loop` always takes the `x` branch — meaning the output is exactly the live
// input every sample, with no held-over-stale-value case ever reached. This
// gives true byte-exact passthrough at the default, not just an approximation
// via crossfade multiply (unlike vinyl.dsp/flutter.dsp, no crossfade is needed
// here because the hold logic itself degenerates to identity at N=1).
process = _ <: srHold;
