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

harmonize = component("effects/home/faust/multitranspose.dsp");

// Chain order (REVISED per direct user correction after the first reorder):
// glitch (microrepeat) -> filter -> delay -> reverb. The user's own words:
// "filter isnt applying to glitch, glitch should be before filter" and
// "glitch, once filtered, should feed into delay and reverb" -- so
// microStage moves BEFORE filterStage (glitch content must be filterable,
// which it wasn't when filter ran first), and filterStage stays before
// delay/reverb (from the earlier reorder: turning the filter should shape
// what feeds the reverb/delay tails, not just the dry/direct signal).
// Previous (now superseded) order was pitch->filter->delay->reverb->micro;
// original (pre-session) order was pitch->delay->reverb->micro->filter.
//
// Single output (was a duplicated <: (_, _) fanout feeding a second
// `rawGlitchTap` program output on aloop.dsp's mixAndFx -- confirmed dead
// there, see AGENTS.md's "confirmed-dead rawGlitchTap output" entry, and
// removed from both files together).
process(dry, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5) =
    (pitchStage(dry) + harmonize(dry, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5))
    : microStage : filterStage : delayStage : reverbStage;
