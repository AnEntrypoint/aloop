# aloop — the looper, reborn on Alpine Linux

**aloop** is the forward-thinking successor to the bare-metal ([Circle](https://github.com/rsta2/circle))
Raspberry Pi looper. It moves the loop engine and effects onto **Alpine Linux +
PREEMPT_RT** so that the effects become **hot-swappable LV2 plugins**, Ableton
Link runs over a **tested kernel WiFi stack** (with the Pi acting as its own
access point when no network is around), and the buggiest hand-rolled
subsystems are replaced by battle-tested kernel/userspace components — **without
adding audio latency**.

This is not a rewrite for its own sake. Every architectural decision here is
grounded in a **witnessed feasibility study** ([`docs/FEASIBILITY.md`](docs/FEASIBILITY.md))
that read the real bare-metal looper source and measured the tradeoffs. This
README is the front door; the *why* behind everything lives in [`docs/`](docs/).

---

## The idea in one picture

```
Raspberry Pi 4  ·  Alpine Linux (diskless/RAM)  ·  PREEMPT_RT kernel
┌─────────────────────────────────────────────────────────────────────────┐
│ Core 0  USB audio I/O   — kernel f_uac2 gadget (Pi is a USB audio device) │
│ Core 1  home-FX LV2     — the verified Faust chain (pitch/delay/reverb/…) │─┐ in-process
│ Core 3  user-FX LV2     — drop your own .lv2 on flash, hot-swapped        │─┘ host, NO graph
│ Core 2  control         — Ableton Link · WiFi/AP (autoAP) · MIDI · telem  │   (zero added latency)
└─────────────────────────────────────────────────────────────────────────┘
   Loop engine + DSP: ported unchanged from ../looper (Circle-free, alloc-free)
```

## What this buys you (the four goals, all feasible on Pi 4)

| Goal | How aloop does it | Why it works |
|------|-------------------|--------------|
| **Ableton Link over WiFi, no audio glitch** | Official Link lib on the control core; audio reads a lock-free snapshot | Link carries *timing*, not audio — and the kernel WiFi stack fixes the bare-metal once-a-second glitch |
| **Home FX + your own FX, each on its own core** | Two LV2 plugins, in-process host, pinned to Core 1 and the (previously idle) Core 3 | Multi-core without a JACK/PipeWire graph |
| **No added latency vs bare-metal** | In-process LV2 hosting; the chain fits one core inside the 1.333 ms block | A graph host would add a full period — so aloop never uses one |
| **Pi is its own Link AP when offline** | autoAP mode-switching (STA when a network exists, AP otherwise) | The bare-metal looper already did this; Linux does it with `hostapd`/`wpa_supplicant` |

## Hot-swappable effects — the moddability story

The effects are **LV2 plugins on flash**, not compiled into the firmware:

- The **home effects** are the verified [dubfx Faust chain](docs/FEASIBILITY.md)
  packaged with `faust2lv2` — pitch shifter, tape delay, reverb, beat-repeat,
  and SVF filters.
- **You** drop your own `.lv2` bundle into `/effects/user/` on the SD card and it
  loads on the free core — write it in [Faust](https://faust.grame.fr), or use
  any of the thousands of existing LV2 plugins. See [`effects/README.md`](effects/README.md).

This is only possible because Linux gives us a filesystem and a dynamic linker —
neither of which existed on bare metal. That single capability is the reason for
the whole migration.

---

## Repository layout

| Directory | What's in it |
|-----------|--------------|
| [`docs/`](docs/) | The design record — architecture, the migration map, the decision log, and the feasibility study that justifies it all |
| [`src/`](src/) | The ported loop engine + the in-process LV2 host, Link integration, WiFi/AP control, MIDI |
| [`effects/`](effects/) | The home-FX LV2 bundle + the user drop-in directory |
| [`image/`](image/) | The Alpine diskless image build |
| [`kernel/`](kernel/) | PREEMPT_RT kernel config + RT tuning |
| [`ci/`](ci/) + [`.github/workflows/`](.github/workflows/) | GitHub Actions that build the LV2 bundles, cross-compile the binary, and assemble the image |

## Status

This repo is **planned in full before it is built** — the entire migration is
enumerated as a task plan (`.gm/prd.yml`) and drained in dependency order. See
[`docs/PLAN.md`](docs/PLAN.md) for the drain order and current state. Rows that
need real Pi 4 hardware to *measure* (RT jitter, USB latency, on-air Link) are
marked as such — everything buildable (code, config, CI, docs) is done first.

## Provenance

- **`../looper`** — the bare-metal Circle looper this migrates from (the DSP/loop
  engine ports from here unchanged).
- **`../dubfx`** — the Faust reproduction of looper's effects, A/B-verified
  sample-for-sample. Its verified chain becomes aloop's home-FX LV2, and its A/B
  harness is aloop's regression oracle.
- **`docs/FEASIBILITY.md`** — the witnessed study that grounds every decision.
