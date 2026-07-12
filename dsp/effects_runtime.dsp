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
REVAMT   = hslider("REVAMT",  0.0, 0.0, 1.0, 0.001);
DELAYAMT = hslider("DELAYAMT",0.0, 0.0, 1.0, 0.001);
TIME     = hslider("TIME",    0.5, 0.0, 1.0, 0.001);
FORMANT  = hslider("FORMANT", 0.0, -3.0, 3.0, 0.001);
SEMIS    = hslider("SEMIS",   0.0, -12.0, 12.0, 0.001);
ENGAGED  = checkbox("ENGAGED");
DIV      = 0;      // microrepeat div: a command, wired separately if used
MLB      = 0;

// Reuse the verified dubfx stage components with these runtime params.
filterStage = component("effects/home/faust/filters.dsp")[ HPCUT=HPCUT; LPCUT=LPCUT; LPRES=LPRES; ];
delayStage  = component("effects/home/faust/delay.dsp")[ DELAYAMT=DELAYAMT; TIME=TIME; ];
reverbStage = component("effects/home/faust/reverb.dsp")[ REVAMT=REVAMT; TIME=TIME; ];
microStage  = component("effects/home/faust/microrepeat.dsp")[ DIV=DIV; MLB=MLB; ];
pitchStage  = component("effects/home/faust/pitch.dsp")[ SEMIS=SEMIS; FORMANT=FORMANT; ENGAGED=ENGAGED; ];

process = pitchStage : delayStage : reverbStage : microStage : filterStage;
