# Boot sequence — from power-on to making sound

The whole runtime, in order, so the device is legible. Configured by
`config/aloop.conf` and the overlay laid down by `.github/workflows/build-image.yml`.

```
power on
  │
  ├─ 1. Alpine boots diskless from RAM (read-only root, OpenRC)
  │      WHY: no disk writes, no background daemons — near-bare-metal determinism.
  │
  ├─ 2. RT tuning applied  (/etc/local.d/10-rt-tune.start → kernel/rt-tune.sh)
  │      · governor=performance, deep C-states off on the audio cores
  │      · network/USB IRQs steered onto the control core (Core 2)
  │      · (isolcpus/nohz_full/rcu_nocbs already set by kernel/cmdline.txt)
  │      WHY: so the audio cores meet the 1.333 ms deadline regardless of load.
  │
  ├─ 3. USB gadget up  (libcomposite + configfs f_uac2 on dwc2)
  │      The Pi now presents itself to a host as a UAC2 soundcard (mono/48k).
  │      WHY: replaces looper's hand-rolled UAC2 — the kernel does microframes right.
  │
  ├─ 4. aloop process starts  (/opt/aloop/aloop --config /etc/aloop.conf)
  │      · reads aloop.conf (cores, RT priority, effect dirs, topology)
  │      · mlockall(); spawns the audio threads SCHED_FIFO, pinned:
  │           Core 1 → home-FX,  Core 3 → user-FX,  Core 2 → control
  │      · loads the home-FX LV2 (/effects/home) + any user LV2 (/effects/user)
  │        into the in-process host (no graph — ADR-002)
  │      · opens the ALSA PCM bridged to the f_uac2 gadget
  │
  ├─ 5. Control plane comes up on Core 2
  │      · Ableton Link (official lib) joins the session over UDP multicast
  │      · MIDI (ALSA rawmidi) — the APC / controller knobs → the param snapshot
  │      · telemetry socket (core load, xruns, Link sync, AP/STA state)
  │
  ├─ 6. WiFi / autoAP  (/opt/aloop/autoap.sh, or an OpenRC service)
  │      · try to join a known network (STA)
  │      · if none: host the 'aloop' AP so peers can Link (ap_isolate=0)
  │
  └─ READY: audio flows USB-in → home-FX (Core 1) → user-FX (Core 3) → USB-out,
            synced to Link, tunable from MIDI, no added latency vs bare metal.
```

## What can go wrong, and what happens (degraded modes)

None of these crash the device — every failure path is explicit (see
`docs/DEGRADED-MODES.md`):

| Situation | Behavior |
|-----------|----------|
| No user LV2 present | Home-FX only. Normal. |
| A user LV2 crashes/hangs | Watchdog disables it; home chain + audio continue (ADR-002). |
| No external WiFi network | Host the AP; peers still Link. |
| No Link peers | Free-run the internal tempo. |
| USB host disconnects | Output silence; loops keep their state; resume on reconnect. |
| An RT xrun occurs under extreme load | Logged in telemetry; audio recovers next block (the tuning exists to make this rare). |
