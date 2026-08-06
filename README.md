# MC-420 — a right-to-repair live workstation for punch-in musicians

**aloop** is the codebase behind the **MC-420**: a world-first right-to-repair
live-performance workstation built for punch-in live musicians — looping,
polyphonic pitch-lock, a granular/modal-resonator synth engine, and hot-swappable
effects, all on a Raspberry Pi 4 running **pure Linux** (Alpine + PREEMPT_RT).

Right-to-repair is not a marketing label here — it is the actual architecture:

- **The DSP is Faust source, not a firmware blob.** Every algorithm — the loop
  engine (`dsp/loop.dsp`), the effects chain, the polyphonic pitch-lock
  (`multitranspose.dsp`), the Resonode modal-resonator synth
  (`resonode_synth.dsp`), the granulator patches — is readable, editable `.dsp`
  text a repair tech or a curious musician can open, change, and recompile.
  There is no sealed DSP core.
- **The control surface is a plain-text mapping, not a hardcoded binding.**
  Every pad, knob, and CC on the controller maps through a name-keyed control
  store a technician can re-map with a text editor, no recompile, no vendor
  tool.
- **The effects chain is hot-swappable LV2, not a locked plugin slot.** Drop
  your own `.lv2` bundle onto the flash storage and it loads on the free core —
  write it in Faust or use any existing LV2 plugin.
- **The whole runtime is standard Linux**, debuggable with the same tools that
  work on any Linux box — SSH, `/proc`, ALSA, standard cross-compilers — instead
  of a proprietary bootloader and an undocumented DSP ISA.

This is not a rewrite for its own sake. The original architectural decision to
leave the bare-metal ([Circle](https://github.com/rsta2/circle)) looper for Linux
is grounded in a **witnessed feasibility study**
([`docs/FEASIBILITY.md`](docs/FEASIBILITY.md)) that read the real bare-metal
looper source and measured the tradeoffs. This README is the front door; the
*why* behind everything lives in [`docs/`](docs/), and the accumulated
hardware/DSP/build constraints from real device testing live in
[`AGENTS.md`](AGENTS.md).

---

## The idea in one picture

```
Raspberry Pi 4  ·  Alpine Linux (diskless/RAM)  ·  PREEMPT_RT kernel
┌─────────────────────────────────────────────────────────────────────────┐
│ Core 0  USB audio I/O   — kernel f_uac2 gadget (Pi is a USB audio device) │
│ Core 1  home stack      — loop/pitch-lock/Resonode/granulator/FX, compiled  │  compiled into
│                           straight into the binary, no dynamic loading   │  the aloop binary
│ Core 3  user-FX LV2     — drop your own .lv2 on flash, hot-swapped        │─┐ in-process
│ Core 2  control         — Ableton Link · WiFi/AP (autoAP) · MIDI · telem  │─┘ host, NO graph
└─────────────────────────────────────────────────────────────────────────┘   (zero added latency)
   Home stack = ONE Faust program: dsp/aloop.dsp (loop.dsp : effects_runtime.dsp), no Circle source
```

## What this buys you (the four goals, all feasible on Pi 4)

| Goal | How aloop does it | Why it works |
|------|-------------------|--------------|
| **Ableton Link over WiFi, no audio glitch** | Official Link lib on the control core; audio reads a lock-free snapshot | Link carries *timing*, not audio — and the kernel WiFi stack fixes the bare-metal once-a-second glitch |
| **Home FX + your own FX, each on its own core** | Two LV2 plugins, in-process host, pinned to Core 1 and the (previously idle) Core 3 | Multi-core without a JACK/PipeWire graph |
| **No added latency vs bare-metal** | In-process LV2 hosting; the chain fits one core inside the 1.333 ms block | A graph host would add a full period — so aloop never uses one |
| **Pi is its own Link AP when offline** | autoAP mode-switching (STA when a network exists, AP otherwise) | The bare-metal looper already did this; Linux does it with `hostapd`/`wpa_supplicant` |

## The instrument, not just the looper

The home Faust stack (Core 1) has grown well past "loop + effects" into a real
punch-in-live performance instrument:

- **20 independent loopers** with an ARM → FINISH → pause/resume press cycle per
  pad (not simple record/play), long-hold erase, and Link-quantized successive
  recordings that snap to a musical power-of-2 subdivision of the established
  phrase length.
- **6-voice polyphonic pitch-lock** (`multitranspose.dsp`) — a Whammy/Manipulator-
  style harmonizer that locks the input to held keys, with round-robin voice
  stealing and pitch-synchronous shift windows.
- **Resonode** — a 4-voice, 6-mode-per-voice modal-resonator synth, excited by
  the live input signal rather than a synthetic oscillator, engaged by holding
  the LofiFx pad past a 1-second threshold (a quick tap instead latches a
  background granulator texture). Both live in the always-on home stack, never
  gated behind an LV2 host round-trip.
- **A 6-patch granulator** (Glass/Cloud/Freeze/Chop/Tape/Shatter) blended as a
  continuous convex morph across 6 knob slots, with real velocity-sensitive
  grain density.
- **A 16-beat grid visualization** on 4 dedicated pads showing the shared Link
  phrase position live.
- **Continuous USB-drive ring recording** of the fully-effected mix, and an
  Ableton Link mesh (the `ticker` AP) that lets multiple aloop/ESP32 devices
  share tempo and transport with no manual pairing.

See [`docs/CONTROLS.md`](docs/CONTROLS.md) for the full control-surface
reference and [`AGENTS.md`](AGENTS.md) for the technical constraints behind each
of these.

## Hot-swappable effects — the moddability story

The user-extension slot is a **plain LV2 plugin directory on flash**, not
compiled into the firmware:

- The **home stack** (loop engine, pitch-lock, Resonode, granulator, the verified
  [dubfx](../dubfx)-derived effects chain) is Faust source compiled straight
  into the `aloop` binary and runs on Core 1 every block.
- **You** drop your own `.lv2` bundle into `/effects/user/` on the SD card and it
  loads on the free core (Core 3) — write it in [Faust](https://faust.grame.fr),
  or use any of the thousands of existing LV2 plugins. See
  [`effects/README.md`](effects/README.md).

This is only possible because Linux gives us a filesystem and a dynamic linker —
neither of which existed on bare metal. That single capability is the reason for
the whole migration, and it is the mechanical basis of the right-to-repair claim:
nothing about extending or fixing this instrument requires reflashing a sealed
firmware image.

---

## Repository layout

| Directory | What's in it |
|-----------|--------------|
| [`docs/`](docs/) | The design record — architecture, the migration map, the decision log, the control-surface reference, and the feasibility study that justifies it all |
| [`AGENTS.md`](AGENTS.md) | The living technical-constraints reference — hardware quirks, DSP gotchas, deploy/netboot mechanics, all discovered through real-device testing |
| [`src/`](src/) | The audio thread, in-process LV2 host, Link integration, WiFi/AP control, MIDI/control-surface handling |
| [`dsp/`](dsp) + [`effects/`](effects/) | The Faust source for the home stack (loop engine, pitch-lock, Resonode, granulator, effects) and the user-drop-in LV2 directory |
| [`image/`](image/) | The Alpine diskless image build, multi-board (Pi 3/4/5, Orange Pi Prime) |
| [`kernel/`](kernel/) | PREEMPT_RT kernel config + RT tuning |
| [`ci/`](ci/) + [`.github/workflows/`](.github/workflows/) | GitHub Actions that build the LV2 bundles, cross-compile the binary, and assemble each board's image |

## Status

The original Circle→Linux migration (steps 1–11 of the original build plan) is
long done and running on real Pi 4 hardware — the codebase has moved well past
"migrated" into active feature iteration: the pitch-lock, Resonode synth,
granulator, grid-LED visualization, and USB ring-recording work above all
shipped *after* the migration, each as its own real-hardware-tested change (see
`git log` for the full history, and [`docs/PLAN.md`](docs/PLAN.md) for the
current state summary). Work continues as a live, PRD-driven queue
(`.gm/prd.yml`) rather than a fixed pre-planned list; the queue currently also
carries an in-progress hardware bring-up effort for a second board target
(Orange Pi Prime, Allwinner H5) alongside the Pi 4/pi3/pi5 boards already
supported by the shared boot-tree tooling — see `AGENTS.md`'s Boards section for
the real per-board capability matrix and the Orange Pi Prime's current
boot-blocker status.

## Provenance

- **`../looper`** — the bare-metal Circle looper this migrates from (the DSP/loop
  engine ports from here unchanged).
- **`../dubfx`** — the Faust reproduction of looper's effects, A/B-verified
  sample-for-sample. Its verified chain becomes aloop's home-FX LV2, and its A/B
  harness is aloop's regression oracle.
- **`docs/FEASIBILITY.md`** — the witnessed study that grounds every decision.
