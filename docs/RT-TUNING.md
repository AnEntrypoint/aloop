# RT tuning — why each knob exists

The goal: worst-case scheduling jitter in the **tens of microseconds** so a
64-sample (1.333 ms) audio block never misses its deadline. Every setting in
`kernel/cmdline.txt` and `kernel/rt-tune.sh` maps to a specific latency source.

| Knob | Where | Why |
|------|-------|-----|
| **PREEMPT_RT kernel** | kernel config | Bounds the *worst-case* time between an interrupt and the audio thread running. Without it, stock Linux can stall audio for milliseconds under load. This is the single biggest lever. |
| `isolcpus=domain,managed_irq,1,3` | `cmdline.txt` | Removes cores 1 and 3 (the audio cores) from the general scheduler, so only our pinned threads run there — nothing else steals a time slice. **WITNESSED GAP, this session (real Pi 4):** the bare `isolcpus=1,3` form only isolates the *scheduler domain* (ordinary userspace tasks) — it does NOT exclude per-CPU kernel housekeeping threads. Live inspection (`/proc/<tid>/stat`'s last-run-cpu field for every thread on the system) found `kworker/1:0-events`, `ksoftirqd/1`, and `kworker/1:1-rcu_gp` all currently schedulable on CPU1 (the home-FX audio core) alongside the pinned SCHED_FIFO audio thread — despite `rcu_nocbs=1,3` already moving RCU *callback processing* off these cores (a related but distinct mechanism: `rcu_nocbs` stops callbacks from running there, it does not stop the `rcu_gp`/`kworker` *threads themselves* from being schedulable there). Any one of these firing mid-block is a real, sporadic multi-microsecond stall on an otherwise-idle-looking core — the kind of jitter that shows up as occasional xruns without moving average `core_busy%` much, and plausibly why adding more DSP work (shrinking the per-block slack that used to silently absorb these stalls) made a pre-existing gap suddenly audible as crackling. **Fix:** the extended `isolcpus=domain,managed_irq,...` form (kernel 5.8+, this Pi runs 6.6.49) additionally excludes IRQ-affinity-managed interrupt handling and general kernel-thread placement from the isolated cores, not just the scheduler domain. This is a cmdline-only change verified reachable on this kernel version; a real cyclictest run (see below) is still needed to confirm it closes the gap in practice, not just in theory. |
| `nohz_full=1,3` | `cmdline.txt` | Stops the periodic scheduler tick on the audio cores; the tick is a recurring source of jitter. |
| `rcu_nocbs=1,3` + `rcu_nocb_poll` | `cmdline.txt` | Moves RCU callback processing off the audio cores (another background jitter source). |
| `threadirqs` | `cmdline.txt` | Runs IRQ handlers in threads so their priority can be set — lets audio threads out-prioritize non-audio interrupts. |
| `governor=performance` | `rt-tune.sh` | Frequency scaling parks the CPU; waking it costs microseconds. Pin to max clock. |
| deep C-states disabled | `rt-tune.sh` | Deep idle states take microseconds to exit — enough to blow a block. Disabled on the audio cores. |
| **network/USB IRQ affinity → control core** | `rt-tune.sh` | **The mechanism that makes Link-over-WiFi not glitch audio.** WiFi/USB interrupts are steered onto Core 2, so the audio cores never service them. This is *the* condition behind feasibility Requirement 1. Works on Pi 4; Pi 5's RP1 blocks it (ADR-006). |
| `SCHED_FIFO` + high rtprio | audio threads | The audio threads run under real-time scheduling above everything else, so they always get the CPU when a block is due. |
| `mlockall` + unlimited memlock | audio process | Locks all memory into RAM so the audio thread never page-faults (a fault would blow the deadline). |
| pinned thread affinity | audio threads | home-FX → Core 1, user-FX → Core 3, matching `isolcpus`. |

## How we know it's tuned (the verification, needs hardware)

The proof is a `cyclictest` run under simultaneous audio + WiFi/AP + MIDI load,
confirming worst-case wakeup latency stays under the block budget with zero
xruns over a long run. That measurement **requires a real Pi 4** and is parked as
a hardware-dependent row (ADR-009) with the exact command documented:

```
cyclictest -m -S -p 95 -i 200 -d 0 -h 400   # while aloop runs with Link over WiFi
```

Until that runs on hardware, the tuning is *configured correctly by design* but
the jitter number is not yet *measured* — and we say so rather than claiming it.
