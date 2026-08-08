declare name "Resonode";
declare author "aloop";
declare license "GPLv3";
declare description "4-voice modal-resonator instrument: a bank of tuned resonant modes per voice, excited only by the live mic/dry input -- there is no synthetic strike or self-contained exciter anywhere in the signal path. Architecture (a shared exciter driving a mode-filter bank per voice) ported from DawDreamer's examples/resonaut/resonaut.py, sized to 4 voices x 2 modes to fit aloop's real-time Pi 4 budget. A 2-voice x 6-mode real-hardware experiment (2026-08-08) also overran the 1.333ms audio-thread block deadline (readi took 2.341ms), so this reverts to the last confirmed-safe 4x2 shape -- do not raise voice or mode count again without a fresh real-Pi-4 crash test, not just a synthetic/compile check (see AGENTS.md's 'compiling clean proves nothing about runtime safety'). exciteIn/note/gate/vel are signal inputs, not hslider/button UI elements, matching multitranspose.dsp's own convention for momentary per-voice state fed through as control-rate-free wires. collision is a per-patch bounded soft-clip/waveshape amount applied to each voice's own resonator output (0 = exact passthrough); pitch-mod is a small, velocity- and dispersion-scaled onset frequency deviation that decays back to the true pitch over ~40ms, modeling impact deformation without any synthetic exciter to carry it.";

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

modeGain1 = 1.00*abs(sin(ma.PI*position*1));
modeGain2 = 0.60*abs(sin(ma.PI*position*2));

modeR(t60) = pow(0.001, 1.0/(t60*ma.SR));
r1 = modeR(decayTime);
r2 = modeR(decayTime*damping);

modeFilterR(r, freq, gain) = fi.tf2(1.0, 0.0, -1.0, a1, a2) * gain
with {
    a1 = -2.0*r*cos(2.0*ma.PI*freq/ma.SR);
    a2 = r*r;
};

bank(freqHz, exc) = exc <: (m1, m2) :> _
with {
    f2 = freqHz*stretchRatio2;
    m1 = modeFilterR(r1, freqHz, modeGain1*aliasGuard(freqHz));
    m2 = modeFilterR(r2, f2,     modeGain2*aliasGuard(f2));
};

voice(sharedIn, note, gate, vel) = collisionDrive(bank(freqGlide(note, gate, vel), exciteFor(sharedIn, note, gate, vel))) * voiceGain;

process(exciteIn, note0,gate0,vel0, note1,gate1,vel1, note2,gate2,vel2, note3,gate3,vel3) =
    (voice(sharedIn,note0,gate0,vel0) + voice(sharedIn,note1,gate1,vel1) + voice(sharedIn,note2,gate2,vel2) + voice(sharedIn,note3,gate3,vel3)) : ma.tanh : *(outLevel)
with {
    sharedIn = sharedExciteIn(exciteIn);
};
