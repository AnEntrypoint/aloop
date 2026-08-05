import("stdfaust.lib");

loop = component("loop.dsp");
fx   = component("effects_runtime.dsp");

monitorFold = hslider("MONITORFOLD", 0.0, 0.0, 1.0, 1.0);
glitchFold = hslider("GLITCHFOLD", 0.0, 0.0, 1.0, 1.0);

mixAndFx(dry, loopSum, freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5, on0,og0, on1,og1, on2,og2, on3,og3) = filtOut, loopSum, recordTap
with {
    fxBus = (dry, loopSum, freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5, on0,og0, on1,og1, on2,og2, on3,og3) : fx;
    fxOuts         = fxBus : _,!;
    loopHarmonyWet = fxBus : !,_;
    anyVoiceGated  = min(1.0, g0+g1+g2+g3+g4+g5);
    loopDirectRaw  = 1.0 - max(max(monitorFold, glitchFold), anyVoiceGated*freeXpose);
    loopDirectGate = loopDirectRaw : si.smoo;
    filtOut = fxOuts + loopSum*loopDirectGate + loopHarmonyWet;
    recordTap = fxOuts + loopHarmonyWet;
};

process(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen, sidechainEnv, freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5, on0,og0, on1,og1, on2,og2, on3,og3) =
    (loop(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen, sidechainEnv), freeXpose, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5, on0,og0, on1,og1, on2,og2, on3,og3)
    : mixAndFx;
