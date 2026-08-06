# Current state and how work is tracked

The original Circle→Linux migration this document used to plan (scaffold →
publish → DSP core port → LV2 host → Link → control → WiFi/AP → USB gadget →
image → RT kernel → CI → on-hardware verify) is **done**. That plan drained in
full, on real Pi 4 hardware, and the project has been in active feature
iteration ever since — this document now describes what actually shipped and
how ongoing work is tracked, rather than a pre-build task list.

## What shipped (the original migration, steps 1–11 + on-hardware verify)

- **DSP core**: the loop engine (`dsp/loop.dsp`) and the [dubfx](../dubfx)-derived
  effects chain, composed into one home-FX Faust program (`dsp/aloop.dsp`),
  compiled straight into the `aloop` binary — no Circle source anywhere in this
  tree.
- **LV2 host**: an in-process host (`src/host/lv2_host.cpp`) that `dlopen`s LV2
  bundles and calls `run()` inside the audio callback directly — no JACK/PipeWire
  graph, zero added latency. The home stack runs compiled-in on Core 1; a
  user-supplied `.lv2` bundle runs on Core 3.
- **Ableton Link**: the official Link library integrated via a lock-free
  double-buffered snapshot (`LinkSnapshot`) so the audio thread never touches a
  lock.
- **Control / MIDI**: the full control-surface handling now living in
  `src/control/apc_grid.cpp` — see [`CONTROLS.md`](CONTROLS.md) for the current
  surface, which has grown well past the original rec/play/vol mapping.
- **WiFi / AP (autoAP)**: `hostapd`/`wpa_supplicant`/`dnsmasq`, with MAC-ordered
  host election so aloop and the paired `esp-idf-link` ("ticker") ESP32 project
  form one ad-hoc mesh for Link discovery with no manual pairing.
- **USB gadget (f_uac2)**: a real UAC2 gadget on Pi 4/CM4/Zero2 boards, opened
  NONBLOCK as a best-effort mirror of the real instrument-device audio path.
- **Alpine image + RT kernel + CI**: diskless/RAM Alpine image, PREEMPT_RT with
  `isolcpus`/IRQ-affinity/`SCHED_FIFO`/`mlockall`, built and cross-compiled by
  GitHub Actions for real musl/aarch64 targets.
- **On-hardware verification**: all of the above has run and been debugged live
  on a real Pi 4 (`192.168.137.100`) — RT jitter, USB audio round-trip, Link
  sync, and AP meshing are not theoretical, they are the subject of most of
  `AGENTS.md`'s entries.

## What shipped after the migration (the instrument layer)

The home Faust stack kept growing well past "loop + effects" once the migration
landed. In roughly chronological order (see `git log` for exact commits):

- 20 independent loopers with a real ARM/FINISH/pause/resume press cycle,
  long-hold erase, and Link-quantized successive-recording alignment.
- SHIFT/monitor-fold gesture, native fold mechanism, and latency-compensated
  recording while SHIFT is held.
- A 6-voice polyphonic pitch-lock engine (`multitranspose.dsp`) with
  pitch-synchronous shift windows and round-robin voice stealing.
- Continuous USB-drive ring recording of the fully-effected mix.
- A 3-bank FX control surface (dub / guitar / lofi) with per-bank knob
  targets.
- **Objekt**: a 4-voice modal-resonator synth excited by the live input,
  reachable via a real-hold gesture on the LofiFx pad, replacing (never
  layering over) the dry/pitch-lock signal while engaged, with alias-guarded
  higher modes and click-free voice stealing.
- A 6-patch granulator (Glass/Cloud/Freeze/Chop/Tape/Shatter) blended as a
  convex morph, with velocity-sensitive grain density and a grain
  envelope-shape parameter.
- A 16-beat grid visualization on 4 dedicated pads, reading the shared Link
  phrase position.
- Ableton Link Test Plan compliance auditing (TEMPO/STARTSTOPSTATE/BEATTIME/
  AUDIOENGINE cases).

## Work in progress

- **Orange Pi Prime board port** (Allwinner H5): the shared boot-tree tooling
  (`image/lib-boot-tree.sh`) already supports Pi 3/4/5 and Orange Pi Prime's
  apkovl/userspace layer, but the board's Armbian-sourced boot chain
  (BootROM → SPL/U-Boot → boot.scr → kernel) is still being brought up on real
  hardware. This is genuinely open, real diagnostic work, not a placeholder —
  see the `opi-*` rows in `.gm/prd.yml` for the current live diagnosis state
  (most recently: a kernel-compile failure in vendored, unrelated Realtek WiFi
  drivers was root-caused and fixed; boot-chain silence after the U-Boot→kernel
  handoff is still being isolated).
- Whatever else is currently pending in `.gm/prd.yml` — that file, not this
  document, is the live source of truth for open work. This document is updated
  periodically to summarize state, not to track it in real time.

## How work is tracked now

There is no fixed drain-order DAG anymore — the migration that DAG described is
complete. Ongoing work (bug fixes, new DSP features, board ports, doc updates)
is tracked as rows in `.gm/prd.yml`, added as discovered and resolved with real
on-device or DawDreamer-JIT witness evidence, per the discipline in
[`DECISIONS.md`](DECISIONS.md) and `AGENTS.md`. Anything genuinely blocked on
real Pi 4/board hardware is marked as such rather than claimed done from a dev
host — that honesty rule from the original plan still applies.
