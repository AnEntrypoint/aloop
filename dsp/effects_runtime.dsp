declare name "EffectsRuntime";
declare author "aloop";
declare description "The dubfx effect stages with every param exposed as a runtime UI control (hslider/checkbox/nentry) instead of a compile-time constant, so the remappable control map can set them live. Zone labels match targetToZone in the native shell (HPCUT, LPCUT, LPRES, REVAMT, DELAYAMT, TIME, FORMANT, SEMIS); any fx/resonode/... or fx/xpose.../... target passes through targetToZone verbatim. resonodeIn is now an external audio-rate signal input (the resonode.lv2 bundle's own output, computed in C++ audio_thread.cpp ONLY when fx/resonode/engaged is true) rather than an in-Faust component -- Resonode's per-block cost used to be paid unconditionally on every block even when idle, since Faust has no runtime branching to skip a stage. dry is now the guitar/lofi-fx LV2 bundle's OUTPUT (an input-stage effect that now runs before this whole chain), not the raw mic signal.";

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

process(dry, loopSum, freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5, resonodeIn) = mainOut, loopHarmonyWet
with {
    anyVoiceGated = min(1.0, g0+g1+g2+g3+g4+g5);
    dryGate = (1.0 - anyVoiceGated*(1.0-freeXpose)) : si.smoo;
    resonodeEngageGate = RESONODE_ENGAGED : si.smoo;
    harmonyBus     = harmonize(dry, loopSum, freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5);
    dryWet         = harmonyBus : _,!;
    loopHarmonyWet = harmonyBus : !,_;
    preChain = (pitchStage(dry)*dryGate + dryWet)*(1.0-resonodeEngageGate) + resonodeIn*resonodeEngageGate;
    mainOut = preChain : microStage : filterStage : delayStage : reverbStage;
};
