# Controls — fully remappable via a config file

Every control on aloop is bound in `/etc/aloop-controls.conf` (see
`config/controls.conf` for the default + the format). **You re-map any control by
editing that file — no recompile.** The MIDI thread loads it at startup and
applies incoming MIDI per the map.

## Format
```
<midi>  <target>
```
- `<midi>`: `cc<N>[.<ch>]` (a control-change) or `note<N>[.<ch>]` (a note; note-on
  = press). Optional MIDI channel after a dot.
- `<target>`: a control name —
  - `looper<i>/rec` `looper<i>/play` `looper<i>/vol` for looper `i` (0..19),
  - `fx/hp fx/lp fx/lpres fx/reverb fx/delay fx/time fx/formant fx/pitch` (effects),
  - `cmd/clearall cmd/halfspeed cmd/doublespeed` (global commands).

## How it flows
`/etc/aloop-controls.conf` → the MIDI thread parses each incoming CC/note, looks
it up in the map, and writes the target's value into a name-keyed control store →
the audio thread reads each target by name and sets the matching Faust control
zone (e.g. `looper03/rec`, `HPCUT`). Nothing is hardcoded; the map is the single
source of truth for the control surface.

## 20 independent loopers, record/play only
The engine has 20 independent loopers, each with `rec`, `play`, `len` (Link-synced
length), and `vol`. There is **no overdub** — record replaces the loop, play loops
it. Map the rec/play of each looper to whatever pads/CCs your controller has.
