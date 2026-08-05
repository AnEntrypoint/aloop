declare name "ObjektSynth";
declare author "aloop";
declare license "GPLv3";
declare description "4-voice modal-resonator instrument in the spirit of Reason Studios' Objekt -- an independent mode-filter-bank implementation, not a port of Objekt's own proprietary DSP. Architecture (shared exciter driving a bank of resonant modes per voice) ported from DawDreamer's examples/resonaut/resonaut.py (Resonaut, an from-scratch Objekt alternative already in this project's sibling DawDreamer repo), reduced from Resonaut's 3-object/8-mode/8-voice offline design to a single fixed STRING-like 4-mode object at 4 voices to fit aloop's real-time Pi 4 budget. exciteIn/note/gate are signal inputs, not hslider/button UI elements, matching multitranspose.dsp's own convention for momentary per-voice state and for feeding a live audio signal through as a control-rate-free wire.";

import("stdfaust.lib");

character = hslider("fx/objekt/character", 0.15, 0.0, 1.0, 0.001);
tone      = hslider("fx/objekt/tone", 6000.0, 200.0, 18000.0, 1.0);
objDecay  = hslider("fx/objekt/decay", 1.2, 0.05, 8.0, 0.001);
damping   = hslider("fx/objekt/damping", 0.85, 0.05, 1.0, 0.001);
stretch   = hslider("fx/objekt/stretch", 0.0, -0.5, 1.5, 0.001);
objLevel  = hslider("fx/objekt/level", 0.8, 0.0, 1.5, 0.001);

strikePos = 0.3;
strikeSharp = 0.7;
bankPosition = 0.35;
voiceGain = 0.5;
retuneGlide = 0.01;

stealEvent(note, gate) = (note != note') * (gate <= gate');
retriggerGate(note, gate) = gate * (1.0 - stealEvent(note, gate));

exciteFor(exciteIn, note, gate) = impact*(1.0-character) + wash*character
with {
    xgate = retriggerGate(note, gate);
    impact = pm.strike(strikePos, strikeSharp, 1.0, xgate) : fi.lowpass(2, tone);
    wash   = exciteIn : fi.lowpass(2, tone) : *(en.asr(0.02, 1.0, 0.3, xgate));
};

freqGlide(note, gate) = f
letrec {
    'f = ba.if(gate > gate', target, f + (target - f)*retuneGlide)
    with { target = ba.midikey2hz(note); };
};

aliasGuard(f) = min(1.0, max(0.0, (ma.SR*0.5 - f) / (ma.SR*0.05)));

mode1(freqHz) = pm.modeFilter(freqHz, objDecay, 1.0*abs(sin(ma.PI*bankPosition*1))*aliasGuard(freqHz));
mode2(freqHz) = pm.modeFilter(f2, objDecay*pow(damping,1), 0.6*abs(sin(ma.PI*bankPosition*2))*aliasGuard(f2)) with { f2 = freqHz*pow(2.0, 1.0+stretch); };
mode3(freqHz) = pm.modeFilter(f3, objDecay*pow(damping,2), 0.4*abs(sin(ma.PI*bankPosition*3))*aliasGuard(f3)) with { f3 = freqHz*pow(3.0, 1.0+stretch); };
mode4(freqHz) = pm.modeFilter(f4, objDecay*pow(damping,3), 0.3*abs(sin(ma.PI*bankPosition*4))*aliasGuard(f4)) with { f4 = freqHz*pow(4.0, 1.0+stretch); };

bank(freqHz, exc) = exc <: (mode1(freqHz), mode2(freqHz), mode3(freqHz), mode4(freqHz)) :> _;

voice(exciteIn, note, gate) = bank(freqGlide(note, gate), exciteFor(exciteIn, note, gate)) * voiceGain;

process(exciteIn, note0,gate0, note1,gate1, note2,gate2, note3,gate3) =
    (voice(exciteIn,note0,gate0) + voice(exciteIn,note1,gate1) + voice(exciteIn,note2,gate2) + voice(exciteIn,note3,gate3)) : ma.tanh : *(objLevel);
