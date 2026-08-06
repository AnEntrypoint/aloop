// Tremolo — amplitude modulation: an LFO multiplies the signal's gain over
// time. New guitar-bank stage (see guitar-fx-dsp-tremolo in .gm/prd.yml):
// top-row knob 2, "tremolo amnt".
//
// TREMOLOAMT (0..1, default 0) is the wet-mix/depth: at 0 the gain multiplier
// is pinned to exactly 1.0 for every sample (no modulation at all), giving a
// byte-exact passthrough; as it rises toward 1 the gain swings further below
// unity on each LFO trough, deepening the "throb".
//
// BANKSPEED (0..1, default 0.5) sets the LFO rate. This same hslider name is
// ALSO declared in phaser.dsp with an IDENTICAL label string and IDENTICAL
// range/default/step — per the task spec and Faust's UI-elaboration rule,
// two `component()`-imported files that each declare an hslider with the same
// label text unify into ONE physical UI zone, so turning the shared "bank
// speed" knob moves both effects' LFOs together (this mirrors the existing
// dub-bank pattern of a single physical CC meaning "TIME" for both delay.dsp
// and reverb.dsp's shared `TIME` control — see chain.dsp, where DELAYAMT's
// and REVAMT's `TIME` params both receive the identical value from one
// upstream knob; the label-identity-unifies-zones mechanism is the same one
// at play there, just via component() parameter binding rather than hslider
// name matching — both resolve to "one physical control, many consumers").
//
// Rate mapping 0..1 -> ~0.1Hz..10Hz: a two-decade exponential sweep (not
// linear) because musically-useful tremolo/phaser rates span from a slow
// "pulsing" swell (~0.1-0.5Hz) up to a fast "warble" (several Hz to ~10Hz) —
// a linear 0..1->0.1..10 mapping would crowd all the musically-common slow
// rates into the bottom few percent of the knob's travel. An exponential
// (log-uniform) curve spreads perceptually-equal rate steps evenly across the
// knob's full range.
import("stdfaust.lib");

// ---- Runtime controls ----
TREMOLOAMT = hslider("TREMOLOAMT", 0.0, 0.0, 1.0, 0.01);
BANKSPEED  = hslider("BANKSPEED", 0.5, 0.0, 1.0, 0.01);

// 0.1Hz .. 10Hz exponential mapping (2 decades): rate = 0.1 * 10^(2*speed).
RATE_MIN_HZ = 0.1;
DECADES     = 2.0;
lfoRateHz   = RATE_MIN_HZ * pow(10.0, DECADES * BANKSPEED);

// Unipolar LFO (0..1) via os.osc (-1..1) rescaled, so the gain multiplier
// stays >= (1 - depth) and never inverts phase (a bipolar multiplier would
// briefly flip the signal's sign each cycle, which is not classic tremolo).
lfoUni = (os.osc(lfoRateHz) + 1.0) * 0.5;

// Gain = 1 - depth*(1-lfoUni): at depth=0 this is exactly 1.0 for every
// sample regardless of the LFO's value, so the multiply is an exact identity
// and the output is bit-for-bit the input (the required passthrough).
gain = 1.0 - TREMOLOAMT * (1.0 - lfoUni);

process = _ * gain;
