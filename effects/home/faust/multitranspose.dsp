declare name "MultiKeyTranspose";
declare author "aloop";
declare license "GPLv3";
declare description "Polyphonic pitch-LOCK harmonizer: an.pitchTracker detects the tracked signal's own fundamental once per sample, and each held voice's shift is (targetNote - detectedNote), so the wet output always lands on the exact pressed key regardless of what pitch is actually being played -- Infected Mushroom Manipulator style, not a fixed-interval transpose. The shift window is pitch-synchronous (sized from the detected period, like davemollen/dm-Whammy and DawDreamer's own dt_whammy.dsp auto_window), since a fixed window measurably detunes ef.transpose at large shift ratios -- confirmed via DawDreamer: a fixed 10ms window put a +22-semitone lock ~114 cents flat, while pitch-synchronous sizing holds every tested shift within a few cents. free crossfades the tracked/shifted source between dry (free=0) and loopSum (free=1), and routes the single shared wet bus to the matching output (dryWet/loopWet) -- so SHIFT-held polyphonic keyplay locks the loops instead of the live input, with no second pitch-tracker/voice bank. trackPitchHz reimplements an.pitchTracker's zero-crossing/adaptive-lowpass loop locally with a fast broadband zero-crossing floor on the adaptive cutoff: the stock function's cutoff is bounded only by a hardcoded 20Hz and its own zero-initialized recursive state, so after silence a fresh attack's cutoff has to crawl up from that floor over hundreds of ms, starving the filter of the true fundamental and reading far too low -- since shiftAmount is targetNote-detNote, an under-read detNote inflates the locked output a chunk of an octave sharp for the whole crawl, audible as a slide down into the held key. WITNESSED via the DawDreamer JIT harness: a fresh 196Hz attack read ~20 semitones sharp for ~250ms before dropping to the true pitch. Seeding the cutoff's floor from a fast (3ms-tau) unfiltered zero-crossing estimate of the same highpassed signal (halved, so a transient's own broadband noise can't itself push the floor an octave high) collapses that convergence to within a couple of semitones by 60-100ms across the tested 82-880Hz range, with the already-converged/steady-state reading numerically unchanged (the floor is a max() against the original recursive value, never a replacement of it) and silence/chord/no-op cases unaffected.";

import("stdfaust.lib");

NVOICES = 6;

glideTau = ba.tau2pole(0.008);
voiceGain = 0.6;
trackerHarmonics = 4;
trackerTau = 0.02;
minTrackHz = 60.0;
maxTrackHz = 1500.0;
maxWindowMs = 20.0;

coarseTrackerTau = 0.003;

trackPitchHz(N, t, x) = loop ~ _
with {
    xHighpassed = fi.highpass(1, 20.0, x);
    coarseHz = an.zcr(coarseTrackerTau, xHighpassed) * ma.SR * .5;
    loop(y) = an.zcr(t, fi.lowpass(N, cutoff, xHighpassed)) * ma.SR * .5
    with {
        cutoff = max(minTrackHz, max(y, coarseHz * .5));
    };
};

detectedFreq(sig) = sig
    : trackPitchHz(trackerHarmonics, trackerTau)
    : max(minTrackHz) : min(maxTrackHz);

windowFor(freqHz) = (ma.SR / freqHz)
    : max(64) : min(maxWindowMs * 0.001 * ma.SR)
    : si.smooth(ba.tau2pole(0.05)) : max(64) : int;

xposeMaxDelay = 4096;

xpose(w, x, s, sig) = de.fdelay(xposeMaxDelay,d,sig)*ma.fmin(d/x,1) +
    de.fdelay(xposeMaxDelay,d+w,sig)*(1-ma.fmin(d/x,1))
with {
    i = 1 - pow(2, s/12);
    d = i : (+ : +(w) : fmod(_,w)) ~ _;
};

voiceOut(sig, detNote, winSamples, xfSamples, targetNote, gate) = wet
with {
    shiftAmount = (targetNote - detNote) : si.smooth(glideTau);
    voiceEnv    = en.adsr(0.003, 0.03, 1, 0.05, gate);
    wet = (sig : xpose(winSamples, xfSamples, shiftAmount)) * voiceEnv * voiceGain;
};

harmonySum(sig, detNote, winSamples, xfSamples, n0,g0, n1,g1, n2,g2, n3,g3, n4,g4, n5,g5) =
    voiceOut(sig,detNote,winSamples,xfSamples,n0,g0) + voiceOut(sig,detNote,winSamples,xfSamples,n1,g1)
  + voiceOut(sig,detNote,winSamples,xfSamples,n2,g2) + voiceOut(sig,detNote,winSamples,xfSamples,n3,g3)
  + voiceOut(sig,detNote,winSamples,xfSamples,n4,g4) + voiceOut(sig,detNote,winSamples,xfSamples,n5,g5);

process(dry, loopSum, free, n0,g0, n1,g1, n2,g2, n3,g3, n4,g4, n5,g5) = dryWet, loopWet
with {
    freeSmooth = free : si.smoo;
    sigIn      = dry*(1.0-freeSmooth) + loopSum*freeSmooth;
    freqDet    = detectedFreq(sigIn);
    winSamples = windowFor(freqDet);
    xfSamples  = int(winSamples * 0.5) : max(32);
    wet = harmonySum(
        sigIn, ba.hz2midikey(freqDet), winSamples, xfSamples,
        n0,g0, n1,g1, n2,g2, n3,g3, n4,g4, n5,g5
    ) : ma.tanh;
    dryWet  = wet * (1.0-freeSmooth);
    loopWet = wet * freeSmooth;
};
