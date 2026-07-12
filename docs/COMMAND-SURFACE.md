# Command surface — the looper control behavior aloop clones

100% clone includes the control/command behavior, not just audio. aloop
reproduces looper's full command surface (the ported apcKey25*.cpp logic + loop
commands), with the input source moved to ALSA rawmidi (src/control/midi.cpp).

## Loop commands (ported from looper loopMachine command dispatch)
record · play · stop · clear/erase (per track) · halve/double speed (varispeed) · loop-immediate · set/clear loop-start — for each of the 20 independent loopers. NO overdub.
· set/clear mark point · pause (mute-with-advancing-head) · quantize-to-grid.

## APC Key25 controls (the exact mapping — src/control/midi.cpp, param_mapping.md)
- CC48–55 → reverb/delay/time/HP/LP/resonance (all `/127`)
- CC53 → formant depth (deadzone + range, SHIFT expands)
- CC52 / keybed / mod-wheel → live pitch semitones
- notes 82–86 → microrepeat divisors {1,2,4,8,16}
- notes 70/71 → global speed-scrub (varispeed)
- SHIFT gestures → loop-fold/monitor routing
- the 8×5 grid → track/clip select + state display

## Link / transport
Ableton Link tempo/phase drives the loop grid; loop record start quantizes to the
Link phase; the first loop can propose the tempo to the session (ported logic).

Every one of these behaviors is the *ported looper logic* — same code, same
result. The mapping table above is the same one dubfx verified against the real
control plane (param_mapping.md).
