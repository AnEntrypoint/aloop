declare name "MultiKeyTranspose";
declare author "aloop";
declare license "GPLv3";

import("stdfaust.lib");

NVOICES = 6;

glideTau = ba.tau2pole(0.008);
windowSamples = int(0.010 * ma.SR) : max(64);
xfadeSamples  = int(windowSamples * 0.5) : max(32);
voiceGain = 0.6;

voiceOut(sig, semis, gate) = wet
with {
    shiftAmount = semis : si.smooth(glideTau);
    voiceEnv    = en.adsr(0.003, 0.03, 1, 0.05, gate);
    wet = (sig : ef.transpose(windowSamples, xfadeSamples, shiftAmount)) * voiceEnv * voiceGain;
};

harmonySum(sig, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5) =
    voiceOut(sig,s0,g0) + voiceOut(sig,s1,g1) + voiceOut(sig,s2,g2)
  + voiceOut(sig,s3,g3) + voiceOut(sig,s4,g4) + voiceOut(sig,s5,g5);

process(sig, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5) =
    harmonySum(sig, s0,g0, s1,g1, s2,g2, s3,g3, s4,g4, s5,g5) : ma.tanh;
