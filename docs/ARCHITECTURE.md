# aloop architecture

This document explains **how aloop is structured and why** — every design choice
here traces to a witnessed finding in [`FEASIBILITY.md`](FEASIBILITY.md). Read
that first if you want the raw evidence; this is the distilled architecture.

## Design principle: keep the good bare-metal ideas, delete the hand-rolled ones

The bare-metal looper got the *hard* things right — a lock-free 4-core audio
architecture with no mutexes in the audio path — and got the *tedious* things
wrong-then-painfully-fixed (a hand-rolled USB-audio decoder, a hand-rolled WiFi
+ DHCP + ARP + Ableton-Link wire stack). aloop **keeps the architecture and
deletes the reimplementations**, replacing each with the tested kernel/userspace
equivalent.

## The 4-core partition

The Pi 4 has four cores. aloop assigns them exactly as the bare-metal looper did
(`multicore.cpp:10-34`), because that partition already works and already keeps
the audio core clean:

| Core | Role | Bare-metal origin | aloop mechanism |
|------|------|-------------------|-----------------|
| **0** | USB audio I/O | USB completion ISR + dispatch | kernel `f_uac2` gadget IRQ + ALSA |
| **1** | **home-FX DSP** | audio DSP worker (`AudioSystem::doUpdate`) | RT thread, `SCHED_FIFO`, pinned |
| **3** | **user-FX DSP** | *idle / reserved* | RT thread, `SCHED_FIFO`, pinned — **now used** |
| **2** | control | net / MIDI / Link / WiFi | Ableton Link lib, `hostapd`/`wpa_supplicant`, ALSA MIDI, telemetry |

The single new thing: **Core 3, which sat idle on bare metal, now runs the
user's swappable effect.** That's where the "add your own effect on the free
core" goal lands.

### Why network/WiFi lives on Core 2, not the audio cores

Ableton Link and WiFi generate interrupts and CPU load. If they landed on the
audio core they would blow the 1.333 ms per-block deadline and cause audible
xruns. By pinning all network work to Core 2 and using `isolcpus` +
`/proc/irq/*/smp_affinity` to steer WiFi/network IRQs *off* the audio cores, the
audio cores meet their deadline regardless of network activity. (This is
achievable on Pi 4; see the Pi-5 caveat in `FEASIBILITY.md`.)

## The audio path: lock-free, allocation-free, ported unchanged

The DSP core — `loopMachine`, the effects, the pitch engine, the sampler — has
**no dependency on Circle** and does **no allocation, locking, or syscalls in the
audio callback**. That's why it ports to Linux unchanged: it drops onto a Linux
`SCHED_FIFO` audio thread exactly as it sat on a bare-metal core. The dubfx
project already proved the effects compile standalone and produce
sample-identical output.

Control parameters (Link tempo/phase, MIDI knob values) reach the audio thread
through the same **double-buffered atomic snapshot** the looper used
(`paramSnapshot.h`): a single writer on the control core fills an inactive buffer
and flips an index; the audio thread reads the active buffer. No locks, no
tearing, no priority inversion. On Linux the cross-core wakeup primitive changes
from bare-metal `SEV`/`WFE` to a `futex`/`eventfd`, but the lock-free discipline
is identical.

## Effects: two LV2 plugins, hosted *in-process*

This is the load-bearing decision, and it exists to satisfy two goals at once —
**hot-swappable effects** *and* **zero added latency**.

### Why LV2

LV2 gives us a standard plugin ABI (`instantiate` / `connect_port` / `run`) and a
metadata format (`.ttl`). That means:
- The home effects are a `.lv2` bundle built once with `faust2lv2`.
- A user drops their own `.lv2` bundle on flash and it loads — no recompile, no
  firmware flash. This is the moddability the whole project is for.

### Why *in-process*, and never a JACK/PipeWire graph

The obvious way to run two plugins on two cores is a graph host (JACK2/PipeWire
run each client on its own core). **aloop deliberately does not do this.** A
graph host schedules clients across period boundaries, which adds **one full
audio period (1.333 ms) of latency per hop**. That violates the no-added-latency
goal.

Instead, aloop `dlopen`s the LV2 bundles and calls their `run()` **directly
inside its own audio callback**. The witnessed latency arithmetic
(`FEASIBILITY.md`, Requirement 3) shows why this is enough:

| Topology | Parallel across cores? | Added latency |
|----------|:---:|:---:|
| Serial chain, in-process, one core | ✗ | **0 ms** |
| Parallel FX, fork-join across two cores | ✓ | **0 ms** |
| JACK/PipeWire graph | ✓ | **+1 period** |

The home + user chain (~0.9 ms of work) **fits inside one 1.333 ms block on a
single core**, so the default is to run it serial in-process — zero added
latency, no cross-core coordination needed. Cross-core fork-join (Core 1 + Core
3 joined within the same block, ~0.51 ms wall) is available for genuinely
*parallel* effects or if the chain ever becomes CPU-bound. **A graph host is
never used.**

### User-plugin crash isolation

A user's LV2 is untrusted code running in the audio process. In-process hosting
means a segfaulting plugin could take down audio — so the host installs a
`SIGSEGV` handler + watchdog that **disables a misbehaving plugin and continues
with the home chain**, degraded rather than dead. (True process isolation would
require a graph and its latency; that tradeoff is documented in `DECISIONS.md`.)

## Ableton Link: the official library, RT-safe by the same pattern

The bare-metal looper reimplemented the Link wire protocol from scratch because
it had no IP stack. Linux has one, so aloop uses the **official Ableton Link
library** over ordinary UDP multicast. Link's design already matches the
looper's: its network thread runs on the control core; the audio thread reads
the session state through the RT-safe `captureAudioSessionState` /
snapshot boundary. WiFi jitter therefore degrades Link *sync accuracy*, never the
audio deadline.

## WiFi: STA when there's a network, AP when there isn't (autoAP)

The Pi tries to join a known network (station mode); if none is available it
hosts its own access point so peer devices can still discover it for Link. This
is *mode-switching*, not simultaneous AP+STA (which is flaky on a single Pi
radio). `wpa_supplicant` + `hostapd` + `dnsmasq` under OpenRC, with a switch
service — the exact behavior the looper hand-rolled, now on the tested stack.
The AP passes multicast between clients (`ap_isolate=0`) so Link works over it.

## USB audio: the kernel `f_uac2` gadget

The Pi presents itself *as* a USB audio device (a UAC2 gadget) so a host computer
sees it as a soundcard. The bare-metal looper hand-wrote this, and every
buzz/crackle bug in its history came from that code. aloop uses the Linux
**`f_uac2` configfs gadget** on the `dwc2` controller — the kernel lays out the
isochronous USB packets correctly by construction, deleting that entire class of
bug. This is the hardest single port, but it removes the most maintenance.

## Boot to appliance

Alpine boots **diskless from RAM** with a read-only root and only the services
aloop needs (the audio binary, `hostapd`/`wpa_supplicant`, RT tuning). No
desktop, no background daemons stealing cycles — near-bare-metal determinism.
The full startup order is in [`BOOT.md`](BOOT.md).
