# Feasibility: looper → Alpine Linux with LV2 effects, multi-core, and Ableton-Link-over-WiFi AP

Investigation of four requirements for migrating the bare-metal (Circle, Pi 4)
looper to Alpine Linux. Every claim is grounded in looper's actual source
(cited `file:line`) or a witnessed computation/toolchain check — not estimates.

## TL;DR verdicts

| # | Requirement | Verdict | Key condition |
|---|-------------|---------|---------------|
| **1** | Ableton Link over WiFi without glitching audio | ✅ **Feasible — and *improved* vs today** | Isolate the audio core from WiFi IRQs (achievable on Pi 4) |
| **2** | Home FX as LV2 on one core + user-swappable FX as LV2 on a free core | ✅ **Feasible** | In-process host (not a JACK/PipeWire graph); Core 3 is already free |
| **3** | No added latency vs bare-metal | ✅ **Achievable, with a topology rule** | Keep the chain in-process; the multi-core split is optional, and must avoid a graph host |
| **4** | Pi as its own AP for Link when no external AP | ✅ **Feasible, low-risk** | AP/STA *mode-switching* (autoAP), which looper already does |

**The single most important finding:** looper's *current* architecture already
does ~80% of what's asked (Core 3 is free; Link is core-isolated behind a
lock-free snapshot; WiFi already does AP/STA role-negotiation). The Alpine
migration mostly **replaces hand-rolled bare-metal subsystems with tested kernel
ones** — and in doing so *fixes* the existing once-a-second Link glitch rather
than inheriting it.

---

## The looper baseline (witnessed, `../looper`)

- **Pi 4, bare-metal Circle**, 48 kHz, 64-sample block → **1.333 ms/block budget**
  (`AudioTypes.h:12-14`; `audio.cpp:214` `USB_FREERUN_TICKS = CLOCKHZ*64/48000`).
- **4-core partition** (`multicore.cpp:10-34`): Core 0 = USB ISR + dispatch,
  **Core 1 = audio DSP**, **Core 2 = control / network / Link / WiFi**,
  **Core 3 = free / idle-forever**.
- **Inter-core = lock-free**: SPSC ring for audio dispatch (`coreDispatch.cpp`),
  double-buffered atomic `LiveParams` snapshot for control→DSP (`paramSnapshot.h`).
  **No mutexes in the audio path.**
- **Ableton Link = a custom raw-Ethernet reimplementation** (not the official
  lib — bare metal has no IP stack): `linkWire.h`/`linkGhost.h`/`linkSession.h` +
  `abletonLink.cpp` (938 lines) hand-building Ethernet/IP/UDP frames, multicast
  `224.76.78.75:20808`. Runs **entirely on Core 2**; hands phase to audio via the
  snapshot.
- **WiFi already does AP + STA** with automatic role negotiation
  (`JoinOpenNet` → fallback `CreateOpenNet`, `kernel.cpp:131-169`), plus
  hand-rolled DHCP client/server, ARP, IGMP — but it is **opt-in
  (`LOOPER_ENABLE_WLAN`) and unvalidated under audio load** (`kernel.cpp:268`).
- **USB = a UAC2 *gadget*** (`dwusbgadget.cpp`, `usbaudiogadget*`): the Pi
  presents itself *as* an audio device to a host.

---

## Requirement 1 — Link over WiFi without audio glitch ✅ (improved)

**Why it's decoupled:** Link carries *timing*, not audio. Its network thread
runs on Core 2 and hands tempo/phase to the audio thread through the lock-free
`LiveParams` snapshot; the audio callback only reads the snapshot and does cheap
integer phase math (`loopMachine.cpp:620, 628-696`). So **WiFi jitter degrades
Link *sync accuracy*, never the audio callback deadline.** On Linux the official
Ableton Link library is explicitly RT-safe (`captureAudioSessionState`/commit
pair) and maps directly onto this same snapshot pattern.

**The one real glitch — and why migration fixes it:** looper *does* have a
known "once-a-second audio glitch," root-caused (`abletonLink.cpp:725-736`) to
the **1 Hz proactive ALIVE beacon `SendFrame`** on the single shared bare-metal
WiFi radio. It runs on Core 2 but glitches Core-1 audio through **hardware
contention** (shared SDIO/DMA bus / IRQ stall) — the author added a runtime
`LTX0/LTX1` toggle specifically to A/B-confirm this. On Linux the WiFi TX goes
through the kernel `mac80211`/`brcmfmac` driver with **threaded IRQs on an
isolated core**, decoupling WiFi TX from the audio DMA path. **The bare-metal
contention glitch is a migration *fix*, not something inherited.**

**Condition:** `isolcpus` + `/proc/irq/*/smp_affinity` must steer WiFi/network
IRQs off the audio core. This is **achievable on Pi 4** (see the Pi-5 caveat
below).

---

## Requirement 2 — home FX + user FX as LV2s on separate cores ✅

- **Home FX as LV2:** *witnessed* — `faust -a lv2.cpp dsp/chain.dsp` generated a
  complete 69 KB LV2 architecture (7 `LV2_Descriptor`/`connect_port` refs), and
  `faust2lv2` ships in the native Faust install. The verified dubfx chain
  (pitch + delay + reverb + microrepeat + filters, pitch engine linked via
  `ffunction`) packages as one LV2 bundle.
- **User-swappable FX:** a directory on flash the user drops an LV2 bundle
  (`.so` + `.ttl`) into; the host scans and `dlopen`s it. **This is the whole
  point of the migration** — a filesystem and a dynamic linker, both *absent*
  on bare-metal Circle. Users can author their own with `faust2lv2` or any LV2
  toolchain.
- **Core placement:** slots onto looper's existing map — Core 1 = home FX,
  **Core 3 (already free) = user FX**, Core 2 = control/Link. Linux
  `pthread_setaffinity_np` + `SCHED_FIFO` replaces Circle's `CMultiCoreSupport`.
- **Crash isolation tradeoff:** a user plugin is untrusted. Full process
  isolation (PipeWire nodes as separate processes) survives a user-plugin crash
  but **adds a graph period of latency**. For zero latency, host in-process with
  a `SIGSEGV` handler + watchdog that disables a misbehaving plugin and continues
  (degraded, not dead). This is a user-facing tradeoff to choose.

---

## Requirement 3 — no added latency ⚠️→✅ (with a topology rule)

This is the one with real physics. **Witnessed arithmetic** (1.333 ms budget,
illustrative FX costs home 0.5 ms / user 0.4 ms, RT cross-core wake ~5 µs):

| Topology | Parallel across cores? | Added latency | Fits budget? |
|----------|:----:|:----:|:----:|
| **A. Serial chain, in-process, one core** | ❌ | **0 ms** | ✅ (0.9 ms cpu) |
| **B. Serial chain, pipelined across 2 cores** | ✅ | **+1 block (1.333 ms)** | ✅ |
| **C. Parallel FX, fork-join across 2 cores** | ✅ | **0 ms** | ✅ (0.51 ms wall) |
| **D. JACK/PipeWire graph, 2 clients** | ✅ | **+1 period** | ✅ |

**The hard truth:** you cannot have *serial chain* + *cross-core parallel* +
*zero latency* at once — pick two. A serial chain across two cores is either
same-block-serial (no parallelism, no added latency) or pipelined (parallel,
+1 block).

**The resolution:** the home + user FX combined (~0.9 ms) **fits one core inside
the 1.333 ms budget**, so run them serial *in-process on the free Core 3* →
**zero added latency, no parallelism needed.** Cross-core fork-join (topology C,
0.51 ms wall, zero added latency) is available for *parallel* effect topologies
(sends/sum) or if the chain ever becomes CPU-bound. **Never use a JACK/PipeWire
graph (topology D) — it adds a full period.** So *zero added latency is
achievable*; multi-core splitting is an optimization, not a requirement, and the
implementation must stay in-process.

---

## Requirement 4 — Pi as AP fallback for Link ✅ (low-risk)

The requirement — "AP *when* external AP unavailable" — is **mode-switching
(AP-or-STA)**, *not* simultaneous AP+STA. That distinction is everything:
simultaneous single-radio AP+STA is flaky on the Pi (`nl80211: Could not
configure driver mode`), but mode-switching is a solved pattern
([autoAP](https://github.com/gitbls/autoAP)). **looper already implements the
switch logic** (join first → fall back to hosting AP → yield if another AP
appears → 40 s safety net, `kernel.cpp:85-169`). On Alpine this becomes
`wpa_supplicant` (STA) + `hostapd` + `dnsmasq` (AP) with a state-driven switch
under OpenRC.

- **Link over the Pi's own AP works** — looper already runs Link over its
  self-hosted AP (DHCP server `192.168.4.1`, hand-rolled ARP responder so AP
  clients resolve the Pi, `abletonLink.cpp:667-694`). On Linux, ensure the AP
  passes multicast between clients (`ap_isolate=0`) so peer devices see Link.
- **AP IRQ load** (~5–15 % CPU for hostapd/beacon/multicast) steers onto the
  control core on Pi 4, keeping the audio core clean.
- **If simultaneous AP+STA is ever actually needed** (Pi on home WiFi *and*
  hosting a Link AP at once): add a USB WiFi dongle for a clean second radio.
  Not needed for the stated requirement.

---

## The Pi-4 vs Pi-5 caveat (a real risk gate)

The audio-core-isolation story depends on being able to steer network/WiFi IRQs
off the audio core. **This works on Pi 4** (the target). On **Pi 5**, the RP1
I/O controller has `PF_NO_SETAFFINITY` on force-threaded IRQ handlers
([raspberrypi/linux#7301](https://github.com/raspberrypi/linux/issues/7301)) —
you *cannot* always move those IRQs, which would put requirement 1's no-glitch
guarantee at risk. **Verdict holds for Pi 4; re-open this if the project moves
to Pi 5.**

---

## Migration architecture (recommended)

```
Alpine Linux (diskless / RAM, OpenRC), PREEMPT_RT kernel
├── Core 0: USB audio I/O — kernel f_uac2 configfs gadget (replaces hand-rolled UAC2)
├── Core 1: home-FX LV2  (dubfx Faust chain via faust2lv2, pitch ffunction linked)   ┐ in-process
├── Core 3: user-FX LV2  (dlopen'd from /effects on flash, watchdog-isolated)         ┘ host, no graph
├── Core 2: control — official Ableton Link lib, wpa_supplicant/hostapd (autoAP),
│           MIDI (ALSA rawmidi), telemetry sockets, isolcpus target for network IRQs
└── DSP core (loopMachine/effects/pitch/sampler): ported unchanged (Circle-free, alloc-free)
```

### Effort, by subsystem (from the Circle-dependency map)

| Subsystem | Move | Effort |
|-----------|------|--------|
| DSP core (loopMachine, effects, pitch, sampler) | port unchanged (Circle-free, alloc-free) | **low** |
| Ableton Link | delete raw-Ethernet clone → official Link lib over UDP | **low–medium** |
| WiFi + AP/STA + DHCP/ARP | delete hand-rolled stack → wpa_supplicant/hostapd/dnsmasq/autoAP | **low–medium** |
| Ethernet / sockets / timers / scheduler / SD / LED / serial | trivial Linux equivalents | **low** |
| Multicore IPC (SEV/WFE + rings) | pthreads + affinity + SCHED_FIFO + futex/eventfd | **medium** |
| PREEMPT_RT kernel build + RT tuning | RT-patched kernel, isolcpus, IRQ affinity, mlockall | **medium–high** (biggest single task) |
| **USB UAC2 gadget** (`dwusbgadget`) | → configfs `f_uac2` via dwc2 | **high** (hardest port; but *deletes* looper's buggiest code) |
| LV2 hosting + user-swap | in-process host + lilv/dlopen + flash dir | **medium** (new capability) |

### The two-sided ledger

- **What migration *deletes/evicts*:** the hand-rolled UAC2 decode (source of
  every buzz/crackle lesson), the entire raw WLAN/DHCP/ARP/IGMP/Link-wire stack,
  and the effects-maintenance burden (already reproduced as Faust LV2). All
  become tested kernel/userspace components.
- **What migration *costs*:** a PREEMPT_RT kernel + RT tuning, the `f_uac2`
  gadget port, unwinding the multicore SEV/WFE IPC to pthreads, musl
  verification, and accepting a slightly worse *worst-case* jitter than
  bare-metal (mitigated to tens of µs by RT + isolation — inaudible if tuned).

**Net:** all four requirements are feasible on Pi 4; requirement 1 is *improved*;
requirement 3 is achievable with the in-process (no-graph) rule; the tall poles
are the RT kernel and the `f_uac2` gadget, both of which retire more maintenance
than they add.
