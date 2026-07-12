# Command surface — the looper control behavior aloop clones

100% clone includes the control/command behavior, not just audio. aloop
reproduces looper's full command surface (the ported apcKey25*.cpp logic + loop
commands), with the input source moved to ALSA rawmidi (src/control/midi.cpp).

## Loop commands (ported from looper loopMachine command dispatch)
record · play · stop · clear/erase (per track) · halve/double speed (varispeed) · loop-immediate · set/clear loop-start — for each of the 20 independent loopers. NO overdub.
· set/clear mark point · pause (mute-with-advancing-head) · quantize-to-grid.

### Exact opcode → aloop control target (commonDefines.h → config/controls.conf)
The hardware speaks `LOOP_COMMAND_*` opcodes; aloop's control map speaks named
targets that resolve to Faust zones (src/dsp/audio_thread.cpp `targetToZone` +
the engine-global `clear`/`speed` handling). This is the authoritative parity map:

| looper opcode | value | aloop target | how it's realized |
|---|---|---|---|
| `LOOP_COMMAND_RECORD` (per track `TRACK_BASE 0x20`) | 0x80 / 0x20+i | `looper<i>/rec` | Faust `button("rec")` — record replaces the loop (NO overdub) |
| `LOOP_COMMAND_PLAY` | 0x81 | `looper<i>/play` | Faust `checkbox("play")` — gates the looper output |
| `LOOP_COMMAND_STOP` / `STOP_IMMEDIATE` | 0x03 / 0x02 | `cmd/stopall` | audio thread clears **every** `looper<i>/play` to 0 |
| `LOOP_COMMAND_STOP_TRACK_BASE` | 0x40+i | `looper<i>/play`=0 | per-track stop = clearing that one play checkbox |
| `LOOP_COMMAND_CLEAR_ALL` | 0x01 | `cmd/clearall` | engine-global Faust `button("clear")` — wipes all 20 loops |
| `LOOP_COMMAND_ERASE_TRACK_BASE` | 0x60+i | `looper<i>/erase` | per-looper Faust `button("erase")` — wipes that one loop |
| `LOOP_COMMAND_HALFSPEED_ON/OFF` | 0x0C/0x0D | `cmd/halfspeed` | engine-global `speed`=0.5 while held (varispeed: shorter fdelay ⇒ 0.5× read) |
| `LOOP_COMMAND_DOUBLESPEED_ON/OFF` | 0x0E/0x0F | `cmd/doublespeed` | engine-global `speed`=2.0 while held (2× read) |
| Link tempo/phase | — | `looper<i>/len` | audio thread sizes every loop from the Link BPM (varispeed grid sync) |

Momentary semantics match the hardware exactly: `HALFSPEED`/`DOUBLESPEED` and
`clear`/`erase` are **held** (value 1 = active, release = neutral), driven each
block from the atomic ParamStore the MIDI thread writes. `speed` composes with the
Link-set `len`: Link sets the base loop length, `speed` divides it (loop stays
grid-locked, plays at 0.5×/2×). The `TRACK_BASE 0x20` / `STOP_TRACK_BASE 0x40` /
`ERASE_TRACK_BASE 0x60` families are 20 contiguous per-track slots — the default
`config/controls.conf` binds all 20 of each; remap freely (no recompile).

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
