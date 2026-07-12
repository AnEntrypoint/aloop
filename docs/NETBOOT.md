# Netbooting aloop over TFTP/DHCP — the pre-SD dry run

Before you burn an SD card, boot the **exact same** aloop diskless Alpine tree
over the network. The Pi 4 fetches its firmware, kernel, and config over TFTP
(address from DHCP), then Alpine boots diskless into RAM and restores the aloop
apkovl — identical to the SD path (`docs/FLASHING.md`), but with **zero
reflashing** between iterations. This is the fastest way to shake out boot issues.

Everything the Pi runs is the same as the card: same firmware chain, same
kernel + initramfs, same `aloop.apkovl.tar.gz`, same dwc2/serial/isolcpus config.
The SD image and the netboot root are assembled from **one source of truth**
(`image/lib-boot-tree.sh`), so what you netboot is what you'd flash.

## 1. Build the netboot root

On any Linux/Alpine host with `curl tar` (no `mtools`/FAT tooling needed — the
netboot root is a plain directory):

```sh
ALOOP_BIN=build/aloop LV2_DIR=effects/home image/build-netboot.sh   # -> aloop-netboot/
image/validate-netboot.sh aloop-netboot                              # structural check
```

A build with no `ALOOP_BIN`/`LV2_DIR` still produces a valid, bootable layout
(it just warns and has no binary/effects yet) — good for validating the boot path.

The result is a directory containing the Pi 4 firmware chain (`bootcode.bin`,
`start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`), the kernel payload
(`boot/vmlinuz-rpi`, `boot/initramfs-rpi`, `boot/modloop-rpi`), `config.txt` /
`cmdline.txt` / `usercfg.txt`, and `aloop.apkovl.tar.gz`.

## 2. Enable network boot on the Pi 4 (one-time)

The Pi 4 boot order lives in the bootloader EEPROM. Set it to try network boot.
Easiest is the Raspberry Pi Imager → *Misc utility images* → *Bootloader* →
*Network Boot*, flashed to a throwaway SD (it only rewrites the EEPROM, then you
remove the card). Or from a running Pi OS:

```sh
sudo rpi-eeprom-config --edit
# set:  BOOT_ORDER=0xf21   (0x2 = network, 0x1 = SD — tries network first, SD fallback)
```

With no SD inserted, the Pi 4 will DHCP + TFTP on power-up.

## 3. Serve it from the host

Run `dnsmasq` on a Linux host cabled to the Pi (directly or via a switch) on the
same wired LAN. The provided config answers the Pi 4 bootloader's DHCP and serves
the netboot root over TFTP:

```sh
sudo dnsmasq --conf-file=src/net/config/netboot-dnsmasq.conf \
             --interface=eth0 \
             --tftp-root="$PWD/aloop-netboot" \
             -d
```

- Set `--interface` to your host's wired NIC.
- `-d` keeps dnsmasq in the foreground so you watch the DHCP + TFTP transfers live
  (this is where the first boot issues show up — a missing file logs as a failed
  TFTP request).
- If the LAN already has a DHCP server, switch to **proxy DHCP** so dnsmasq only
  answers the boot part: use `--dhcp-range=192.168.50.0,proxy` instead of the range
  in the conf. (The conf ships a standalone range for an isolated Pi↔host link.)
- The Pi 4 requests its files under a subdirectory named by its serial number
  (`tftp-unique-root`). For a single Pi you can also drop the netboot-root files
  directly at the TFTP root — dnsmasq falls back to the root if the serial subdir
  is absent. To pin it, `mkdir aloop-netboot/<serial>` and copy the tree in, or
  read the serial from the failed TFTP request dnsmasq logs on the first attempt.

## 4. Boot and watch (serial console)

Wire the 3.3 V USB-UART exactly as for the SD path (`docs/FLASHING.md` §3:
GND→pin 6, Pi TX GPIO14→adapter RX, Pi RX GPIO15→adapter TX, **115200 8N1** —
`enable_uart=1` is carried into the netboot config too). `screen /dev/ttyUSB0
115200`, power the Pi, and watch:

- dnsmasq (`-d`) logs the DHCP handshake then each TFTP fetch: `bootcode.bin` →
  `start4.elf` → `config.txt` → `bcm2711-rpi-4-b.dtb` → kernel + initramfs.
- The initramfs brings up the NIC (`ip=dhcp`, added by the netboot builder), mounts
  `modloop-rpi`, restores `aloop.apkovl.tar.gz`, and OpenRC starts the `aloop` +
  `autoap` services — same as `docs/BOOT.md`.
- The USB-audio gadget still comes up (`dtoverlay=dwc2,dr_mode=peripheral`); network
  boot uses the Pi's **Ethernet**, not the USB-C/OTG port, so the f_uac2 gadget and
  netboot do not conflict.

## What netboot is (and isn't) good for

- **Good for:** iterating the boot config, the apkovl layout, the service startup,
  and catching "a shipped path doesn't resolve on the device" issues — all without
  reflashing. `validate-netboot.sh` catches the structural class in CI before you
  even boot.
- **Not a substitute for** the on-hardware measurements (`docs/HARDWARE-TESTS.md`):
  RT jitter, f_uac2 round-trip latency, and Link-over-WiFi still need a real Pi 4
  and are measured there. Netboot just gets you to a booted appliance faster.

## Known first-boot considerations (solved in the build)

- **apkovl over TFTP** — Alpine diskless restores `<hostname>.apkovl.tar.gz` from
  the boot medium; the builder places it in the served root so the initramfs finds
  it over TFTP (validated by `validate-netboot.sh`).
- **modloop over the network** — `boot/modloop-rpi` (the kernel-modules squashfs)
  is served in the netboot root and mounted by the initramfs; `ip=dhcp` on the
  cmdline gives the initramfs the network it needs.
- **firmware completeness** — the Alpine RPi tarball already ships the full Pi 4
  TFTP firmware chain (`start4.elf`/`fixup4.dat`/`bootcode.bin`/`bcm2711-rpi-4-b.dtb`);
  no external firmware sourcing is required.
