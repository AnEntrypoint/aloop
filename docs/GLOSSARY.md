# Glossary

Domain terms used throughout aloop, so the docs are self-contained.

- **Ableton Link** — a protocol/library that keeps the tempo, beat, and bar
  phase of musical apps and devices synchronized over a local network (UDP
  multicast). It syncs *timing*, not audio samples.
- **aloop** — this project: the looper migrated to Alpine Linux.
- **ALSA** — Advanced Linux Sound Architecture; the Linux kernel audio API. aloop
  reads/writes audio through an ALSA PCM.
- **autoAP** — a mode-switching pattern: a device runs as a WiFi *station* when a
  known network is available and hosts its own *access point* when none is,
  switching automatically.
- **block / period** — the batch of audio samples processed at once. aloop uses
  64 samples at 48 kHz = a **1.333 ms** block; all per-block work must finish
  inside that budget.
- **Circle** — the bare-metal C++ framework the original looper runs on (no OS).
- **configfs** — a Linux kernel filesystem for configuring kernel objects at
  runtime; used to set up the USB gadget.
- **dwc2** — the USB controller on the Pi that supports device/gadget mode.
- **f_uac2 / UAC2 gadget** — the Linux function that makes the Pi present itself
  *as* a USB Audio Class 2 device (a soundcard) to a host computer.
- **Faust** — a functional DSP language that compiles to C++/LV2/etc. aloop's home
  effects are written in Faust.
- **ffunction** — a Faust mechanism to call an external C function; used to link
  the exact C++ pitch engine into the Faust chain.
- **fork-join** — running two tasks on two cores and waiting for both to finish
  within the same block; how aloop can use two cores for *parallel* effects with
  zero added latency.
- **in-process host** — running plugins by calling their code directly inside
  aloop's own audio callback (as opposed to a separate graph-server process).
- **isolcpus** — a Linux boot parameter that removes CPUs from the general
  scheduler so only pinned threads run on them (used for the audio cores).
- **LV2** — an open standard plugin format (a `.so` + a `.ttl` metadata file).
  aloop's effects are LV2 plugins.
- **lilv** — the reference library for discovering and loading LV2 plugins.
- **mlockall** — a syscall that locks a process's memory into RAM so it never
  page-faults (page faults would blow the audio deadline).
- **PREEMPT_RT** — the Linux real-time preemption patch/config that bounds
  worst-case scheduling latency to the tens of microseconds.
- **RP1** — the Pi 5 I/O controller; its IRQ handling breaks the audio-core
  isolation aloop relies on, which is why aloop targets Pi 4 (ADR-006).
- **SCHED_FIFO** — a Linux real-time scheduling policy; the audio threads run
  under it at high priority.
- **snapshot (double-buffered atomic)** — the lock-free handoff of control
  parameters (Link tempo, MIDI values) from the control core to the audio core:
  a single writer flips between two buffers, the reader always sees a complete
  one. No locks, no tearing.
- **SPSC ring** — single-producer/single-consumer lock-free queue; how audio work
  is dispatched between cores.
- **STA / AP** — WiFi *station* (client, joins a network) vs *access point*
  (hosts a network). autoAP switches between them.
- **UAC2** — USB Audio Class 2, the USB spec for audio devices.
- **xrun** — an audio buffer under/overrun: the audio thread missed its deadline
  and produced a glitch. The enemy. RT tuning exists to prevent xruns.
