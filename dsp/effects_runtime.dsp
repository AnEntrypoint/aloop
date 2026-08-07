declare name "EffectsRuntime";
declare author "aloop";
declare description "The dubfx effect stages with every param exposed as a runtime UI control (hslider/checkbox/nentry) instead of a compile-time constant, so the remappable control map can set them live. Zone labels match targetToZone in the native shell (HPCUT, LPCUT, LPRES, REVAMT, DELAYAMT, TIME, FORMANT, SEMIS); any fx/resonode/... or fx/xpose.../... target passes through targetToZone verbatim.";

import("stdfaust.lib");

HPCUT    = hslider("HPCUT",   0.0, 0.0, 1.0, 0.001);
LPCUT    = hslider("LPCUT",   1.0, 0.0, 1.0, 0.001);
LPRES    = hslider("LPRES",   0.0, 0.0, 1.0, 0.001);
REVAMT   = hslider("REVAMT",  0.0, 0.0, 2.0, 0.001);
DELAYAMT = hslider("DELAYAMT",0.0, 0.0, 1.0, 0.001);
TIME     = hslider("TIME",    0.5, 0.0, 1.0, 0.001);
FORMANT  = hslider("FORMANT", 0.0, -3.0, 3.0, 0.001);
SEMIS    = hslider("SEMIS",   0.0, -12.0, 12.0, 0.001);
ENGAGED  = checkbox("ENGAGED");
RESONODE_ENGAGED = checkbox("fx/resonode/engaged");
glitchDivisor    = nentry("DIV", 0, 0, 16, 1);
masterLoopBlocks = nentry("MLB", 0, 0, 4096, 1);

filterStage = component("effects/home/faust/filters.dsp")[ HPCUT=HPCUT; LPCUT=LPCUT; LPRES=LPRES; ];
delayStage  = component("effects/home/faust/delay.dsp")[ DELAYAMT=DELAYAMT; TIME=TIME; ];
reverbStage = component("effects/home/faust/reverb.dsp")[ REVAMT=REVAMT; TIME=TIME; ];
microStage  = component("effects/home/faust/microrepeat.dsp")[ DIV=glitchDivisor; MLB=masterLoopBlocks; ];
pitchStage  = component("effects/home/faust/pitch.dsp")[ SEMIS=SEMIS; FORMANT=FORMANT; ENGAGED=ENGAGED; ];

harmonize = component("effects/home/faust/multitranspose.dsp");
resonode  = component("effects/home/faust/resonode_synth.dsp");

process(dry, loopSum, freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5, on0,og0,ov0, on1,og1,ov1, on2,og2,ov2, on3,og3,ov3) = mainOut, loopHarmonyWet
with {
    anyVoiceGated = min(1.0, g0+g1+g2+g3+g4+g5);
    dryGate = (1.0 - anyVoiceGated*(1.0-freeXpose)) : si.smoo;
    resonodeEngageGate = RESONODE_ENGAGED : si.smoo;
    resonodeOut = 0.0;
    harmonyBus     = harmonize(dry, loopSum, freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5);
    dryWet         = harmonyBus : _,!;
    loopHarmonyWet = harmonyBus : !,_;
    preChain = (pitchStage(dry)*dryGate + dryWet)*(1.0-resonodeEngageGate) + resonodeOut*resonodeEngageGate;
    mainOut = preChain : microStage : filterStage : delayStage : reverbStage;
};
