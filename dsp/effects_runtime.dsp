// aloop runtime effects chain — the dubfx effect stages with the params exposed
// as UI controls (hslider/checkbox) instead of compile-time constants, so the
// remappable control map can set the knobs LIVE. The DSP is the same verified
// dubfx math (imported from effects/home/faust/); only the param SOURCE changes
// from a baked constant to a runtime UI zone. The zone labels match targetToZone
// in the native shell (HPCUT, LPCUT, LPRES, REVAMT, DELAYAMT, TIME, FORMANT, SEMIS).
import("stdfaust.lib");

// runtime param controls (labels the control map targets bind to)
HPCUT    = hslider("HPCUT",   0.0, 0.0, 1.0, 0.001);
LPCUT    = hslider("LPCUT",   1.0, 0.0, 1.0, 0.001);
LPRES    = hslider("LPRES",   0.0, 0.0, 1.0, 0.001);
// REVAMT max raised 1.0 -> 2.0 (user-requested: more reverb than the
// original hardware's ceiling allows). reverb.dsp's own verified/ported
// math (effects/home/faust/reverb.dsp) is untouched -- amt feeds linearly
// into `reverb(amt,t,x) = x + revL*amt*0.25`, so widening the UI range here
// (a runtime-control wrapper, not the hardware-parity-ported DSP itself)
// simply lets the wet mix scale further than the original hardware's own
// UI ever exposed, without altering the verified formula at REVAMT<=1.0.
REVAMT   = hslider("REVAMT",  0.0, 0.0, 2.0, 0.001);
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

// Reuse the verified dubfx stage components with these runtime params.
filterStage = component("effects/home/faust/filters.dsp")[ HPCUT=HPCUT; LPCUT=LPCUT; LPRES=LPRES; ];
delayStage  = component("effects/home/faust/delay.dsp")[ DELAYAMT=DELAYAMT; TIME=TIME; ];
reverbStage = component("effects/home/faust/reverb.dsp")[ REVAMT=REVAMT; TIME=TIME; ];
microStage  = component("effects/home/faust/microrepeat.dsp")[ DIV=DIV; MLB=MLB; ];
pitchStage  = component("effects/home/faust/pitch.dsp")[ SEMIS=SEMIS; FORMANT=FORMANT; ENGAGED=ENGAGED; ];

// Chain order: filter now runs BEFORE delay/reverb (user-requested: turning
// the filter should audibly shape what feeds the reverb/delay tails, e.g. a
// lowpass cut should darken the reverb wash too, not just the dry/direct
// signal). Previously filterStage ran LAST (after microStage, at the final
// <: split), so delay/reverb always received full-band input regardless of
// the filter knobs. microrepeat's position is UNCHANGED (still after
// reverb) -- only the filter moved earlier, per explicit confirmation this
// change should not also relocate microStage.
//
// Second output: aloop.dsp's mixAndFx unpacks this as `rawGlitchTap`, a
// leftover tap from an EARLIER design (the old separate glitchIn/
// prevGlitchTap record-path mechanism, since replaced by prevFiltOut --
// see aloop.dsp's own top-of-file history and audio_thread.cpp's
// "REPLACES the old glitch-only prevGlitchTap wiring" comment). Confirmed
// dead: audio_thread.cpp's fouts[1] (rawGlitchTap) is populated every
// block but never read again anywhere in that file -- so this second
// output has no live consumer today, and simply mirrors the same
// (filtered) signal as output 1 rather than needing a separate pre-filter
// tap.
process = pitchStage : filterStage : delayStage : reverbStage : microStage <: (_, _);
