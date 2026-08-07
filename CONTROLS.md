# Controls — the real, current control surface

The control surface is implemented in `src/control/apc_grid.cpp` and driven by
an APC Key25-class MIDI grid controller. Every binding still goes through the
same name-keyed `ParamStore`/Faust-zone mechanism the project started with —
re-mapping a CC or note is a config change, not a recompile — but the gestures
each pad/knob drives have grown well past plain rec/play/vol. This document is
the user-facing summary; `AGENTS.md`'s "Control Surface" section has the
byte-level detail and the bug history behind each rule below.

## Looper pads: a real press cycle, not a toggle

Each looper pad drives one of 20 independent loopers through a state cycle,
not a simple record/play flag:

```
empty → ARM (rec=1, held) → FINISH (rec=0, play=1) → pause (play=0) → resume (play=1) → ...
```

- **ARM and FINISH fire on press**, not release — both are instants that must
  land precisely.
- **Long-hold erases** a looper's content and, if it was the last looper still
  holding content, resets the shared master phrase length so the next
  recording re-establishes it from scratch.
- **CLEAR_ALL** zeroes every looper's `play` and `rec` in one gesture.
- Real APC Key25 hardware re-sends note-on for an already-held pad; a press
  guard treats the repeat as a no-op so it can't reset a hold timer or
  re-trigger ARM/FINISH mid-recording.
- **Successive recordings snap to a musical subdivision** of the first
  recording's real length — the master phrase length is read from live
  `writeIdx` telemetry (sample-accurate), never estimated from wall-clock time.
  Candidates are always powers of 2 relative to the established length, chosen
  by the log-space geometric midpoint between the two bracketing candidates —
  so any two loopers' lengths are always in a clean power-of-2 ratio and stay
  drift-free forever.

## Guitar-FX held: sidechain-source toggle

While the Guitar-FX pad is held, a looper-pad press is redirected entirely — it
toggles that looper as a sidechain-pump source instead of touching its
ARM/FINISH state. The designation auto-clears when that looper's content is
wiped.

## The LofiFx / granulator pad: two gestures on one button

This pad (note 69) always switches the active knob bank to LofiFx and flips
the granulator's latched state on press, but what happens as the press
continues depends on how long it's held:

- **The press itself**: flips a *latched* granulator state the instant the
  pad goes down — no need to wait for release. This turns the grain engine
  into a persistent, backgrounded texture layered under everything else; a
  second press flips it back off. The latch survives release regardless of
  how long the press turns out to last.
- **Real hold** (still held past 1 second): switches the instrument entirely
  to **Resonode**, a 4-voice, 6-mode-per-voice modal-resonator synth in the
  spirit of a classic hardware physical-modeling instrument. While engaged:
  - The keybed drives up to 4 Resonode voices (round-robin/oldest-steal
    allocation, same shape as the pitch-lock voice allocator below), each
    carrying its own real MIDI velocity into the exciter's gain — a harder
    key press rings out louder.
  - Knob slots 1–4 are the blend weight of one of 4 named material patches —
    **Percussive, Metal/Glass, Strings, Dance Bass** — each a full point
    in position/decay/damping/stretch/collision space, blended as a convex
    combination exactly like the granulator's own patch surface below. Knob
    slots 5–6 are direct performative dials: **tone** (exponential-taper
    brightness) and **level** (output loudness).
  - Resonode is excited by the *live input signal* ("reactor mode"), not a
    synthetic oscillator — it only sounds through a voice while that voice's
    key is held, and it fully **replaces** the dry/pitch-lock signal while
    engaged rather than layering under it.
  - A harder key press also bends the pitch briefly upward on attack before
    settling to the true note — more so on "flexible" (low-dispersion)
    patches, barely at all on "stiff" (high-dispersion) ones — and
    **collision** adds a bounded per-patch soft-clip "bounce" to each voice's
    own output (0 = untouched passthrough), both scaled per named patch
    rather than exposed as free-standing knobs.
  - Releasing the pad releases every held Resonode voice and reverts to the
    bank that was active before the press.

LED feedback on the pad itself: blinking red once Resonode is actually
engaged, blinking green during the pre-threshold granulator preview, solid
green while latched-on in the background, off otherwise.

### Granulator and Resonode: named patches, blended, never raw sliders

When Resonode is *not* engaged, knob slots 1–6 are the blend weight of one of
6 fixed named grain patches — **Glass, Cloud, Freeze, Chop, Tape, Shatter** —
each a full point in grain-size/density/pitch-spray/position-jitter/scan-rate/
reverse-probability/envelope-shape space representing a distinct musical
character. When Resonode *is* engaged, knob slots 1–4 blend the 4 named
material patches above instead. Both surfaces share the same convex-
combination discipline: turning up multiple patch dials together produces a
coherent blend, never independent raw parameters fighting each other, and all
weights at zero falls back to the first named patch (Glass for the granulator,
Percussive for Resonode) rather than dividing by zero. Real key velocity
scales overall voice loudness for both instruments, and (for granular voices
only) grain spawn density, so a harder key press plays louder — and for the
granulator, spawns a denser, brighter grain cloud.

## 6-voice polyphonic pitch-lock (SHIFT + keybed)

Holding SHIFT and pressing keys drives a 6-voice polyphonic pitch-lock engine
(`multitranspose.dsp`) — a Whammy/Manipulator-style harmonizer where the
output lands on the exact held key regardless of the input's actual pitch.

- Each held key claims one of 6 voice slots (reuse-own-slot / prefer-unheld /
  oldest-steal allocation).
- The transpose window is pitch-synchronous (sized from the detected input
  period, capped at 20ms), not a fixed window, so large shifts track
  correctly and rapid retriggers stay click-safe.
- SHIFT alone (no key held) is a separate gesture — the native fold mechanism
  that lets the previous block's loop content bleed into the live input.
- SHIFT + `fx/monitorfold`'s **free-transpose** mode redirects the whole
  tracking/shifting engine onto the loop content itself instead of the dry
  input — a live "pitch-shift the loop" gesture on the same voice engine.
- Locked pitch **replaces** the original signal — a dry/pitch-lock crossfade
  gate closes as voices gate, so this is a lock, never a harmonizer sitting on
  top of the unshifted signal.

## Grid-beat visualization

The 4 top-right otherwise-unassigned pads show the shared 16-beat Link phrase
position live as a cumulative bar-graph (blank → yellow → red → green per
beat index), read from `AudioThread::Telemetry::gridBeatIndex`.

## 3-bank FX knob surface

The knob row is one shared set of 6 CCs whose target table depends on which
bank is active — **Dub**, **Guitar**, or **LofiFx** (granulator/Resonode, above).
A bank-select press only flips which target table the next knob CC reaches
and starts a brief LED flash; it never re-pushes state to the DSP.

## Config-file remapping (unchanged mechanism)

Every control still ultimately binds through `/etc/aloop-controls.conf` (see
`config/controls.conf` for the format): a plain `<midi> <target>` line per
binding, parsed by the MIDI thread and written into the name-keyed
`ParamStore`, which the audio thread reads by name into the matching Faust
control zone. Nothing is hardcoded — the map is the single source of truth for
which physical control reaches which target, and re-mapping needs no
recompile. What has changed since this file's original scope is the *behavior*
layered on top of individual pads (the gestures documented above) — the
underlying binding mechanism is untouched.
