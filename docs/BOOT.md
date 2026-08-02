# Boot sequence — from power-on to making sound

The whole runtime, in order, so the device is legible. Configured by
`config/aloop.conf` and the overlay laid down by `.github/workflows/build-image.yml`.

## Board support matrix

`build-image.yml` builds one image per board via `image/build-image.sh BOARD=<x>`
(see `image/lib-boot-tree.sh`'s `board_supports_usb_gadget`/`board_wifi_irq_name`
for the authoritative capability table):

| Board | Boot chain | USB-audio gadget (step 3 below) | WiFi |
|-------|-----------|----------------------------------|------|
| pi4 (+ CM4, Zero2) | Pi firmware (FAT partition) | dwc2 peripheral mode — real UAC2 gadget | Broadcom brcmfmac |
| pi3 | Pi firmware (FAT partition) | none — Pi 3 has no OTG-capable controller | Broadcom brcmfmac |
| pi5 | Pi firmware (FAT partition) | none — RP1 southbridge USB is host-only | Broadcom brcmfmac |
| opi-prime (Orange Pi Prime, Allwinner H5) | Armbian-sourced U-Boot (raw SD sectors) + ext4 root + extlinux.conf | unproven (MUSB dual-role controller exists on the micro-USB OTG port; no confirmed f_uac2 report for this SoC) | Realtek RTL8723BS |

Boards without a working USB-audio gadget still run the full aloop DSP/effects
stack and Ableton Link — they just cannot present themselves as a USB soundcard
to a host computer the way pi4 does; see `docs/CLONE-PARITY.md`/AGENTS.md for the
audio-I/O implications on those boards.

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
