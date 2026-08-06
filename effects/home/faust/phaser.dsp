// Phaser — cascade of allpass filter stages, all modulated together by one
// shared LFO, producing the classic sweeping-notch "whoosh". New guitar-bank
// stage (see guitar-fx-dsp-phaser in .gm/prd.yml): top-row knob 4,
// "phaser amnt".
//
// PHASERAMT (0..1, default 0) is the wet mix: at 0 the output is the dry
// signal with zero contribution from the allpass cascade, giving a byte-exact
// passthrough (the allpass chain still computes every sample, but its result
// is scaled by 0 and dropped, exactly like flanger.dsp/tremolo.dsp's
// zero-at-default crossfades).
//
// Reads the SAME `BANKSPEED` hslider tremolo.dsp declares (identical label
// string "BANKSPEED", identical range/default/step 0..1/0.5/0.01) so Faust's
// UI elaboration unifies both declarations into one physical zone — turning
// the shared bank-speed knob moves this phaser's LFO and tremolo.dsp's LFO
// together, per the task spec. (See tremolo.dsp's header comment for the
// existing-codebase precedent: chain.dsp already shares one upstream `TIME`
// value across delay.dsp's and reverb.dsp's own `TIME` params, the same
// "one control, many consumers" idea applied here via label matching instead
// of component() parameter binding.)
//
// 6 allpass stages: the classic phaser range is 4-8 stages (4 = subtle, 8 =
// dense multi-notch "jet"); 6 is picked as the middle ground so the effect
// has clearly audible multiple notches without being as extreme as an 8-stage
// unit — matches typical analog phaser pedal designs (e.g. Small Stone/
// Phase 90-style units commonly use 4-6 stages).
import("stdfaust.lib");

// ---- Runtime controls ----
PHASERAMT = hslider("PHASERAMT", 0.0, 0.0, 1.0, 0.01);
BANKSPEED = hslider("BANKSPEED", 0.5, 0.0, 1.0, 0.01);

// Same rate mapping as tremolo.dsp (0.1Hz..10Hz, exponential) so the two
// effects' LFOs are not merely reading the same knob but landing on the same
// musical rate scale — a shared knob would feel inconsistent if the mapped
// Hz range differed between the two consumers.
RATE_MIN_HZ = 0.1;
DECADES     = 2.0;
lfoRateHz   = RATE_MIN_HZ * pow(10.0, DECADES * BANKSPEED);

// Unipolar LFO drives the allpass corner frequency sweep.
lfoUni = (os.osc(lfoRateHz) + 1.0) * 0.5;

// Sweep the allpass center frequency between ~200Hz and ~2000Hz — the
// classic phaser sweep band (below ~200Hz the notches are mostly inaudible
// on typical guitar/loop material; above ~2000Hz the moving notches start
// sounding thin/harsh rather than the smooth "swoosh").
SWEEP_MIN_HZ = 200.0;
SWEEP_MAX_HZ = 2000.0;
sweepHz = SWEEP_MIN_HZ + lfoUni * (SWEEP_MAX_HZ - SWEEP_MIN_HZ);

// One first-order allpass stage tuned to sweepHz, all 6 stages sharing the
// SAME modulated frequency (a single shared LFO drives every stage together,
// per the task spec's "cascade ... all modulated together by one shared
// LFO"). filters.lib has no ready-made frequency-controlled first-order
// allpass (its allpassN* family is comb/ladder-style for reverb use), so the
// textbook first-order allpass is written out directly:
//   a = (tan(pi*fc/SR) - 1) / (tan(pi*fc/SR) + 1)
//   y[n] = a*x[n] + x[n-1] - a*y[n-1]
// State (previous x and previous y) is carried via Faust's `~` feedback
// operator; `fc` (sweepHz) is recomputed every sample from the shared LFO, so
// the coefficient itself is time-varying (matching a genuinely swept phaser,
// not a fixed-frequency filter).
SR = 48000.0;
PI = 3.14159265;
// tan(PI*fc/SR) was computed TWICE per sample (once in the numerator, once
// in the denominator) -- a real, free win: compute it once via a `with`
// binding. tan() is a genuinely expensive transcendental call (unlike the
// PI*fc/SR argument itself, which Faust already constant-folds where
// possible since PI/SR are compile-time constants) -- halving the tan()
// call count here is a correctness-preserving optimization, not a
// behavioral change (same math, same result, computed once instead of
// twice).
apCoeff(fc) = (t - 1.0) / (t + 1.0)
with { t = tan(PI * fc / SR); };

// blk(xprev_state, yprev_state) takes the fed-back previous-x and
// previous-y, plus the current input x, and emits (x, y) as the new state —
// `~ si.bus(2)` loops both back with one unit delay, and the trailing
// (!,_) drops the recirculated x, keeping only the allpass output y.
allpass1(fc, x) = (blk ~ si.bus(2)) : (!,_)
with {
    a = apCoeff(fc);
    blk(xprev, yprev) = x, y
    with {
        y = a*x + xprev - a*yprev;
    };
};

apStage(x) = allpass1(sweepHz, x);
apCascade(x) = x : apStage : apStage : apStage : apStage : apStage : apStage;

// Wet crossfade: at PHASERAMT=0, `x + 0*(wet-x) == x` exactly, so the cascade
// (whose feedback state still evolves sample-by-sample "for free" in the
// background) contributes nothing and the output is bit-exact passthrough.
phaserMix(x) = x + PHASERAMT * (apCascade(x) - x);

process = _ <: phaserMix;
