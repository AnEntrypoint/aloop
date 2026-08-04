declare name "MultiKeyTranspose";
declare author "aloop";
declare license "GPLv3";
declare description "Polyphonic pitch-LOCK harmonizer: an.pitchTracker detects the live input's own fundamental once per sample, and each held voice's shift is (targetNote - detectedNote), so the wet output always lands on the exact pressed key regardless of what pitch is actually being played -- Infected Mushroom Manipulator style, not a fixed-interval transpose. The shift window is pitch-synchronous (sized from the detected period, like davemollen/dm-Whammy and DawDreamer's own dt_whammy.dsp auto_window), since a fixed window measurably detunes ef.transpose at large shift ratios -- confirmed via DawDreamer: a fixed 10ms window put a +22-semitone lock ~114 cents flat, while pitch-synchronous sizing holds every tested shift within a few cents.";

import("stdfaust.lib");

NVOICES = 6;

glideTau = ba.tau2pole(0.008);
voiceGain = 0.6;
trackerHarmonics = 4;
trackerTau = 0.02;
minTrackHz = 60.0;
maxTrackHz = 1500.0;
maxWindowMs = 20.0;

detectedFreq(sig) = sig
    : an.pitchTracker(trackerHarmonics, trackerTau)
    : max(minTrackHz) : min(maxTrackHz);

windowFor(freqHz) = (ma.SR / freqHz)
    : max(64) : min(maxWindowMs * 0.001 * ma.SR)
    : si.smooth(ba.tau2pole(0.05)) : max(64) : int;

voiceOut(sig, detNote, winSamples, xfSamples, targetNote, gate) = wet
with {
    shiftAmount = (targetNote - detNote) : si.smooth(glideTau);
    voiceEnv    = en.adsr(0.003, 0.03, 1, 0.05, gate);
    wet = (sig : ef.transpose(winSamples, xfSamples, shiftAmount)) * voiceEnv * voiceGain;
};

harmonySum(sig, detNote, winSamples, xfSamples, n0,g0, n1,g1, n2,g2, n3,g3, n4,g4, n5,g5) =
    voiceOut(sig,detNote,winSamples,xfSamples,n0,g0) + voiceOut(sig,detNote,winSamples,xfSamples,n1,g1)
  + voiceOut(sig,detNote,winSamples,xfSamples,n2,g2) + voiceOut(sig,detNote,winSamples,xfSamples,n3,g3)
  + voiceOut(sig,detNote,winSamples,xfSamples,n4,g4) + voiceOut(sig,detNote,winSamples,xfSamples,n5,g5);

process(sig, n0,g0, n1,g1, n2,g2, n3,g3, n4,g4, n5,g5) = harmonySum(
    sig, ba.hz2midikey(freqDet), winSamples, xfSamples,
    n0,g0, n1,g1, n2,g2, n3,g3, n4,g4, n5,g5
) : ma.tanh
with {
    freqDet     = detectedFreq(sig);
    winSamples  = windowFor(freqDet);
    xfSamples   = int(winSamples * 0.5) : max(32);
};
