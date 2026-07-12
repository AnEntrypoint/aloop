# Flashing aloop to an SD card and testing from it

This is the card-test procedure. The CI `build-image` workflow produces
`aloop-pi4.img.gz` (a real, bootable Alpine diskless image for the Pi 4); flash it,
boot it, and the Pi comes up as the aloop appliance.

## 1. Get the image

- **From CI:** open the latest green `build-image` run on `AnEntrypoint/aloop` and
  download the **`aloop-pi4-image`** artifact (`aloop-pi4.img.gz`).
- **Locally** (on a Linux/Alpine host with `mtools dosfstools fdisk curl`):
  ```sh
  ALOOP_BIN=build/aloop LV2_DIR=effects/home image/build-image.sh   # -> aloop-pi4.img
  image/validate-image.sh aloop-pi4.img                             # structural check
  ```

Verify the download:
```sh
gunzip aloop-pi4.img.gz
image/validate-image.sh aloop-pi4.img     # must print "IMAGE VALID"
```

## 2. Flash it

Pick one:

- **Raspberry Pi Imager** → "Use custom image" → `aloop-pi4.img` → your SD card.
- **balenaEtcher** → select the image → the card → Flash.
- **`dd`** (Linux/macOS, double-check the device — this erases it):
  ```sh
  # find the card first (lsblk / diskutil list); then, replacing sdX:
  sudo dd if=aloop-pi4.img of=/dev/sdX bs=4M conv=fsync status=progress
  sync
  ```

## 3. Wire it up

- **SD card** → Pi 4 slot.
- **USB audio to the host:** connect the Pi 4's **USB-C power/OTG port** to the
  computer you want it to be a soundcard for. The port is in peripheral mode
  (`dtoverlay=dwc2,dr_mode=peripheral`), so the host sees a **UAC2 mono 48 kHz
  soundcard** named `aloop`. (Power the Pi from the 5 V GPIO pins or a powered hub
  if the OTG port is busy being the gadget.)
- **MIDI controller** (optional): a class-compliant USB MIDI controller on a
  USB-A port drives the loopers/effects per `config/controls.conf` (remappable).
- **Serial console (recommended for the first boot):** a 3.3 V USB-UART on the
  GPIO header lets you watch boot without a display —
  - GND → pin 6, **Pi TX GPIO14 → adapter RX** (pin 8), **Pi RX GPIO15 → adapter TX**
    (pin 10), **115200 8N1** (`enable_uart=1` is set).
  - `screen /dev/ttyUSB0 115200` (or PuTTY) to watch it come up.

## 4. First boot — what to expect

- Alpine boots diskless into RAM and restores the `aloop.apkovl.tar.gz` overlay.
- `/etc/local.d/*.start` run in order: **10** RT-tune (isolcpus already applied via
  cmdline; sets IRQ affinity + rt limits), **20** brings up the `f_uac2` USB-audio
  gadget, then the **aloop** + **autoap** OpenRC services start (supervised —
  they respawn on crash; logs in `/var/log/aloop.log`).
- The host should enumerate the `aloop` USB soundcard within a few seconds of the
  OTG cable being connected.
- **WiFi / Ableton Link:** `autoap` joins a known network if `wpa_supplicant.conf`
  has one; otherwise it hosts an AP (SSID `aloop`, `ap_isolate=0` so Link multicast
  works). Put a Link-enabled app on the same network and it should sync tempo.

## 5. Verify it's alive

Over the serial console or (if networked) from another machine:
```sh
# status file on the device:
cat /run/aloop/status.json
# or query the telemetry UDP responder (port 4445) from a peer:
echo status | nc -u -w1 <pi-ip> 4445
```
You get JSON: `core_busy` (per-core %), `xruns`, `link` (synced/bpm), `wifi`, and
`loopers` (rec/play bitmaps + vols) — the live device state.

## 6. Run the hardware measurements

The build is green; the numbers that need real silicon are the last step. On the
booted Pi:
```sh
/opt/aloop/test/hardware/run-all.sh     # or the individual test-*.sh
```
See **docs/HARDWARE-TESTS.md** for what each measures (RT jitter, f_uac2 round-trip
latency, Link no-glitch, AP multicast) and the pass criteria. If RT jitter misses
the target with the stock kernel, build the PREEMPT_RT kernel
(`kernel/build-rt-kernel.sh`, ADR-011) and re-flash.

## Dropping in a user effect

The free core runs a swappable user LV2. Drop an `.lv2` bundle into `/effects/user`
(mount the card's boot FAT partition on any computer, or `lbu add` on the device)
and reboot — the in-process host loads it after the home stack, zero added latency.
