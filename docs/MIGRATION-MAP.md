# Migration map: Circle (bare metal) → Alpine Linux

Every bare-metal looper subsystem, what it becomes on aloop, and the effort/risk.
This is the traceability record — each Linux component points back at the
hand-rolled thing it replaces, so nothing is lost or silently dropped.

Classification: **delete** (kernel/userspace replaces it entirely) · **trivial**
(a one-liner Linux equivalent) · **port** (real code to move) · **new**
(capability that didn't exist on bare metal).

| Bare-metal (Circle) | aloop (Linux) | Class | Effort | Notes |
|---------------------|---------------|-------|--------|-------|
| Loop engine + effects (the whole audio DSP) | **reimplemented in Faust** (`dsp/loop.dsp` + the dubfx effect chain, composed into `dsp/aloop.dsp`) | **new (native)** | low–med | No Circle source; the effects are the dubfx A/B-verified chain, the loop engine is a native Faust looper (witnessed) |
| `CMultiCoreSupport` + SEV/WFE rings + `paramSnapshot` | pthreads + `sched_setaffinity` + `SCHED_FIFO`; `futex`/`eventfd` wakeups; the atomic snapshot unchanged | **port** | medium | Lock-free discipline transfers; only the wakeup primitive changes |
| Ableton Link raw-Ethernet clone (`abletonLink.cpp`, `linkWire/linkGhost/linkSession.h`, 938+ lines) | official Ableton Link library over UDP multicast | **delete** | low–med | Existed only for the missing IP stack; the lib is RT-safe and interoperable |
| WiFi: `CBcm4343Device` + `JoinOpenNet`/`CreateOpenNet` + `wlanDHCP`/`wlanDHCPServer` + hand-rolled ARP/IGMP | `brcmfmac` (mainline) + `wpa_supplicant` (STA) + `hostapd` (AP) + `dnsmasq` (DHCP) + autoAP switch | **delete** | low–med | The whole raw stack collapses into standard userspace |
| USB UAC2 gadget: `dwusbgadget.cpp`, `usbaudiogadget*` | `f_uac2` configfs gadget on dwc2 + ALSA PCM | **delete** | **high** | Hardest port; deletes the buzz/crackle bug class (kernel does microframes right) |
| `CNetSubSystem`, `CSocket`, `CIPAddress`, `CSysLogDaemon` | Linux kernel netstack + BSD `socket()`; `syslog(3)` | **trivial** | low | Ordinary sockets |
| USB-MIDI (APC Key25) via Circle USB | ALSA rawmidi / `snd-usb-audio`; the CC/note mapping logic (`apcKey25*.cpp`) ports unchanged | **port** | low | Only the input source changes |
| `CTimer::GetClockTicks()` @ 1 MHz | `clock_gettime(CLOCK_MONOTONIC)` | **trivial** | low | — |
| `CScheduler` cooperative yield | native threads / `sched_yield` | **trivial** | low | — |
| `CEMMCDevice` + FatFs | native VFS / `mount` | **trivial** | low | — |
| `CScreenDevice`, `CActLED`, `CSerialDevice` | framebuffer/DRM, sysfs LED, `/dev/ttyAMA0` | **trivial** | low | Rarely needed at runtime |
| Circle boot / kernel image | Alpine diskless/RAM image + PREEMPT_RT kernel | **new** | med–high | The RT kernel is the biggest single task |
| — (Core 3 idle) | user-swappable LV2 on Core 3 | **new** | medium | The moddability capability — impossible without a filesystem + linker |
| effects compiled into firmware | LV2 bundles on flash (home + user) | **new** | medium | The dubfx Faust chain via `faust2lv2` |
| `:4445` telemetry verbs, core-busy % | Linux telemetry surface (UDP / status file) | **port** | low | Keep the diagnosability looper had |

## The two-sided ledger

**Deleted / evicted** (maintenance that leaves the project): the hand-rolled UAC2
decoder (source of every buzz/crackle lesson), the entire raw WLAN/DHCP/ARP/IGMP/
Link-wire stack, and the effects-maintenance burden (now declarative Faust LV2).

**Cost** (new work this migration takes on): a PREEMPT_RT kernel + RT tuning, the
`f_uac2` gadget port, unwinding the multicore SEV/WFE IPC to pthreads, musl
verification, and accepting a slightly worse *worst-case* jitter than bare metal
(mitigated to tens of µs by RT + core isolation — inaudible when tuned).

The net is strongly positive: the migration **retires two entire subsystems
worth of hand-rolled bugs** while adding the moddability the project exists for.
