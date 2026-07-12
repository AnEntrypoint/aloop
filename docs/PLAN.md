# Build plan and drain order

The entire migration is enumerated as task rows in `.gm/prd.yml` (56 rows at
kickoff) and drained in **dependency order** — hardest reachable node proven
early, on-hardware measurements deferred honestly. This document is the human-
readable view of that plan and the order we work it.

## Drain order (dependency DAG)

```
1. Scaffold + docs        ── repo tree, README, this plan, ARCHITECTURE, DECISIONS, MIGRATION-MAP
        │                    (done first so the plan is legible in the repo)
        ▼
2. Publish                ── AnEntrypoint/aloop, so the plan is visible before we build
        │
        ▼
3. DSP core port          ── extract the Circle-free engine from ../looper; compile standalone;
        │                    reuse the dubfx A/B harness as the regression oracle
        ▼
4. LV2 host + home bundle ── the in-process host; package the dubfx chain as home-FX LV2 (CI-built)
        │                    ── the crux of the moddability + zero-latency goals
        ▼
5. Link integration       ── official Link lib → the snapshot → the loop grid
        │
        ▼
6. Control / MIDI         ── control plane on Core 2, ALSA MIDI, telemetry
        │
        ▼
7. WiFi / AP (autoAP)     ── wpa_supplicant + hostapd + dnsmasq + switch service
        │
        ▼
8. USB gadget (f_uac2)    ── the hardest port; configfs gadget + ALSA bridge
        │
        ▼
9. Alpine image           ── diskless/RAM image, OpenRC services, boot config
        │
        ▼
10. RT kernel + tuning     ── PREEMPT_RT kernel, isolcpus/IRQ-affinity/SCHED_FIFO/mlockall
        │
        ▼
11. CI (GitHub Actions)    ── build LV2 bundles, cross-compile binary, assemble image
        │
        ▼
12. On-hardware verify     ── (needs a real Pi 4) RT jitter, f_uac2 latency, Link no-glitch, AP multicast
```

## Buildable-now vs needs-hardware

A hard, honest split (see `DECISIONS.md` ADR-009):

- **Buildable now** (all code, config, CI, docs): steps 1–11. Every source file,
  every service config, every workflow, every doc is reachable on a dev host and
  in CI containers.
- **Needs a real Pi 4 to *measure*** (step 12): RT jitter (cyclictest), f_uac2
  round-trip latency, on-air Link no-glitch under WiFi, AP multicast between
  peers. These rows carry `blockedBy: [external, no-Pi4-in-session]` with the
  exact on-hardware test written down — they are **never** marked done from a dev
  host. When a Pi 4 is available, the documented tests run and resolve them with
  real output.

## Why this order

- **Docs + publish first** because the user asked for the plan to be visible and
  instructive before building — and because the decision log must exist before
  decisions are made, so none are lost.
- **DSP core early** because everything downstream (the LV2 host, Link, the
  image) depends on a working audio path, and it's the lowest-risk port (already
  Circle-free).
- **LV2 host as the "hardest reachable node"** because it's where the two
  headline goals (moddable effects + zero latency) actually meet — proving it
  early de-risks the whole design.
- **f_uac2 and the RT kernel last among buildable steps** because they're the two
  tall poles and depend on the image existing to test in.
- **On-hardware verification genuinely last** because it cannot be faked.

## Current state

Track live progress in `.gm/prd.yml`. This session's scope: steps 1–2 (scaffold,
docs, publish) plus as much of the reachable build (3+) as the plan drains, with
CI wired. Hardware-dependent rows are parked honestly.
