declare name "Resonode";
declare author "aloop";
declare license "GPLv3";
declare description "4-voice, 6-mode-per-voice modal-resonator instrument, packaged as its own standalone LV2 bundle (resonode.lv2) rather than living inside the always-on home Faust stack. Excited only by the live mic/dry input -- there is no synthetic strike or self-contained exciter anywhere in the signal path. Architecture (a shared exciter driving a mode-filter bank per voice) ported from DawDreamer's examples/resonaut/resonaut.py. This DSP previously lived inside effects_runtime.dsp/aloop.dsp (the always-on Core-1 Faust program), where its per-block cost was paid on every block regardless of whether the instrument was engaged -- confirmed via real-hardware testing that even a reduced 4x2 shape produced a sustained ~2ms readi gap (vs a 1.333ms budget) with the instrument fully idle, since Faust has no runtime branching to skip a stage's cost. Moving it to its own LV2 bundle lets the C++ host (src/dsp/audio_thread.cpp) call process() on it ONLY when fx/resonode/engaged is true -- genuine cost elimination at the call-site level, not a Faust-side crossfade. note/gate/vel per voice are now LV2 control ports (hslider), matching guitar_lofi_fx.dsp's own control-port convention for this codebase's LV2 bundles, rather than Faust signal inputs -- they are already only ever updated once per control-tick from ApcGrid (MIDI note-on/off), never per-sample, so this is a lossless representation change. collision is a per-patch bounded soft-clip/waveshape amount applied to each voice's own resonator output (0 = exact passthrough); pitch-mod is a small, velocity- and dispersion-scaled onset frequency deviation that decays back to the true pitch over ~40ms, modeling impact deformation without any synthetic exciter to carry it.";

import("stdfaust.lib");

morphGlidePole = ba.tau2pole(0.015);
morphIsFirstSample = ba.time == 0;
morphGlide(x) = y
letrec {
    'y = ba.if(morphIsFirstSample, x, y + (x - y)*(1.0 - morphGlidePole));
};

position  = hslider("fx/resonode/position", 0.35, 0.0, 1.0, 0.001) : morphGlide;
tone      = hslider("fx/resonode/tone", 6000.0, 200.0, 18000.0, 1.0) : morphGlide;
decayTime = hslider("fx/resonode/decay", 1.2, 0.05, 8.0, 0.001) : morphGlide;
damping   = hslider("fx/resonode/damping", 0.85, 0.05, 1.0, 0.001) : morphGlide;
stretch   = hslider("fx/resonode/stretch", 0.0, -0.5, 1.5, 0.001) : morphGlide;
collision = hslider("fx/resonode/collision", 0.0, 0.0, 1.0, 0.001) : morphGlide;
outLevel  = hslider("fx/resonode/level", 0.8, 0.0, 1.5, 0.001) : morphGlide;

note0 = hslider("fx/resonodevoice0/note", -1.0, -1.0, 127.0, 1.0);
gate0 = hslider("fx/resonodevoice0/gate", 0.0, 0.0, 1.0, 1.0);
vel0  = hslider("fx/resonodevoice0/vel",  1.0, 0.0, 1.0, 0.001);
note1 = hslider("fx/resonodevoice1/note", -1.0, -1.0, 127.0, 1.0);
gate1 = hslider("fx/resonodevoice1/gate", 0.0, 0.0, 1.0, 1.0);
vel1  = hslider("fx/resonodevoice1/vel",  1.0, 0.0, 1.0, 0.001);
note2 = hslider("fx/resonodevoice2/note", -1.0, -1.0, 127.0, 1.0);
gate2 = hslider("fx/resonodevoice2/gate", 0.0, 0.0, 1.0, 1.0);
vel2  = hslider("fx/resonodevoice2/vel",  1.0, 0.0, 1.0, 0.001);
note3 = hslider("fx/resonodevoice3/note", -1.0, -1.0, 127.0, 1.0);
gate3 = hslider("fx/resonodevoice3/gate", 0.0, 0.0, 1.0, 1.0);
vel3  = hslider("fx/resonodevoice3/vel",  1.0, 0.0, 1.0, 0.001);

voiceGain = 0.5;
retuneGlide = 0.01;
pitchModDepth = 0.04;
pitchModDecayS = 0.04;
pitchModPole = pow(0.001, 1.0/(pitchModDecayS*ma.SR));

stealEvent(note, gate) = (note != note') * (gate <= gate');
retriggerGate(note, gate) = gate * (1.0 - stealEvent(note, gate));
attackEdge(note, gate) = (gate > gate') + stealEvent(note, gate);

velGain(vel) = max(0.0, min(1.0, vel));
flexibility = max(0.0, min(1.0, (0.5 - stretch)));

pitchEnv(note, gate) = e
letrec {
    'e = ba.if(attackEdge(note, gate) > 0.5, 1.0, e * pitchModPole);
};

sharedExciteIn(exciteIn) = exciteIn : fi.lowpass(2, tone);

exciteFor(sharedIn, note, gate, vel) = sharedIn * (en.asr(0.02, 1.0, 0.3, xgate) * velGain(vel))
with {
    xgate = retriggerGate(note, gate);
};

freqGlide(note, gate, vel) = f
letrec {
    'f = ba.if(gate > gate', target, f + (target - f)*retuneGlide)
    with { target = ba.midikey2hz(note) * (1.0 + pitchModDepth*velGain(vel)*flexibility*pitchEnv(note, gate)); };
};

driveAmt = 1.0 + collision*6.0;
driveNorm = 1.0 / ma.tanh(driveAmt);
collisionDrive(x) = x*(1.0 - collision) + ma.tanh(x*driveAmt)*driveNorm*collision;

aliasGuard(f) = min(1.0, max(0.0, (ma.SR*0.5 - f) / (ma.SR*0.05)));

stretchRatio2 = pow(2.0, 1.0+stretch);
stretchRatio3 = pow(3.0, 1.0+stretch);
stretchRatio4 = pow(4.0, 1.0+stretch);
stretchRatio5 = pow(5.0, 1.0+stretch);
stretchRatio6 = pow(6.0, 1.0+stretch);

modeGain1 = 1.00*abs(sin(ma.PI*position*1));
modeGain2 = 0.60*abs(sin(ma.PI*position*2));
modeGain3 = 0.40*abs(sin(ma.PI*position*3));
modeGain4 = 0.30*abs(sin(ma.PI*position*4));
modeGain5 = 0.22*abs(sin(ma.PI*position*5));
modeGain6 = 0.16*abs(sin(ma.PI*position*6));

dampSq   = damping*damping;
dampCube = dampSq*damping;
dampQuad = dampCube*damping;
dampQuin = dampQuad*damping;

modeR(t60) = pow(0.001, 1.0/(t60*ma.SR));
r1 = modeR(decayTime);
r2 = modeR(decayTime*damping);
r3 = modeR(decayTime*dampSq);
r4 = modeR(decayTime*dampCube);
r5 = modeR(decayTime*dampQuad);
r6 = modeR(decayTime*dampQuin);

modeFilterR(r, freq, gain) = fi.tf2(1.0, 0.0, -1.0, a1, a2) * gain
with {
    a1 = -2.0*r*cos(2.0*ma.PI*freq/ma.SR);
    a2 = r*r;
};

bank(freqHz, exc) = exc <: (m1, m2, m3, m4, m5, m6) :> _
with {
    f2 = freqHz*stretchRatio2;
    f3 = freqHz*stretchRatio3;
    f4 = freqHz*stretchRatio4;
    f5 = freqHz*stretchRatio5;
    f6 = freqHz*stretchRatio6;
    m1 = modeFilterR(r1, freqHz, modeGain1*aliasGuard(freqHz));
    m2 = modeFilterR(r2, f2,     modeGain2*aliasGuard(f2));
    m3 = modeFilterR(r3, f3,     modeGain3*aliasGuard(f3));
    m4 = modeFilterR(r4, f4,     modeGain4*aliasGuard(f4));
    m5 = modeFilterR(r5, f5,     modeGain5*aliasGuard(f5));
    m6 = modeFilterR(r6, f6,     modeGain6*aliasGuard(f6));
};

voice(sharedIn, note, gate, vel) = collisionDrive(bank(freqGlide(note, gate, vel), exciteFor(sharedIn, note, gate, vel))) * voiceGain;

process(exciteIn) =
    (voice(sharedIn,note0,gate0,vel0) + voice(sharedIn,note1,gate1,vel1) + voice(sharedIn,note2,gate2,vel2) + voice(sharedIn,note3,gate3,vel3)) : ma.tanh : *(outLevel)
with {
    sharedIn = sharedExciteIn(exciteIn);
};
