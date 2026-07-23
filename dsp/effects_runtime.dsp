// aloop runtime effects chain — the dubfx effect stages with the params exposed
// as UI controls (hslider/checkbox) instead of compile-time constants, so the
// remappable control map can set the knobs LIVE. The DSP is the same verified
// dubfx math (imported from effects/home/faust/); only the param SOURCE changes
// from a baked constant to a runtime UI zone. The zone labels match targetToZone
// in the native shell (HPCUT, LPCUT, LPRES, REVAMT, DELAYAMT, TIME, FORMANT, SEMIS).
//
// 3-BANK FX CONTROL SURFACE (LOFI feature): ApcGrid (src/control/apc_grid.cpp)
// tracks which of 3 banks -- Dub, Guitar, LofiFx (FxBank enum) -- is currently
// active on the control surface, but it always writes the ACTIVE bank's knob
// values into the SAME 7 shared Faust zones this file already declared for the
// dub bank (kFxZoneNames: fx/reverb, fx/delay, fx/time, fx/hp, fx/lpres, fx/lp,
// fx/pitch -> REVAMT, DELAYAMT, TIME, HPCUT, LPRES, LPCUT, SEMIS). So rather
// than rename/add zones per bank, the SAME 7 hslider zones are reinterpreted
// per-bank on the DSP side:
//   Dub    (fx/bank=0): REVAMT/DELAYAMT/TIME/HPCUT/LPRES/LPCUT/SEMIS drive the
//                        existing pitch/delay/reverb/microrepeat/filter chain,
//                        UNCHANGED from before this feature.
//   Guitar (fx/bank=1): REVAMT->FLANGEAMT, DELAYAMT->TREMOLOAMT, TIME->BANKSPEED
//                        (shared tremolo/phaser rate), HPCUT->PHASERAMT. Chain:
//                        flanger -> tremolo -> phaser (matches the dub chain's
//                        own "each stage in series" shape).
//   LofiFx (fx/bank=2): REVAMT->BITCRUSHAMT, DELAYAMT->VINYLAMT,
//                        TIME->FLUTTERAMT, HPCUT->SRRAMT. Chain: bitcrush ->
//                        vinyl -> flutter -> samplerate.
// A new 8th zone, "fx/bank" (nentry, 0/1/2, default 0=Dub), selects which
// bank's fully-computed chain reaches the output. ApcGrid writes this whenever
// the active bank changes (onDubFxPress/onGuitarFxPress/onLofiFxPress).
//
// Compressor (bottom-row guitar-bank knob) is wired here: effects/home/faust/
// compressor.dsp exists and is verified (byte-exact passthrough at 0, real
// monotonic loudness increase with no unbounded clipping across the dial --
// see the file's own header comment for the full generation history). It
// reuses the 7th shared zone (fx/pitch -> SEMIS), which the guitar bank has
// no other use for (unlike attack/release which occupy indices 4/5 and
// flange/tremolo/speed/phaser which occupy 0-3) -- apc_grid.h's own comment
// ("compress lives outside this 7-knob array") is superseded by this: it now
// DOES occupy a slot, the previously-unused 7th one.
//
// Switch behavior: fx/bank is smoothed (si.smoo, matching aloop.dsp's own
// MONITORFOLD/GLITCHFOLD smoothing idiom) and used to derive continuous
// per-bank crossfade weights (a triangular tent around each integer bank
// index), so switching banks mid-performance crossfades smoothly rather than
// hard-cutting -- see PRD row bank-switch-mid-performance-no-glitch. At
// fx/bank's compiled-in default (0.0, never touched), si.smoo's one-pole
// feedback state stays at exactly 0.0 (0-input in, 0 out, forever), so
// bankWeight[0]=1.0/others=0.0 EXACTLY -- the crossfade sum degenerates to
// dubChain*1.0 + guitarChain*0.0 + lofiChain*0.0, preserving the pre-existing
// byte-exact dub-bank passthrough at the documented defaults.
import("stdfaust.lib");

// runtime param controls (labels the control map targets bind to)
HPCUT    = hslider("HPCUT",   0.0, 0.0, 1.0, 0.001);
LPCUT    = hslider("LPCUT",   1.0, 0.0, 1.0, 0.001);
LPRES    = hslider("LPRES",   0.0, 0.0, 1.0, 0.001);
REVAMT   = hslider("REVAMT",  0.0, 0.0, 1.0, 0.001);
DELAYAMT = hslider("DELAYAMT",0.0, 0.0, 1.0, 0.001);
TIME     = hslider("TIME",    0.5, 0.0, 1.0, 0.001);
FORMANT  = hslider("FORMANT", 0.0, -3.0, 3.0, 0.001);
SEMIS    = hslider("SEMIS",   0.0, -12.0, 12.0, 0.001);
ENGAGED  = checkbox("ENGAGED");
// Microrepeat (apc_grid.cpp notes 82-86 -> fx/microrepeat_div): DIV is the beat
// divisor {0=off,1,2,4,8,16} set live from the control map; MLB is the current
// loop's length in blocks (masterLoopBlocks), read from the same varispeed grid
// the looper uses for Link sync so a repeat slice stays musically aligned.
DIV      = nentry("DIV", 0, 0, 16, 1);
MLB      = nentry("MLB", 0, 0, 4096, 1);

// New 8th zone: which bank's chain is active (0=Dub, 1=Guitar, 2=LofiFx),
// written by ApcGrid on every bank switch (see top-of-file comment).
FXBANK = nentry("fx/bank", 0, 0, 2, 1);

// Reuse the verified dubfx stage components with these runtime params.
filterStage = component("effects/home/faust/filters.dsp")[ HPCUT=HPCUT; LPCUT=LPCUT; LPRES=LPRES; ];
delayStage  = component("effects/home/faust/delay.dsp")[ DELAYAMT=DELAYAMT; TIME=TIME; ];
reverbStage = component("effects/home/faust/reverb.dsp")[ REVAMT=REVAMT; TIME=TIME; ];
microStage  = component("effects/home/faust/microrepeat.dsp")[ DIV=DIV; MLB=MLB; ];
pitchStage  = component("effects/home/faust/pitch.dsp")[ SEMIS=SEMIS; FORMANT=FORMANT; ENGAGED=ENGAGED; ];

// ---- Guitar bank: flanger -> tremolo -> phaser, in series (matches the dub
// chain's own "each stage feeds the next" shape). The 3 new guitar stages
// each read their own hslider zone internally; bind those zones to the SAME
// 7 shared fx/* values the dub bank already uses (see top-of-file mapping).
// tremolo.dsp/phaser.dsp share one physical BANKSPEED zone (Faust unifies
// identical-label hsliders across component() imports), so binding it once
// via tremoloStage's parameter list also drives phaserStage's copy.
flangerStage = component("effects/home/faust/flanger.dsp")[ FLANGEAMT=REVAMT; ];
tremoloStage = component("effects/home/faust/tremolo.dsp")[ TREMOLOAMT=DELAYAMT; BANKSPEED=TIME; ];
phaserStage  = component("effects/home/faust/phaser.dsp")[ PHASERAMT=HPCUT; BANKSPEED=TIME; ];
// Compressor reuses the fx/pitch zone (SEMIS) -- see top-of-file comment.
// SEMIS's own Faust-declared range is -12..12 (for the dub bank's pitch use),
// but ApcGrid's onFxKnobCC always normalizes to data2/127 in [0,1] before
// writing any of the 7 shared zones (apc_grid.cpp), so the value this zone
// ACTUALLY receives at runtime is already [0,1] regardless of SEMIS's own
// declared slider bounds -- clamped here anyway as defense-in-depth in case
// this zone is ever driven some other way (e.g. a future direct flat-map
// binding), since compressor.dsp's own passthrough proof assumes COMPRESSAMT
// genuinely stays within [0,1].
compressorGuitarAmt = max(0.0, min(1.0, SEMIS));
compressorStage = component("effects/home/faust/compressor.dsp")[ COMPRESSAMT=compressorGuitarAmt; ];
guitarChain(x) = x : flangerStage : tremoloStage : phaserStage : compressorStage;

// ---- Lofi-fx bank: bitcrush -> vinyl -> flutter -> samplerate, in series.
bitcrushStage   = component("effects/home/faust/bitcrush.dsp")[ BITCRUSHAMT=REVAMT; ];
vinylStage      = component("effects/home/faust/vinyl.dsp")[ VINYLAMT=DELAYAMT; ];
flutterStage    = component("effects/home/faust/flutter.dsp")[ FLUTTERAMT=TIME; ];
samplerateStage = component("effects/home/faust/samplerate.dsp")[ SRRAMT=HPCUT; ];
lofiFxChain(x) = x : bitcrushStage : vinylStage : flutterStage : samplerateStage;

// ---- Dub bank (the existing, unchanged chain) as a single-input function so
// it can be crossfaded against guitarChain/lofiFxChain below. Filter is
// deliberately excluded from `dubChain` itself (it stays applied to the
// POST-select signal, alongside the second glitch-tap output, unchanged from
// before this feature) -- see `process` below.
dubChain(x) = x : pitchStage : delayStage : reverbStage : microStage;

// ---- Bank crossfade ----
// Smooth the raw integer bank index (si.smoo, same idiom as aloop.dsp's
// MONITORFOLD/GLITCHFOLD) so a bank switch ramps rather than steps -- avoids
// an audible click/discontinuity when jumping between banks mid-performance
// (PRD: bank-switch-mid-performance-no-glitch). At the compiled-in default
// (FXBANK=0, never touched) si.smoo's feedback state never leaves 0.0 (0 in,
// 0 out, forever), so this smoothing introduces NO deviation from the
// pre-existing dub-bank passthrough.
bankSmooth = FXBANK : si.smoo;

// Triangular tent weight around each integer bank index: 1.0 exactly AT that
// index, falling linearly to 0.0 by +-1 away, clamped so it never goes
// negative (i.e. a partition-of-unity crossfade across the 0..2 range, exact
// unity gain at any integer bank value and a linear ramp mid-switch).
tentWeight(centerIdx) = max(0.0, 1.0 - abs(bankSmooth - centerIdx));

wDub    = tentWeight(0.0);
wGuitar = tentWeight(1.0);
wLofi   = tentWeight(2.0);

// ---- Top-level signal graph ----
// One input is fanned to all 3 banks in parallel, yielding a 4-wire bus:
// (dubForMix, dubForTap, guitarOut, lofiOut). dubStage fans its OWN single
// computed result out to 2 of those wires (`<: (*(wDub),*(wDub))`) so
// pitchStage/delayStage/reverbStage/microStage run as ONE shared instance --
// never recomputed a second time for the glitch tap -- keeping their
// internal state (delay lines, reverb tails, microrepeat counters) as one
// source of truth no matter which bank is currently selected. Each stage is
// already weighted by its own bank's crossfade weight before it reaches
// the bus, so summing/selecting downstream needs no further gating.
dubStage(x)    = x : dubChain <: (*(wDub), *(wDub));
guitarStage(x) = x : guitarChain : *(wGuitar);
lofiStage(x)   = x : lofiFxChain : *(wLofi);
fourBus(x)     = x <: (dubStage, guitarStage, lofiStage);

// fourBus's 4-wire bus is fanned to 2 identical 4-wire copies (`<:`) so each
// downstream branch can `route` its own copy independently -- Faust requires
// a `(A,B)` parallel group's total INPUT count to match the upstream stage's
// OUTPUT count exactly, so one 4-wire bus can't feed two separate
// full-arity consumers without this explicit duplication first.
//   Main (filtered, crossfaded) output: route picks wires 1,3,4 (dubForMix,
//   guitarOut, lofiOut), sums them, applies filterStage (same filter stage
//   the dub bank always used, now applied to whichever bank is selected).
//   Glitch tap: wire 2 (dubForTap) alone -- already wDub-weighted inside
//   dubStage, so it's silent whenever a non-dub bank is fully active (see
//   top-of-file doc for why: microStage's tap is only meaningful for dub).
process = fourBus <: (
    ( route(4,3, (1,1),(3,2),(4,3)) : (+,_) : + : filterStage ),
    ( route(4,1, (2,1)) )
);
