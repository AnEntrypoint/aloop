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

`image/serve-netboot.sh` runs the whole serving stack (DHCP + TFTP + the HTTP root
server) with the exact flags witnessed booting a real Pi 4. Build the netboot root
with a `NETBOOT_SERVER` that matches the host's IP on the Pi's link (so the baked-in
`alpine_repo`/`modloop`/`apkovl` URLs point back at this host), then serve:

```sh
# build so the cmdline's HTTP URLs point at THIS host (default 192.168.137.1)
NETBOOT_SERVER=192.168.137.1 ALOOP_BIN=build/aloop LV2_DIR=effects/home \
  OUT=/srv/tftp/aloop-netboot image/build-netboot.sh

sudo image/serve-netboot.sh --iface eth0 --server 192.168.137.1 \
     --root /srv/tftp/aloop-netboot
```

Three things get served, and **all three are needed** (learned the hard way on a
real boot):

1. **DHCP** hands the Pi an address + the boot filename. Default is **standalone**
   (dnsmasq owns the lease). Use `--proxy` only if another DHCP server on the LAN
   actually leases to the Pi — in the witnessed Windows-ICS case ICS did *not* lease
   the Pi, so proxy mode left it with no address and it never reached TFTP;
   standalone is the working default.
2. **TFTP** serves the firmware/kernel/initramfs. The Pi 4 requests them under its
   **board-serial** subdir (`<serial>/start4.elf`, e.g. `7bec0617/`) — the serve
   script auto-creates `<root>/<serial>/` as symlinks to the root the first time it
   sees the serial in the log, so you don't have to know it in advance.
3. **HTTP (:8080)** serves the Alpine *root* (the apks repo, `modloop-rpi`, and the
   apkovl). The diskless initramfs fetches these over HTTP per the cmdline — this is
   what makes the whole thing work; see the panic note in §5.

### Running it on Windows via WSL2

The witnessed setup was a Windows host with WSL2 Ubuntu (mirrored networking, so WSL
sees `eth0 = 192.168.137.1/24`, the Internet-Connection-Sharing subnet the Pi is
cabled to). `dnsmasq` + `python3` live in WSL. Two Windows-specific gotchas:

- **WSL2 kills all distro processes when the last `wsl` session exits** — detached
  `setsid`/`nohup` do *not* survive. Run the serve command in a session that stays
  open (a background terminal), or the servers die the moment the launcher returns.
- **Git-Bash rewrites `/mnt/c/...` paths** — prefix `wsl` calls with
  `MSYS_NO_PATHCONV=1` when passing a `/mnt/c/...` script path.
- Windows Firewall must allow the inbound DHCP(67)/TFTP(69)/HTTP(8080) — in the
  witnessed run all firewall profiles were off, so nothing was blocked.

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

## 5. Known first-boot issues — all found on a real Pi 4 and solved in the build

These are the actual problems a live netboot surfaced (Pi 4, board serial
`7bec0617`), each fixed in `image/build-netboot.sh` / `image/serve-netboot.sh`:

- **Initramfs never DHCPs → drops to an emergency shell (`cmdline.txt` had an embedded
  newline)** — the single highest-impact bug. The Pi 4 firmware reads **only the first
  line** of `cmdline.txt` as the kernel command line. The stock Alpine `cmdline.txt`
  ends with a trailing newline, and the builder appended the RT + netboot params
  *after* it, leaving an **embedded `\n`** — so `isolcpus`, `ip=dhcp`, `alpine_repo`,
  `modloop` and `apkovl` were **all silently dropped**. Without `ip=dhcp` the Alpine
  init sets `do_networking=false`, the initramfs `udhcpc` never runs (the live log
  showed **only the firmware's DHCP, never the initramfs's**), no root arrives over
  the network, and init drops to the emergency recovery shell. (It also silently
  disabled `isolcpus` RT isolation on the **SD card** too.) **Fix:** `boot_tree_config`
  (`image/lib-boot-tree.sh`) and the netboot append (`image/build-netboot.sh`) now
  collapse `cmdline.txt` to a **single line** (strip all newlines, join, one trailing
  `\n`). Witness with `od -c aloop-netboot/cmdline.txt` — there must be exactly one
  `\n`, at EOF, and `ip=dhcp`/`alpine_repo`/`modloop`/`apkovl`/`isolcpus` must all be
  on that one line.
- **Kernel panic `unable to mount root fs, unknown-block(0,0)`** — the Pi 4 firmware
  TFTP-loads config/kernel/initramfs fine, but the Alpine *diskless* initramfs then
  has no block device to find the apks/modloop/apkovl on. **Fix:** the Alpine RPi
  initramfs honors HTTP boot params, so `build-netboot.sh` adds
  `alpine_repo=http://<server>:8080/apks modloop=…/boot/modloop-rpi
  apkovl=…/aloop.apkovl.tar.gz` to the cmdline, and `serve-netboot.sh` runs an HTTP
  server for the root. The initramfs then fetches the whole root over HTTP (watch
  for `GET /apks/aarch64/APKINDEX.tar.gz`, `GET /boot/modloop-rpi`,
  `GET /aloop.apkovl.tar.gz`, all `200`).
- **Pi drops off the wired LAN after boot** — `ip=dhcp` brings `eth0` up only inside
  the *initramfs*; the aloop overlay manages `wlan0` (autoap) but nothing brings
  `eth0` back up once userspace starts, so a netbooted Pi became unreachable right
  after the HTTP root fetches. **Fix:** the netboot build injects
  `/etc/network/interfaces` (`eth0 inet dhcp`) + enables the `networking` service in
  the apkovl for the netboot build only (the SD image boots from local media and
  doesn't need it).
- **No DHCP address in proxy mode** — with Windows ICS on the link *not* actually
  leasing to the Pi, proxy DHCP gave the Pi no address at all. **Fix:** standalone
  DHCP is the serve-script default (`--proxy` only when a real DHCP server leases).
- **DHCP loops DISCOVER↔OFFER, never REQUEST** — when a *second* DHCP server shares
  the link (Windows ICS runs one on the same NIC and can't easily be stopped without
  admin), the Pi 4 saw two offers and never accepted ours, looping forever without
  reaching TFTP. **Fix:** `serve-netboot.sh` runs dnsmasq `--dhcp-authoritative` so it
  answers decisively and the handshake completes (DISCOVER→OFFER→REQUEST→ACK).
- **Kernel panic with NO initramfs (TFTP `Permission denied`)** — the Alpine tarball
  ships `boot/initramfs-rpi` as mode `600` (root-only) and `cp -a` preserves it; the
  TFTP server drops privileges to `nobody`, so it got *Permission denied* and sent the
  kernel with **no initramfs** → panic "unable to mount root fs". (Distinct from the
  `unknown-block(0,0)` panic above — here the initramfs never loads at all.) **Fix:**
  `build-netboot.sh` (and `serve-netboot.sh`) `chmod -R a+rX` the served tree so any
  unprivileged TFTP/HTTP server can read every file. Watch the dnsmasq log for
  `sent .../boot/initramfs-rpi` — a `Permission denied` there is this bug.
- **Per-serial TFTP path** — the Pi 4 requests `<board-serial>/start4.elf`, not the
  MAC. **Fix:** `serve-netboot.sh` auto-creates `<root>/<serial>/` as symlinks to
  the root when it first sees the serial in the TFTP log.
- **apkovl + modloop reachable** — served over HTTP (above), not assumed on a local
  FS. **firmware completeness** — the Alpine RPi tarball already ships the full Pi 4
  TFTP firmware chain (`start4.elf`/`fixup4.dat`/`bootcode.bin`/`bcm2711-rpi-4-b.dtb`);
  no external firmware sourcing is required.

## 6. Debug boot — see exactly where the initramfs stalls

When a netboot stalls in the initramfs, don't guess — build with `NETBOOT_DEBUG=1`
and read the serial console. The debug cmdline drops `quiet` and adds `debug_init`
(the Alpine init runs `set -x`), so every step prints:

```sh
NETBOOT_DEBUG=1 NETBOOT_SERVER=192.168.137.1 OUT=/srv/tftp/aloop-netboot image/build-netboot.sh
```

Attach a serial console (the `dwc2`/`enable_uart=1` config is already in `usercfg.txt`)
and power-cycle. A **healthy** boot prints, in order:

```
Loading boot drivers    → Mounting boot media    → Obtaining IP via DHCP (eth0)
→ GET /apks/aarch64/APKINDEX.tar.gz  → GET /boot/modloop-rpi  → GET /aloop.apkovl.tar.gz
```

Where it stops tells you the layer:

- **No `Obtaining IP via DHCP (eth0)` line at all** → the kernel never got `ip=dhcp`
  (the embedded-newline bug above — check `od -c cmdline.txt`).
- **`Obtaining IP via DHCP (eth0)` but no DHCP in the serve log** → the initramfs
  `udhcpc` DISCOVER isn't reaching your dnsmasq. **The wired link must have no other
  DHCP server for the initramfs round** — Windows ICS runs a second one on the same
  NIC; `--dhcp-authoritative` covers the firmware round but the cleanest fix is to
  serve on a link with ICS disabled (or no competing DHCP). Watch the serve log for a
  DHCPDISCOVER *seconds after* the firmware's TFTP GETs — that one is the initramfs.
- **DHCP ok but no `GET /…modloop-rpi`** → the HTTP root server (`:8080`) is
  unreachable from the Pi, or the URLs in `cmdline.txt` point at the wrong
  `NETBOOT_SERVER`.

## 6. Self-updating serve (Windows) — auto-refresh from CI, like looper's tftp-server.js

`image/serve-netboot-win.js` (the native Windows DHCP+TFTP+HTTP server, §3's
Windows path) polls GitHub Actions every 30 seconds for a newer green
`build-binary`/`build-lv2` run and, when one appears, downloads the artifacts,
rebuilds the served netboot root **in place** (`image/build-netboot.sh`), and
sends `REBOOT` to the already-running Pi (`src/control/remote_control.cpp`,
udp/4446) so it picks up the fresh build on its next boot — no manual re-serve,
no manual re-flash, matching looper's `tftp-server.js` `checkAndUpdate()` loop.

```sh
# aloop's repo is PRIVATE, so a token with `repo` + `actions:read` scope is
# REQUIRED (unlike looper's public-repo Releases, which need no auth at all).
# `gh auth token` works if you're already `gh auth login`'d.
GITHUB_TOKEN=$(gh auth token) node image/serve-netboot-win.js \
  --root .netboot-serve --server 192.168.137.1 \
  --pi 192.168.137.100 --pi-token <the aloop.conf [remote] token=>
```

What it does each poll tick:

1. Lists the latest successful run of `build-binary.yml` and `build-lv2.yml` on
   `main` via the Actions API (no `gh` CLI dependency — raw `https.get`, same
   shape as looper's `httpsGet`/`downloadFile`).
2. Compares the pair of `head_sha`s against `.netboot-update-sha` (sibling of
   the netboot root) — matches looper's `.tftp-sha` tracking file.
3. On a new sha: downloads both artifacts (`aloop-aarch64-musl`,
   `home-fx-lv2`), unzips them (PowerShell `Expand-Archive` — no extra
   dependency), and re-runs `build-netboot.sh` with `ALOOP_BIN`/`LV2_DIR`
   pointed at them and `OUT` pointed at the live-served root.
4. Sends `REBOOT:<token>` to `--pi`/`--pi-token` so a Pi that's already running
   picks up the change immediately; a Pi that's mid-boot or off just gets the
   new build on its next power-cycle regardless (the whole root is re-fetched
   fresh every boot — there's no stale-kernel risk like an SD card).

Options: `--update-interval <ms>` (default 30000, matches looper's 30s poll);
`ALOOP_NO_AUTO_UPDATE=1` disables the loop entirely (matches looper's
`LOOPER_NO_AUTO_UPDATE`); omitting `--pi-token`/`PI_TOKEN` skips the `REBOOT`
step with a clear log line (the rebuild still happens — only the "poke a
running Pi right now" step is skipped).

**Windows commonly has 3 different `bash.exe` on PATH** (Git-Bash, the WSL
launcher stub under `System32`, a WindowsApps alias) — `build-netboot.sh` is
invoked via an explicitly-pinned Git-Bash path (`C:\Program Files\Git\bin\bash.exe`
by default), not a bare `bash` lookup, because a bare lookup can resolve to the
WSL stub depending on launch environment, which cannot see `C:/...` paths at
all and fails with a misleading "No such file or directory" on a path that
verifiably exists. If the rebuild step ever fails with that exact error, check
which `bash.exe` actually ran, not whether the script path is correct.

**WITNESSED live** (real Pi 4, board serial `7bec0617`): the update loop found
a green build, downloaded + rebuilt the root, and the Pi's own DHCP/TFTP fetch
of firmware/kernel/initramfs was served correctly **concurrently**, with no
interference between the two. The rebuilt `aloop.apkovl.tar.gz` carried the
fresh binary+LV2 (grew from a smaller layout-only size to a real payload).
