# aloop — technical constraints reference

Durable constraints for this codebase and its build/deploy pipeline. Real Pi 4
device: `192.168.137.100`, root/aloop. Read before touching the device, the DSP,
or the image/netboot scripts.

## Contents

- Boards, images, boot trees
- Device runtime environment (Alpine/musl/aarch64)
- Deploy, netboot, SSH
- Mesh networking (`ticker` AP, Ableton Link)
- Audio thread and ALSA
- Faust DSP: language gotchas, compiler flags, verification
- LV2 hosting
- Control surface (`apc_grid.cpp`)
- Storage (USB ring recording)
- Working rules

---

# Working rules

## No comments in code, ever

No inline, block, or doc comments anywhere (C++, Faust `.dsp`, JS, shell, YAML,
config). A name, a function boundary, an extracted variable, or a small type IS
the explanation — rename or restructure instead of annotating. A paragraph-long
comment is the same violation at higher volume; explaining a "why" is not an
exemption.

Hardware quirks, root-causes, and design rationale belong in THIS file or
`.wfgy/lessons.md`, never inline.

A comment encountered anywhere — pre-existing, vendored, another session's — is
converted to self-explanatory code the same turn: read it, fix the root cause it
was compensating for, delete it. One sighting spawns a full sweep of that file.

## Never add audio-path latency

The existing ~7ms block latency must never grow — not temporarily, not to work
around an unrelated bug. If a fix seems to need a bigger ALSA buffer/period, more
block lag, or any added buffering stage, stop and ask first. Any audio glitch is a
regression to root-cause, not a hardware limit to negotiate around.

A wet effect's own algorithmic latency while engaged (e.g. `ef.transpose`'s
window, the SNAC engine's engaged-only latency) is not covered by this rule — it
is additive on top of an always-instant dry path, not part of the fixed block
chain.

## Never trust an in-repo comment as ground truth

Comments in this tree have been confidently wrong about current intent (loop
quantization spec), about performance ("already alloc-free" when it allocated per
block), and about numeric guarantees ("byte-exact passthrough" that measured
1.5e-05). Read what the code does; for spec questions, grill the user for the
current requirement rather than assuming either code or comment is right.

## Real hardware over asking the user to reproduce input

Prefer byte-level MIDI injection (`tcp/9401`, `src/control/midi.cpp`) or
SSH-based log/state inspection over asking the user to press buttons. Reserve
`AskUserQuestion` for physical steps only once a byte-level substitute is proven
impossible for that bug class (audible sound quality, real analog behavior) or the
user has said they want to verify by ear.

## Stay grounded in what this system is

A real-time C++/Faust audio looper on real ALSA hardware, a real Pi 4, real USB
devices, real MIDI gestures. Abstract "formal verification"/"proof
assistant"/"dependent types" framings do not apply and must not be adopted. Work
the concrete bug with the concrete tools this project uses: static reading, real
device logs, byte-level MIDI injection, CI-verified builds, DawDreamer renders.

## Compiling clean proves nothing about runtime safety

Repeatedly true here: a synthetic x86_64 A/B passed while real aarch64 codegen
SIGSEGV'd (`-mapp`); a JIT `compile()` reported success and crashed at `render()`
(`-fm def`); CI green meant "x86_64 compiled", never "runs on target". Any
numeric-approximation or codegen flag needs a real-target, real-signal test before
shipping.

## Diagnostic logging must carry wall-clock timestamps

A threshold-triggered log line's line-count density is not a proxy for elapsed
time. Always log `clock_gettime(CLOCK_MONOTONIC, ...)` as `t=<sec>.<ms>` alongside
the magnitude, or periodic-vs-bursty is indistinguishable.

---

# Boards, images, boot trees

## `image/lib-boot-tree.sh` is BOARD-parameterized

One source of truth for every board's boot tree, dispatched by a `BOARD` env var
(`pi3`/`pi4`/`pi5`/`opi-prime`, default `pi4`).

`boot_tree_apkovl` (binary, LV2, services, vendored libs) is 100% shared and
unconditional — architecture-independent aarch64 userspace. Only
`boot_tree_fetch` (firmware/kernel/DTB) and `boot_tree_config` (boot
cmdline/USB-gadget config) dispatch per board.

`board_supports_usb_gadget`/`board_wifi_irq_name` in `lib-boot-tree.sh` are the
authoritative capability source, not this table:

| Board | SoC | Boot chain | USB-audio gadget | WiFi chip |
|---|---|---|---|---|
| pi4 (+CM4, Zero2) | BCM2711, quad Cortex-A72 aarch64 | Pi firmware, FAT partition | dwc2 peripheral — real UAC2 gadget | Broadcom brcmfmac |
| pi3 | BCM2837, quad Cortex-A53 aarch64 | Pi firmware, FAT partition | none — no OTG-capable controller | Broadcom brcmfmac |
| pi5 | BCM2712, quad Cortex-A76 aarch64 | Pi firmware, FAT partition | none — RP1 southbridge USB is host-only | Broadcom brcmfmac |
| opi-prime | Allwinner H5, quad Cortex-A53 aarch64 | Armbian U-Boot (raw SD sectors) + ext4 root + extlinux.conf | unproven | Realtek RTL8723BS |

## Orange Pi Prime specifics

**SoC is Allwinner H5, not H3.** (H3 powers the cheaper PC/One/Zero/Lite boards;
the Prime is H5 — see the mainline `sun50i-h5-orangepi-prime.dts`.) H5 is 64-bit
Cortex-A53 aarch64, so Alpine's existing aarch64 packages apply directly with no
32-bit userland detour.

**USB-audio-gadget mode is UNPROVEN.** H5 uses a MUSB dual-role controller
(`sunxi-musb`, `drivers/usb/musb/sunxi.c`) on the micro-USB OTG port; the 3
full-size USB-A ports are host-only EHCI/OHCI and can never do gadget mode. No
source confirms `f_uac2` has ever run on H3/H5. `board_supports_usb_gadget`
returns false for `opi-prime`; the fallback is the board's built-in analog codec
(3.5mm in/out) as a normal ALSA HOST device — a different architecture that
abandons the "looks like a USB soundcard to a laptop" design. If gadget-mode UAC2
is ever proven on real hardware, add `opi-prime` to the true case and update this.

**Boot chain is structurally incompatible with the Pi's FAT-partition firmware
model.** Allwinner's BootROM reads a raw SPL/U-Boot image at a fixed raw SD sector
offset (`dd ... seek=8`, 1K blocks) before any partition table exists; there is no
`config.txt` equivalent. `boot_tree_fetch_opi`/`boot_tree_config_opi` handle this:
download Armbian's `dl.armbian.com/orangepiprime/Trixie_current_minimal` **stable
redirect URL** (never a resolved `github.com/armbian/community/releases/...` asset
URL — Armbian's rolling trunk moves that version string every build), decompress,
read the image's own partition table via `sfdisk` (never assume a fixed offset),
extract the raw pre-partition-1 region as the U-Boot blob, loop-mount the ext4
root to pull kernel/`sun50i-h5-orangepi-prime.dtb`/initrd from `/boot`.
`boot_tree_config_opi` writes `/boot/extlinux/extlinux.conf` carrying the same
isolcpus/RT kernel cmdline as the Pi boards' `cmdline.txt`.

`image/build-image.sh`'s `opi-prime` branch assembles raw-U-Boot-at-8KiB + ext4
root instead of FAT32/mtools; needs real root (`sudo losetup`/`mount`), so CI or a
real Linux host only, never the Windows dev host. `image/validate-image.sh`
mirrors the split: `eGON.BT0` SPL magic at the real write offset, MBR type-83
partition, loop-mount the ext4 root to check `extlinux.conf`/dtb/apkovl.

**No netboot path — SD-card-flash-only.** Allwinner's BootROM requires U-Boot
resident on local media before PXE/TFTP is reachable. `build-image.yml`'s
netboot-build/validate/SD-zip steps are skipped for `BOARD=opi-prime`; only its
raw `.img.gz` is produced and released.

**`boot_tree_write_boot_scr_opi`'s `kernel_addr_r`/`fdt_addr_r`/`ramdisk_addr_r`
must match THIS specific U-Boot build's own compiled-in defaults, not generic
sunxi-common.h values.** Real hardware reached `booti`'s "Starting kernel"
handoff message with fully normal U-Boot output before it (version banner,
DRAM/MMC detection, correct load byte-counts, even a real HDMI U-Boot logo),
then total silence with zero earlycon output and a reset — consistent with a
devicetree the kernel cannot even locate its own UART in. The values previously
hardcoded here (`0x44000000`/`0x4a000000`/`0x4c000000`) were carried over from a
generic sunxi assumption and never verified against this build's real defaults.
`strings` on the U-Boot blob extracted straight from the downloaded
`dl.armbian.com/orangepiprime/Trixie_current_minimal` image (same extraction
offset/span `boot_tree_fetch_opi` already uses) shows this build's real
compiled-in env: `kernel_addr_r=0x40080000`, `fdt_addr_r=0x4FA00000`,
`ramdisk_addr_r=0x4FF00000` (also `loadaddr=0x42000000`,
`scriptaddr=0x4FC00000`) — none overlapping, well-spaced. Fixed to use these
real values. `fdt addr`/`fdt resize 65536` before `booti` (Armbian's own
`boot-sunxi.cmd` runs this to give U-Boot's in-place FDT edits headroom) was
already present from an earlier diagnostic round and stays, unchanged and
untested against the corrected addresses — this remains the next thing to
verify once real hardware or a serial adapter is available.

Two earlier hypotheses for this same silent-post-handoff symptom were tested
live and ruled out: relocated load addresses generically moved off
sunxi-common.h defaults (not derived from the real binary — this row's actual
fix), and an explicit `wdt stop` (kept, harmless either way).

`boot_tree_config_opi`'s `earlycon=uart8250,mmio32,0x01c28000` diagnostic
console param isolates whether the kernel crashes before or after its own
console driver initializes — `0x01c28000` is H5's uart0 MMIO base, same
8250-derived register layout other Allwinner boards use for earlycon;
`console=ttyS0,115200` is independently verified correct for this board's DTB
(`serial0` alias → `uart0`, `chosen` stdout-path).

Armbian's own compiled `bootcmd` sources `/boot/boot.scr` directly by fixed
filename and calls `booti` itself — it never touches `extlinux.conf`. Since
this project's U-Boot binary is sourced from Armbian's real build,
`extlinux.conf` alone is not reachable: real hardware confirmed this live
(SPL+U-Boot proper both genuinely complete their multi-second load each cycle,
then reset — consistent with `bootcmd` finding no `boot.scr` and having
nothing else to fall back to). `extlinux.conf` stays as a defensive fallback
(harmless, costs nothing) for any future U-Boot build that does have
`CONFIG_DISTRO_DEFAULTS` compiled in.

**WiFi is Realtek RTL8723BS.** `kernel/rt-tune.sh`'s IRQ-steering matches by
driver-name substring, so `rtl8723bs` is in its grep pattern alongside
`brcmfmac`. Link multicast behavior tuned against Broadcom should be re-validated
against this driver if Link sync accuracy is questioned on this board.

## apkovl assembly constraints

**`boot_tree_apkovl` must stamp `.default_boot_services`.** Alpine's
`rc_add modloop sysinit` gate (which also enables devfs/dmesg/mdev/hwdrivers — the
whole hardware-bring-up layer) is conditioned on
`[ -f "$sysroot/etc/.default_boot_services" -o ! -f "$ovl" ]`, so shipping an
apkovl with runlevels already populated silently opts out of it unless the marker
is present. Without it `/lib/modules` stays empty, `/proc/asound` never exists,
and `/sys/kernel/config/usb_gadget/` cannot be created even with `modloop-rpi`
fetched. Init removes the marker after reading it (one-shot Alpine mechanism).

**`aloop`'s OpenRC service needs `rc_ulimit="-l unlimited -r 95"`, not a `local.d`
`ulimit` call.** `kernel/rt-tune.sh`'s `ulimit -l unlimited` (memlock, needed for
`mlockall(MCL_FUTURE)`) runs inside a `local.d/*.start` script that OpenRC `eval`s
in a transient subshell — the change never reaches the separately-started `aloop`
process. `rc_ulimit` is read by `openrc-run.sh` immediately before it execs
`command`.

**`aloop`'s `depend()` needs `after local autoap`.** `aloop` constructs
`ableton::Link` and its UDP multicast socket during startup; with both services
declaring only `after local`, Link could open its socket before `autoap` brought
`wlan0` up. `src/main.cpp` additionally waits for the interface to carry an
address before starting Link.

**Vendor alsa-lib and the whole lilv stack as real `.so` files; never `apk add` at
boot.** The device's only reachable apk repo is the ~100-package minimal set
bundled in the Alpine RPi tarball (no CDN fallback) — none of
`alsa-lib`/`lilv-libs`/`serd-libs`/`sord-libs`/`sratom`/`zix-libs` are in it. Real
musl-aarch64 `.so` files live in `vendor/lib-aarch64/` (Alpine 3.20 CDN versions
matching CI) and are copied into `usr/lib/`.

**alsa-lib needs its DATA tree too (`/usr/share/alsa/alsa.conf`).** With
`libasound.so.2` vendored but no `alsa.conf`, `snd_pcm_open("default", ...)`
segfaults inside alsa-lib's config parser — `"default"` is an alias defined in
`alsa.conf`. The whole `vendor/share-alsa/` tree (~340K) is vendored rather than
guessing which `@hooks`/includes are load-bearing.

**`hostapd`/`dnsmasq` must be vendored as aarch64 binaries.** The Alpine RPi
tarball's local repo carries `iw` and `wpa_supplicant` but zero hostapd/dnsmasq
packages, and its `APKINDEX.tar.gz` is RSA-signed by Alpine so it cannot be
regenerated on this Windows host. Real aarch64 binaries live in
`vendor/sbin-aarch64/` (verified `e_machine=0xB7`) and are copied into `usr/sbin`.
`hostapd` additionally needs `libnl-3.so.200`/`libnl-genl-3.so.200`, vendored into
`vendor/lib-aarch64/` from the local repo's `libnl3` package.

**`dnsmasq` needs explicit `user=root`/`group=root` in
`src/net/config/dnsmasq.conf`.** Vendoring the binary does not create the
`dnsmasq` system user its package would; without this the daemon exits immediately
with `unknown user or group: dnsmasq` even though `dnsmasq --test` says the config
is fine.

**Every `cmdline.txt`/`extlinux.conf` APPEND write must stay a single line.** Pi
firmware and U-Boot both read only line 1; an embedded newline silently truncates
every kernel param after it (drops `isolcpus`, and for netboot
`ip=dhcp`/`alpine_repo`/`modloop`/`apkovl`, leaving the Pi in an emergency shell).
Every writer collapses both halves via `tr '\n' ' '` + `tr -s ' '` before emitting
one line with a single trailing newline —
`boot_tree_config`/`boot_tree_config_opi`/`build-netboot.sh`'s netboot-cmdline and
`NETBOOT_DEBUG` steps. `validate-image.sh`/`validate-netboot.sh` assert it by
counting newlines.

**Anything newly vendored needs adding to BOTH `tar --mode='+x'` lists.** NTFS
carries no Unix exec bit, so `chmod +x` in the overlay is a silent no-op on this
Windows host. `image/lib-boot-tree.sh` (`_exec_paths`, apkovl build) and
`image/build-netboot.sh` (`_nb_exec_paths`, netboot repack) each re-append every
executable path by name via `tar --mode='+x' -rf ...`. A file not named in those
lists ships `-rw-r--r--`.

Read modes from `tar -tvzf` archive listings, never from extracted files
(extraction on Windows loses the bit anyway). `opt/aloop/aloop` legitimately
appears twice (a `-rw-r--r--` entry then a `-rwxr-xr-x` one) because the `+x` pass
re-appends rather than overwrites — verifiers grep the LAST match. A single
`-rw-r--r--` entry with no later `-rwxr-xr-x` twin is the failure signature. Both
lists include `$(find usr/sbin -type f ...)`; `build-netboot.sh` verifies
hostapd/dnsmasq explicitly.

The `find` calls building these lists must run inside the overlay directory
(`cd "$OVL" && find usr/sbin ...` / `cd "$NBOVL" && find usr/sbin ...`), never
in the caller's own cwd — `find` against a path that doesn't exist relative to
the current directory returns empty with no error, silently dropping every
match from the `+x` re-append with zero visible failure anywhere in the
pipeline. This produced a real ticker-AP outage: `hostapd`/`dnsmasq` matched
correctly in `boot_tree_apkovl` (where a hardcoded `opt/aloop/aloop` path still
worked) but were silently excluded from the `find`-derived part of the list,
shipping non-executable while every other check passed.

## `core.autocrlf=true` on this Windows clone corrupts shell scripts

Editing or re-checking-out any `.sh`/`.start` file here can silently convert line
endings to CRLF. Alpine's busybox ash chokes on `#!/bin/sh\r` and every trailing
`\r` merges into the next token (`illegal option -`, `: not found`). This fails
with zero visible error anywhere in the pipeline — CI runs nothing, packaging just
copies bytes.

A repo-level `.gitattributes` forces `eol=lf` on
`*.sh *.start *.conf *.yml *.yaml Makefile cmdline.txt config.txt usercfg.txt`.
If a script behaves strangely on-device despite looking correct, check
`file path/to/script.sh` for "with CRLF line terminators"; fix via
`rm path/to/script.sh && git checkout -- path/to/script.sh`.

---

# Device runtime environment

## Alpine/musl/aarch64 — glibc/x86_64 artifacts silently fail to load

A `.so` built with the host's g++ (glibc/x86_64) dlopens on the device with no
bundle-discovery error, then fails at load time:
`Error relocating .../foo.so: unsupported relocation type 7`. CI green only ever
means "the x86_64 build compiled", never "the plugin runs on target".

Pattern (see `.github/workflows/build-lv2.yml`): split `faust2lv2`'s stages —
`faust -i -a lv2.cpp ...` emits a self-contained `.cpp` (only libc/libstdc++/lv2/
boost includes); a `$HOST_CXX` compile+run of that same `.cpp` emits the plugin's
`.ttl` metadata (host-only, never touches target arch/libc); only the final
`-shared .so` link targets the device. Cross-compile that one step in a real
Alpine aarch64 container via `docker/setup-qemu-action` +
`docker run --platform linux/arm64 alpine:3.20`, matching `build-binary.yml`.
Verify: `objdump -p foo.so | grep NEEDED` must show `libc.musl-aarch64.so.1`,
never `libc.so.6`.

**Pass `CPPFLAGS` into nested `docker run ... sh -c "..."` via `docker run -e
VAR="$VAR"`, never string interpolation.** Escaped quotes (e.g.
`-DPLUGIN_URI=\"...\"`) lose their escapes across the nested-shell boundary and
the compiler tries to parse the URL as code
(`'https' was not declared in this scope`).

## `actions/upload-artifact@v4` `path:` wildcard-vs-literal

`path: effects/home/*.lv2` (wildcard) zips the matched directory WITH its basename
preserved. `path: effects/home/guitar_lofi_fx.lv2` (literal single directory) zips
its CONTENTS flattened at the zip root, silently dropping the `.lv2/` wrapper.
Always use the wildcard form for LV2 bundle artifacts.

## `disable_core3_lv2` in `/etc/aloop.conf`

An uncommented `disable_core3_lv2 = 1` makes `audio_thread.cpp`'s worker skip
`homeFx.process()`/`userFx.process()` entirely every block, so `guitar_lofi_fx.lv2`
never runs its DSP at all — fully silent, fully inert, no error or warning. This is
a live-device-only state (the shipped `config/aloop.conf` only carries the line
commented out) that survives any number of `rc-service aloop restart`s. Always
`grep -n disable_core3_lv2 /etc/aloop.conf` (anchored to line-start, no leading
`#`) before debugging "guitar/lofi effects don't do anything" as a code bug.

---

# Deploy, netboot, SSH

## SSH: use a JS `ssh2` client, never Windows ssh.exe or sshpass

Password auth (root/aloop). Bare `ssh.exe` (prompts for a password) and
sshpass-wrapping are both explicitly rejected by the user. Use
`npm install ssh2` in the scratchpad plus a small script doing
`new Client().connect({host, port:22, username:'root', password:'aloop', ...})`.
A fresh netboot generates a new host key every boot, breaking raw `ssh`/
known_hosts but not `ssh2`.

**`sftp.fastGet`/`fastPut` fail with a real server-side `SSH_FX_NO_SUCH_FILE`
against this device's Alpine `internal-sftp`, even for a trivial known-good
local file and a path plain `exec` (`echo | tee`) can write seconds earlier.**
A raw `sftp.open(path, 'w', cb)` + `sftp.write(handle, ...)` sequence succeeds
on the FIRST call within a fresh SFTP session, but a second `open` in a new
connection (or `fastPut`'s own internal chunked-write pipelining) then fails
the same way — consistent with `internal-sftp` only tolerating one real
open-for-write per session cleanly, not a path or permissions issue. Proven
reliable fallback for pushing a file to the device: base64-encode locally,
append it in ~60KB chunks via repeated `exec("printf '%s' '<chunk>' >>
<path>.b64")` calls (same `ssh2` `Client.exec`, no SFTP subsystem involved),
then `exec("base64 -d <path>.b64 > <path> && rm <path>.b64")` once complete.
On Windows/Git-Bash, `/tmp/...`-style remote path arguments get silently
rewritten to a Windows path by MSYS's own path-conversion layer before ever
reaching `node` — prefix the command with `MSYS_NO_PATHCONV=1` or every write
target lands in `C:/Users/.../AppData/Local/Temp/...` instead of the device's
real `/tmp`.

## The `REBOOT:<token>` UDP listener lives INSIDE the aloop process

`config/aloop.conf`'s `[remote] token=` enables a `udp/4446` listener
(`src/control/remote_control.cpp`). If `aloop` has crashed, nothing is listening
and `image/aloop-reboot.js` silently does nothing — no error, no timeout.
`/etc/init.d/aloop`'s `respawn_max=0` means OpenRC will not restart a crashed
`aloop` either, so a crashed device stays crashed indefinitely.

If `rc-service aloop status` shows `crashed`, use
`node ssh-exec.js 192.168.137.100 "reboot"` instead. Only use the UDP REBOOT path
once `aloop` is confirmed running.

**Always verify a reboot actually happened before trusting any device-state
observation**: check `cat /proc/uptime` and `md5sum /opt/aloop/aloop` against the
binary just deployed, BEFORE reading logs. A stale device produces stale data that
looks like a fresh test.

## Netboot self-update: two rebuild paths

- **Automatic**: `image/serve-netboot-win.js` (run elevated, needs
  `GITHUB_TOKEN`/`gh auth token` and `PI_TOKEN`) polls `build-binary.yml`/
  `build-lv2.yml`'s latest green run on `main` every 30s, downloads both artifacts
  into `.netboot-update-work/{bin,lv2}`, and calls `image/build-netboot.sh` when
  the combined SHA changes. State lives in `.netboot-update-sha`
  (`<binSha>:<lv2Sha>`) — if it already matches, the poll loop does nothing ever,
  regardless of `.netboot-serve/`'s actual content.
- **Manual**: `ALOOP_BIN=<path> LV2_DIR=<path> OUT=.netboot-serve
  NETBOOT_SERVER=192.168.137.1 bash image/build-netboot.sh`.

**The automatic path's SHA-tracking is blind to changes in the packaging scripts
themselves** (`image/lib-boot-tree.sh`, `image/build-netboot.sh`) — neither
workflow lists `image/**` in its trigger `paths:`, so a packaging fix never
triggers a rebuild and a rebooting device silently picks the old image back up.
Any packaging-script change requires a manual `.netboot-serve/` rebuild.

**Verify the deployed checksum after every manual rebuild, BEFORE rebooting**:
`tar -xzf .netboot-serve/aloop.apkovl.tar.gz -C <fresh-empty-dir>
./opt/aloop/aloop && md5sum <fresh-empty-dir>/opt/aloop/aloop` vs the source
binary. Extracting to stdout (`-O`) or reusing a not-freshly-emptied directory can
compare against stale leftovers. A checksum match proves SERVER state only —
cross-check `/proc/uptime` for whether the device actually picked it up.

When bisecting with an old commit's binary, expect a crash if it predates the
nullptr-features fix and any LV2 bundle is present in `/effects/home` or
`/effects/user`.

## `build-netboot.sh` publish discipline

**Publish is a staged-directory atomic `mv`, never `rm -rf` + populate-in-place.**
`image/serve-netboot-win.js` can rebuild the netboot root while a Pi is actively
TFTP/HTTP-fetching from it; an in-place rebuild leaves a window where the served
tree is empty or half-copied. `mv` between two directories on the SAME filesystem
is a single atomic `rename(2)`, so the staging directory is built as a SIBLING of
the real output dir — never under `mktemp -d`'s `$WORK`, which typically lands on
a different mount and silently degrades the swap to copy+delete.

**The netboot root must be `chmod -R a+rX`'d after copy.** The Alpine tarball ships
`boot/initramfs-rpi` mode 600 and `cp -a` preserves it; an unprivileged TFTP server
(dnsmasq drops to `nobody`) then gets "Permission denied" and the Pi boots a kernel
with no initramfs, panicking "unable to mount root fs".

## Netboot silently outranks the SD card

Pi 4 firmware prefers network boot when a netboot server is reachable — a correctly
written SD card can look like a broken fix while the device fetches
`start4.elf`/kernel/initramfs over TFTP and the apkovl over HTTP from a stale
`.netboot-serve/`. Before trusting any on-device observation after an SD update,
confirm which path booted (`.netboot-serve.log` for fresh TFTP/HTTP lines) and
compare the running binary's md5 against the card's.

`serve-netboot-win.js` can also die while holding its `updateInFlight` guard,
freezing `.netboot-serve/` and `.netboot-update-sha` indefinitely — the `finally`
and `REBUILD_TIMEOUT_MS` bound only the child process, not a wedged async flow.

## Netboot DHCP diagnosis

**DHCP REQUESTs with ZERO TFTP reads = option 66 points at a dead address**, not a
competing DHCP server. `serve-netboot-win.js`'s `SERVER_IP` must be an address an
interface actually holds (Windows ICS may assign e.g. `192.168.137.101`, not
`.1`). Nothing answers on a dead `.1`, so the Pi ACKs, times out fetching, and
re-DISCOVERs forever.

Tell them apart before theorising: a REQUEST whose offered IP is the HOST's own
address is the host's ICS adapter renewing its own lease. `arp -a` prints the local
address as `Interface: <ip>`; `ping` replies `TTL=128` (Windows) vs `TTL=64`
(Linux). `os.networkInterfaces()` confirms whether the advertised address exists at
all. `resolveServerIp()` auto-detects the single live `192.168.137.0/24` address
and REFUSES an explicit `--server` no interface holds.

Related: the apkovl bakes the server IP at build time (`@NETBOOT_SERVER@`
substitution into `cmdline.txt`'s `alpine_repo`/`modloop`/`apkovl` URLs) — rebuild
with `NETBOOT_SERVER=<real ip>` whenever the host address changes. The DHCP pool
skips every reserved/local address so it can never hand out the host's own.

**DHCP DISCOVERs that never become REQUESTs = wrong egress interface.** No send
error appears (the OFFER sends fine, it just leaves via the wrong NIC). Cause: the
netboot NIC holding `192.168.137.1` with a **/16** mask while Wi-Fi holds a /24 in
the same range — Windows routes by longest prefix match, so replies to the
`192.168.137.255` directed broadcast go out Wi-Fi.

Diagnose: `Find-NetRoute -RemoteIPAddress 192.168.137.255`. If it names anything
but the Pi's NIC, that is the bug. Confirm with `Get-NetIPAddress -AddressFamily
IPv4` — `PrefixLength` must be `24`.

```
Remove-NetIPAddress -InterfaceAlias Ethernet -IPAddress 192.168.137.1 -Confirm:$false
New-NetIPAddress   -InterfaceAlias Ethernet -IPAddress 192.168.137.1 -PrefixLength 24
Set-NetIPInterface -InterfaceAlias Ethernet -InterfaceMetric 10
```

Re-run `Find-NetRoute` before restarting the server. The server resolves
`SERVER_IP` once at startup, so a running instance keeps serving the old address
(it logs `replies via <old-ip>` — the quickest way to spot a stale process).

`pkill -f serve-netboot-win` does not always reap the listener; the replacement
then fails with `[TFTP] bind EADDRINUSE 0.0.0.0:69` while silently falling back to
a different interface. Confirm ports are free
(`netstat -ano | grep -E ':(67|69|8080)\s'`) before concluding a restart took.

## Fast DSP-only iteration: `image/dsp-hotdeploy.js`

A pure `.dsp`/Faust edit does not need a full image assembly or reboot.
`image/dsp-hotdeploy.js` pushes a commit through CI's real musl/aarch64
cross-compile, SFTPs the changed artifact onto a live device, and restarts the
service over the same `ssh2` client.

`node image/dsp-hotdeploy.js --target home` (home-stack `.dsp` →
`aloop-aarch64-musl` → `/opt/aloop/aloop`), `--target guitar`
(`guitar_lofi_fx.dsp` → `guitar-lofi-fx-lv2` → `/effects/home/guitar_lofi_fx.lv2/`),
or `--target both`. Requires the edit already committed and pushed (it polls the
run that commit triggered via `gh run list`, it does not trigger one) and `gh`
authenticated. Fails loudly if the run's conclusion isn't `success` or if
`rc-service aloop status` doesn't report `started` afterward.

**It STOPS the service BEFORE overwriting `/opt/aloop/aloop`, not after.**
`sftp.fastPut` against a currently-executing binary's inode fails with a bare
`Failure` (musl/Alpine ETXTBSY). The sequence is stop → `fastPut` → start, never
`restart`-after-write.

Does NOT replace the netboot path for changes to
`image/lib-boot-tree.sh`/`image/build-netboot.sh`, kernel/cmdline config, or OpenRC
service files.

---

# Mesh networking

## aloop ↔ esp-idf-link paired invariants (change BOTH or the mesh splits)

aloop (Pi 4) and `../esp-idf-link` (ESP32, the "ticker" box) form ONE ad-hoc
single-AP mesh so Ableton Link's multicast peer discovery reaches every device. No
credential provisioning: exactly one device hosts the open SSID `ticker`, everyone
else joins as a station. Changing any value below in one project alone silently
stops meshing, with no error on either side.

| Invariant | aloop | esp-idf-link |
|---|---|---|
| Mesh SSID | `src/net/config/hostapd.conf` `ssid=ticker`, `wpa_supplicant.conf` `ssid="ticker"` | `main.cpp` `wifi_scan_best_bssid("ticker")` / `wifi_start_link_ap("ticker")` |
| Auth | open (`key_mgmt=NONE`; `wpa=` commented out) | `wifi_connect_sta("ticker", "")` |
| AP address / DHCP | `192.168.4.1/24`, dnsmasq `.2-.20` | `esp_netif_set_ip_info` `192.168.4.1/255.255.255.0` |
| Channel | `hostapd.conf` `channel=6` | SoftAP ch6 |
| Link multicast | Link's hardcoded `224.76.78.75:20808` | same (hardcoded in Link) |
| Link quantum | `link_bridge.cpp` `quantum = 16.0` | `main.h` `#define LINK_QUANTUM 16.0` |
| Start/stop sync | `link_bridge.cpp` `enableStartStopSync(true)` | `main.cpp` `g_link->enableStartStopSync(true)` |
| Host election | lowest MAC/BSSID wins | lowest MAC/BSSID wins |

`PHRASE_BEATS 64.0` in esp-idf-link is NOT the Link quantum — it is that project's
transport-correction/SPP boundary (16 bars), intentionally different, and does not
affect phase agreement.

**Host election is MAC-ordered, not "host if scan found nothing".** Two devices
cold-booting together can each scan before the other's AP exists, so a naive
"nothing found → host" makes both host, producing two isolated L2 domains Link can
never cross. Both projects hold for a duration strictly monotonic in their own MAC
(lowest ≈ 0s, highest ≈ 6s), rescanning every second and joining the instant a
peer's AP appears. A genuinely lone device hosts when its hold expires. Both
supervisors yield if another `ticker` AP with a strictly-lower BSSID appears — but
never while clients are attached.

## `src/net/autoap.sh` constraints

- Must host SSID `ticker`, never `aloop`.
- `wpa_supplicant.conf` must carry at least one active (non-commented)
  `network={}` block, or `known_net_available()` can never associate and the script
  always falls through to hosting.
- The AP-mode rescan pattern must not be built with a naive
  `grep -oE 'ssid="[^"]*"' wpa_supplicant.conf` — grep does not skip comments, so
  commented-out placeholder SSIDs end up in the pattern.
- **Keep this file POSIX-clean.** It is `#!/bin/sh` = busybox ash on Alpine;
  bashisms like `grep -qFf <(...)` are a hard syntax error
  (`Syntax error: "(" unexpected`). Check with `dash -n src/net/autoap.sh`.
- **`start_ap()` must clear a previous hostapd, not just wpa_supplicant.** A stale
  hostapd holding the interface produces `nl80211: kernel reports: Match already
  configured` then `Could not set channel for kernel driver` /
  `Interface initialization failed`. **The channel error is a red herring — ch6 is
  fine and is a paired invariant with esp-idf-link's `cfg.ap.channel = 6`; never
  "fix" this by changing the channel.** `start_ap` pkills dnsmasq+hostapd, waits
  (bounded, 20s) for exit, and logs loudly on failure.

`rc-service autoap status` reporting `started` is the real signal — a `crashed`
status with a plausible-looking `ip addr` (wlan0 at `192.168.4.1/24`) and no AP is
exactly what a broken AP looks like.

## Ableton Link integration checklist

- **Thread-correct session-state API.** `captureAppSessionState()` /
  `commitAppSessionState()` from non-audio threads;
  `captureAudioSessionState()`/`commitAudioSessionState()` from the audio thread
  ONLY. aloop calls only the App variants and hands the audio thread a lock-free
  double-buffered `LinkSnapshot` (ADR-005) — legitimate, but audio-side beat/phase
  is up to one control-tick stale; shortening that interval is the lever if phase
  accuracy is questioned.
- **`enableStartStopSync(true)` must be paired with both reading `isPlaying()` and
  calling `setIsPlaying()`.** aloop does both (`LinkSnapshot::isPlaying` +
  `LinkBridge::setTransportPlaying`, published on every play-state edge from
  `ApcGrid`). esp-idf-link only consumes, correctly — it has no local transport
  control and bridges to outgoing MIDI Start/Stop.
- **The three notification callbacks** (`setNumPeersCallback(std::size_t)`,
  `setTempoCallback(double)`, `setStartStopCallback(bool)`) are invoked on a
  Link-managed thread and are documented **Realtime-safe: no**. Bounded logging and
  atomics only: never allocate, never lock, never reach into the audio thread.
- **Tempo authority.** `setTempo` rewrites tempo for EVERY peer. aloop's
  `proposeTempo` refuses when peers are already present and aloop never set the
  tempo itself; esp-idf-link only sets tempo on an explicit LTMP command.
- **Quantum is a shared constant.** `kLinkQuantum` (`src/link/link_bridge.h`) and
  `LINK_QUANTUM` (esp-idf-link `main.h`) are both `16.0` and move together.
- **Telemetry carries peer count, not just a bool.** `status.json` has
  `link.peers` and `link.playing`; `synced` (peers>0) cannot distinguish 1 from 3.
- **Interface readiness is a real race.** Link opens its multicast socket during
  `enable()`. esp-idf-link waits 500ms before constructing Link and re-asserts IGMP
  membership for ~10s after every connection. aloop's equivalent is
  `depend() { after local autoap; ... }` plus a bounded `waitForNetworkInterface()`
  before `link.start()`.

## Ableton's Link Test Plan

`build/_deps/abletonlink-src/TEST-PLAN.md` is Ableton's official 12-case Link Test
Plan; Link's README names compliance with it as the bar, calling out "not hijacking
a jam's tempo when joining". Audit any Link change against it.

- **TEMPO-1..5** — tempo propagates both ways; joining must not change the
  session's tempo; enabling/disabling Link with no session must not change ours.
  `proposeTempo`'s authority guard satisfies TEMPO-2/3.
- **TEMPO-4 range is 20..999 bpm.** aloop's follow path
  (`linkSpeedRatio = recordedBpm / sessionBpm` into `effSpeed`, clamped 0.1..8.0 in
  `dsp/loop.dsp`) never saturates inside that range (saturation begins below
  ~15bpm). esp-idf-link is widened to 20..999 to match.
- **STARTSTOPSTATE-1/2** — must both listen and send. `publishTransport` sends on
  every play-state edge; `applyRemoteTransport` follows a peer's transport with a
  quantized start and an immediate stop. The ESP is listen-only by design.
- **BEATTIME-1/2** — no beat-time jump when enabling Link with no session; no
  discontinuity when a peer joins. Check against `cycleOffset`/`absPos` phase
  derivation.
- **AUDIOENGINE-1** — recorded audio onset must align with the session pulse within
  **3 ms**. Unverified here; interacts with the SHIFT-fold latency compensation (64
  samples = 1.333ms) and the one-control-tick staleness of the audio-thread
  snapshot. If it fails, the levers are a shorter publish interval or explicit
  output-latency compensation — never added buffering.

## Unproven: AP-mode multicast forwarding on the Pi

Whether Link's multicast crosses between the Pi's own AP and its associated
stations on Broadcom `brcmfmac` is UNVERIFIED. `ap_isolate=0` is set and may be
sufficient — but the ESP32's SoftAP needed a full unicast relay beyond isolation
(`wifi_config.cpp`'s `link_multicast_relay_task` re-emits each Link datagram to the
group, to the AP's own IP, and unicast to every associated station, preserving the
original source IP because Link needs it for direct peer connect). Do NOT port that
relay speculatively — confirm the gap on real hardware first
(`docs/LINK-MESH-TESTING.md` Tests 1-3). If real, the Linux-side fix is a
networking-layer daemon fanning out to the dnsmasq lease IPs alongside `hostapd`;
it does not require touching `link_bridge.cpp`.

---

# Audio thread and ALSA

## Two ALSA devices, never conflate them

`src/dsp/audio_thread.cpp`'s `worker()` opens two distinct PCM devices:

- **Instrument device** (default `hw:0,0`, e.g. M-Audio AIR 192|4) — the real
  tight-latency capture+playback path. `cap`/`play` are always this device, opened
  blocking, retried up to 30 times at 1s intervals if not yet plugged in.
- **OTG gadget** (`f_uac2`, `hw:UAC2Gadget,0`) — a best-effort MIRROR of the same
  processed output, opened NONBLOCK so a missing/non-streaming host can never stall
  or desync the real path. `-EAGAIN` on the OTG write is expected and silently
  skipped; any other negative return triggers a one-shot recover, and a permanently
  gone device has its errors silently absorbed. A failed OTG open at startup is
  silent-degrade-only.

## Instrument device is S32_LE — ALSA silently ignores a wrong format request

Class-compliant USB interfaces like the AIR 192|4 support only S32_LE (24-bit data
left-justified in a 32-bit word; `/proc/asound/card0/stream0` shows
"Format: S32_LE, Bits: 24") with no S16_LE fallback. Requesting
`SND_PCM_FORMAT_S16_LE` via `snd_pcm_hw_params_set_format` returns success while
the device negotiates S32_LE anyway — with 16-bit normalization (32768) on 32-bit
data, samples arrive ~65536x too large and produce loud static.

Buffer type is `int32_t`, normalization divisor is `2147483648.0f`, and the
negotiated format is read back via `snd_pcm_hw_params_get_format` and compared
against the request, warning loudly on mismatch. The OTG gadget mirror is a
genuinely separate S16_LE device (`f_uac2-gadget.sh` sets `c_ssize`/`p_ssize=2`) —
the two output paths need separate wire buffers in their own native formats, never
one shared buffer.

## Playback needs `start_threshold` lowered to one period

The hw_params default `start_threshold` for a PLAYBACK stream is the full
`buffer_size`. This block loop writes only one N-frame period per
`snd_pcm_writei()` then blocks on the next capture read, so the ring never reaches
a full buffer and playback stays in `PREPARED` forever while capture (which starts
on any available data) runs fine — the two streams desync and playback underruns on
every write. Fix: `snd_pcm_sw_params_set_start_threshold(pcm, sw, period)`.

## ALSA period/buffer sizing: 4 periods minimum

2 periods (256 frames, the ALSA minimum) produces hundreds of xruns within seconds
on the instrument-device PCM — too tight for a USB path, where each read/write
rides USB transfer-scheduling jitter on top of SCHED_FIFO jitter. 4 periods at the
real `block_size` N per period keeps the same latency granularity with enough slack
to absorb it. The OTG gadget mirror deliberately uses looser timing
(period = 4xN, buffer = 4x that) since it needs no tight latency.

## `f_uac2-gadget.sh`: `req_number` must be 4, not the kernel default 2

`req_number` is the f_uac2 driver's isochronous USB request queue depth, separate
from ALSA's `buffer_size`/`period_size`. The default of 2 silently caps ALSA's
negotiated `buffer_size` at 256 frames regardless of what `hw_params` requests,
producing hundreds of xruns/sec once the ALSA period is tightened to match
`block_size`. Raising it to 4 gives the gadget's own transfer queue the headroom
the ALSA-side sizing intends.

The gadget presents a STEREO wire (`c_chmask`/`p_chmask = 0x3`) — `wireCh` handling
in `audio_thread.cpp` averages capture L/R to mono for the Faust DSP and duplicates
the mono result onto both channels on playback, so the host sees a normal stereo
soundcard while the DSP stays mono internally. Runs at boot from `/etc/local.d`
after `libcomposite` loads. The kernel's `f_uac2` lays out isochronous microframes
correctly by construction, eliminating the buzz/crackle/-4608 corruption class the
bare-metal looper had to fix by hand (ADR-008).

## Flush-to-zero must be set explicitly on the audio thread

Denormal floats occur naturally in any decaying IIR filter/feedback loop (reverb
tails, delay feedback, envelope followers) and are 10-100x slower than
normal-range floats on both ARM and x86. There is no portable C++ API on ARM —
`setFlushToZero()` in `src/dsp/audio_thread.cpp` sets the AArch64 FPCR FZ bit via
inline assembly (`mrs`/`msr fpcr`) on `__aarch64__` and uses SSE intrinsics on
x86. Applied once at thread startup, before any DSP compute.

## `AloopLoopDsp` must be heap-allocated, never a stack-local

`sizeof(AloopLoopDsp)` is ~320 MiB (20 loopers x `MAXLEN=48000*60` rings each). As
a stack-local inside `worker()` it SIGSEGVs at the first local-variable stack write
— no pthread stack size can be large enough, since the frame is unmapped the moment
the stack pointer moves to make room. Use `std::make_unique<AloopLoopDsp>()` — a
one-time allocation at thread startup, never in the per-block RT hot path. The
`Sampler` (~5.3MB) is heap-allocated the same way.

Any standalone harness on Windows hits the same wall against the default 1MB thread
stack (`STATUS_STACK_OVERFLOW` before a single sample).

## Per-block hot path: resolve string-keyed lookups ONCE, never per block

Both the control WRITE path and the telemetry READ path must cache resolved
`(ParamStore slot, Faust zone float*)` pairs at thread startup, rebuilt only when
`ParamStore::count` grows. The established pattern is
`resolvedControls`/`sidechainSrcSlot`/`looperTelemetryZones[]`.

Doing this work per block (`targetToZone()`'s `std::string` + `snprintf`, then
`ParamStore::get` by name and `FaustUI::set`'s `zones.find` with an O(n) linear
suffix-scan fallback) costs scale directly with bound-control count and produced
`readi()` taking 2.2-2.7ms against a 1.333ms expectation, continuously, with xruns
climbing without bound at idle. The telemetry read side is 140
`snprintf`+`std::map::find` pairs per block (7 fields x 20 loopers), 750 blocks/sec.

Diagnostic signal for this class of bug: `/proc/<tid>/schedstat`'s
`sum_exec_runtime` showing the RT thread on-CPU ~95% of uptime with only ~46
voluntary context switches/sec, when a thread genuinely blocking once per `readi()`
at 750 blocks/sec should show ~750/sec. `/proc/<tid>/stat`'s `state` field should
read `S` (sleeping) between blocks, not `R`.

## `FaustUI` shim must register bargraph zones

`addHorizontalBargraph`/`addVerticalBargraph` in `src/dsp/audio_thread.cpp`'s
hand-written `FaustUI` shim must do `zones[full(l)] = z` like every other control
type. As no-ops, every `hbargraph()` zone (level/writeidx/wraplen for all 20
loopers) is never inserted, so every `fui.get()` on them misses the exact-match
`find` and falls through to a full O(n) linear suffix-scan — 45,000 wasted scans/sec
on the RT thread, and telemetry level/wraplen read back all-zero on a live looper.

## `targetToZone()` must have a case for every control target

A missing case falls through to `return ""` and the value silently never reaches
the Faust zone — C++ state updates, DSP never sees it, zero error output. `fx/bank`
was missing this way (a passthrough case, since `effects_runtime.dsp` declares
`nentry("fx/bank", ...)` under its literal name).

## `masterPhaseBuf` must ramp per-sample within the block

`dsp/loop.dsp`'s `absPos` formula treats `masterPhase` as this looper's actual
per-sample READ POSITION at `effSpeed==1.0`. Filling it via `std::fill()` with a
block-constant value (correct for genuinely block-constant commands like
`clearBuf`/`speedBuf`) freezes `readIdx0`/`readIdx1` within each block and jumps 64
samples at block boundaries — a stepped/aliased readback audibly indistinguishable
from bitcrushing. Fill as `masterPhaseBuf[i] = masterPhaseSamples + i`, wrapped at
`masterLen`.

The wrap uses a running accumulator (`p += 1.0`, conditional single subtract on
overflow) rather than per-sample `std::fmod`, guarded by a fallback to the exact
`fmod` formula when `masterLen < N` (a loop shorter than one block — impossible from
normal recording/quantization, but the accumulator would drift there). This fast
path is verified bit-exact against the `fmod` formula across `masterLen` 1..`MAXLEN`
and N 1..512.

## Recording must tap a dedicated post-fx Faust input, never fold post-fx into `fin`

Feeding a post-effects tap into `fin` (the live dry/input signal) makes it next
block's `dsp` input, flowing through `fx` again every block — stages reprocessing
their own prior output produce a fast aliased whine.

`loop.dsp`'s `process()` has a dedicated second input `prevFiltIn` that ONLY the
record-capture term consumes (`record = prevFiltIn * recN`), so it structurally
cannot re-enter `fx`. `audio_thread.cpp` feeds it from `prevFiltOut`, a snapshot of
the previous block's fully-effected mix (`rawFiltTap`, one of `aloop.dsp`'s
outputs), always the full effects chain regardless of SHIFT state — recording must
always capture the fully-effected signal, not raw pre-fx input.

`prevFiltOut` already contains post-glitch content one block later since
`microStage` is upstream of `filterStage` in `effects_runtime.dsp`, so no separate
glitch tap is needed.

## Sampler capture taps `prevFiltOut`, same one-block-lag discipline

Sample recording must capture loop content AFTER the whole effects chain
(pitch/glitch/filter/delay/reverb) — the same fully-effected signal loopers record
from. `captureBlock` reads `prevFiltOut`, never `fin` and never a pre-fx snapshot.

Historical constraint that still applies to any future pre-mix tap: `fin[]` after
`renderInto()` contains this block's own sampler-playback voices, so capturing from
it would let a sample record itself while another sample/drum hit plays. Any tap
that needs dry-input-plus-loop-content-minus-sampler-playback must be a
structurally separate buffer snapshotted before `renderInto()`, not the same buffer
read at two times.

## SHIFT (`fx/monitorfold`) native fold mechanism

`worker()` does `fin[i] += prevLoopSum[i] * combinedFold` whenever
`fx/monitorfold` is engaged, ramping `foldGain` at `kFoldStep` (1/16 per block) —
`prevLoopSum` is always exactly one block (`g_cfg.blockSize`, 64 by default) behind
the live signal. `kFoldStep/N` is hoisted out of the per-sample ramp loop (both
operands are block-constant).

**`foldTarget` also depends on whether any transpose voice is gated**:
`foldTarget = (shiftHeldNow && !anyXposeVoiceGatedNow) ? 1.0f : 0.0f`, checking
`xposeGateSlot[v]` (resolved once at thread startup) each block. Without this, a
SHIFT+held-key pitch-lock plays the RAW unshifted loop (folded into `fin`, through
`fxOuts` at original pitch) simultaneously with the locked wet bus — audibly
indistinguishable from "the lock isn't working". Plain SHIFT-hold with no voice
pressed is unchanged. `glitchFoldGain`/glitch-hold is a separate, untouched
gesture.

## SHIFT-hold recording latency compensation

SHIFT's fold adds one block of lag into what gets captured, on top of the baseline
pipeline lag, so a take recorded with SHIFT held sits audibly late relative to
other loops.

`dsp/loop.dsp`'s `latencyBiasN = hslider("latencybias", 0, -MAXLEN, MAXLEN, 1)` is
subtracted from `masterPhase` at the instant `recordStartPhaseOffset` latches
(`recordStartPhaseOffsetStep(prev) = ba.if(finishEdge, masterPhase - latencyBiasN,
prev)`). A smaller `recordStartPhaseOffset` makes
`absPos = wrapAbs(masterPhase - recordStartPhaseOffset + cycleOffset, wrapLen)`
larger, so playback reads further ahead and catches the content up.

`applyRecPlayCycle` writes this at FINISH: `kShiftFoldBlockLatencySamples` (64) if
`m_looperShiftHeldDuringTake[looper]` was ever set during the take, else 0. The
flag is sampled every `pollHolds` tick against `fx/monitorfold` (not only at
ARM/FINISH) and reset to false at ARM.

This gates on `fx/monitorfold` (the SHIFT-fold gesture), NOT
`fx/pitchbend_engaged` (the SNAC pitch/varispeed engine, a genuinely distinct
control).

---

# Faust DSP

## `par()`-replicated UI controls silently duplicate — use signal inputs

A `button()`/`hslider()` inside a function that `par()` instantiates N times gets
RE-ELABORATED (UI declaration included) at each of the N call sites — **even when
the declaration text is hoisted outside the `par`/`vgroup` and passed in as a
parameter.** This produces N duplicate zones, so writing "the" zone only affects one
of them. Verify against the generated C++ (`build/loop.cpp`:
`grep -c '"speed"'` must be 1, not 20), never against how the `.dsp` source reads.

Fix: thread the value as a plain **signal input** to `process()` — a wire, nothing
to re-elaborate. `dsp/loop.dsp`'s `oneLooper` takes
`clearAll`/`speedMul`/`masterPhase`/`masterLen`/`sidechainEnv` this way;
`audio_thread.cpp` fills each buffer per block (`std::fill` for block-constant
values, a real ramp for `masterPhase`).

Genuinely per-looper values that only change once per take (`finishtarget`,
`latencybias`) are correct as `par()`-replicated hsliders — 20 distinct instances is
the intent there.

Momentary/held UI state is threaded as signal inputs by convention even in files
that are imported once and never `par()`-replicated (`multitranspose.dsp`'s
`note`/`gate`/`free`), so nobody has to rediscover this if the stage is later
wrapped in a `par()`.

## Faust has no runtime branching

`select2`/`ba.if` choose among ALREADY-COMPUTED signals; they do not skip computing
them. There is no in-Faust way to skip a stage's cost based on its own runtime
amount being zero. Any "gate this expensive stage when its amount is 0" idea must
be solved at the topology level (move the stage to another core / another plugin),
not with a selector.

This is why `effects_runtime.dsp` has no `fx/bank` 3-way crossfade: computing all 3
effect chains every block cost a real ~7pp `core_busy` regression with continuous
dropouts. `effects_runtime.dsp` is the dub-only chain; Guitar and Lofi-Fx live in a
permanent Core-3 LV2 bundle (`guitar_lofi_fx.dsp`), always active, never gated.
`ApcGrid`'s 3-bank fx surface (`onDubFxPress`/`onGuitarFxPress`/`onLofiFxPress`) is
pure UI state — a bank press only flips which knob-target table the next CC reaches
(`m_activeBank`) and starts an LED flash; nothing is re-pushed to Faust.

## Faust direct function-call syntax substitutes whole expressions, not buses

`f(loop(...), a, b)` binds the ENTIRE multi-wire `loop(...)` output to `f`'s FIRST
formal parameter (function application is closer to textual substitution than a
wire-count splice) — symptom is `too much arguments : 2, instead of : 1` deep inside
the callee. `:`-based composition DOES wire positionally by wire count. Correct
idiom: build the bus with `,` then pipe with `:` —
`(loop(...), s0,g0,...,s5,g5) : mixAndFx`, and inside `mixAndFx`,
`(dry, s0,g0,...,s5,g5) : fx`.

A named signal binding referenced multiple times (`fxBus : _,!` / `fxBus : !,_`) is
a single shared computation, not a duplication — this is NOT the `par()` bug class.

## Faust stdlib functions can hide oversized buffers

`ef.transpose` (`misceffects.lib`) has a HARDCODED `maxDelay = 65536` inside the
library function, completely independent of the window argument the call site
passes. With 6 polyphonic voices × 2 `de.fdelay` calls each, that was 3.0MB of
RT-thread memory for a working set that never exceeds ~1920 samples per tap.

`multitranspose.dsp` therefore defines its own local `xpose` — the identical
algorithm with `maxDelay = 4096` (`2 * (0.02 * 96000)` rounded up to a power of 2,
double the project's 48kHz rate for margin). Verified bit-exact against
`ef.transpose` across window sizes 64..1920 and shifts -48..+48 semitones, and
measured ~2.0% relative DSP CPU reduction (12.22% → 11.98%, `faust2bench`, 20 runs,
`-bs 64`) — cache locality, not instruction count.

When auditing a stdlib call for cost, read its real definition
(`/usr/share/faust/*.lib`). `an.pitchTracker` was checked the same way and is clean
(a zero-crossing-rate detector from `fi.highpass`/`fi.lowpass`, no table or delay
line). `flanger.dsp` (`de.fdelay`, `MAXD=4096`) and `flutter.dsp` (`MAXD=1024`) size
their own lines correctly.

## Faust compiler flags — currently shipped

**`-vec -fun -dfs -vs 32 -nvi -ct 0`** at every real `faust` invocation site
(`build-local.sh`, `build-binary.yml`'s `loop.cpp` codegen, both jobs in
`build-lv2.yml`).

- `-vec -fun -dfs -vs 32 -nvi` are pure codegen-strategy flags (vectorized codegen,
  function inlining, depth-first scheduling, no-virtual C++ backend — Faust's docs
  call `-nvi` "especially useful in embedded devices context").
- `-ct 0` (disable table range-checking) is safe here because every `rwtable` index
  in this codebase is software-bounded already: `dsp/loop.dsp`'s
  `readIdx0`/`readIdx1` are always `... % wrapLen` with `writeIdx` clamped to
  `MAXLEN-1`; `microrepeat.dsp`'s `wpos`/`rpos` are clamped against
  `MR_MAX`/modulo'd against `sliceLen`. `guitar_lofi_fx.dsp` has no tables at all.
  **Any new `rwtable` needs its own explicit index-bound trace before this flag
  stays valid.**

**`-mcpu=cortex-a72`** at every real target-compile step (`src/CMakeLists.txt`,
both LV2 `.so` link steps in `build-lv2.yml`). Deliberately NOT on the two
native-host `.ttl`-metadata `g++` compiles in `build-lv2.yml` — those run on the CI
runner's x86_64.

**`-O3`** for the aloop binary (`src/CMakeLists.txt`), matching the LV2 `.so`
builds. `-O3`'s extra passes over `-O2` are behavior-preserving (`-ftree-vectorize`,
loop unswitching/distribution/peeling, predictive commoning) — no `-Ofast`, no
`-march=native`, no fast-math.

Reference benchmark (`faust2bench`, `dsp/aloop.dsp`, `-bs 64`, x86_64 CI host):
baseline no-flags ≈ 4.27% DSP CPU; with the shipped Faust flag set ≈ 4.12%. Current
full-stack figure is ≈12.0% — the difference is the real cost of the polyphonic
pitch-lock engine added since, not a regression.

## Faust compiler flags — deliberately NOT shipped

- **`-mapp`** — causes a 100%-reproducible SIGSEGV (`si_addr=0x0`) inside
  `AloopLoopDsp::compute()` on real aarch64 with real audio, despite a synthetic
  x86_64 A/B showing byte-identical output. Never pass it at any of the 3 real
  invocation sites. Re-adding it requires a fresh live Pi 4 test with real audio.
- **`-fm def`** — broader than `-mapp` (sin/cos/tan/atan/exp/log/pow/sqrt
  approximations, touching `filters.dsp`'s `tan()`, `reverb.dsp`,
  `compressor.dsp`'s `exp()`/`log10()`/`pow()`, `pitch.dsp`'s `pow()`). It emits
  calls to `fast_tanf`/`fast_powf`/etc. from `faust/dsp/fastmath.cpp` — an
  architecture file meant to be compiled alongside `-lang cpp` output, not baked
  into `libfaust` — so under the LLVM JIT nothing resolves those symbols and it
  segfaults at `render()` on every real-usage case while `compile()` reports
  success. Needs a real link-time proof, not a numeric diff, before ever shipping.
  See `test/faust-flags/README.md`.
- **`ba.tabulate` for `filters.dsp`'s `pow(1000.0, cutoff)` or `pitch.dsp`'s
  `pow(2.0, SEMIS/12.0)`** — both files claim exact-port/bit-identical hardware
  parity; tabulation is inherently approximate, and `pitch.dsp`'s dominant
  per-sample cost is the `dubfx_pitch_tick` ffunction call anyway. Any future push
  must first prove the tabulation error is inside that file's parity tolerance.
- **Splitting the home Faust stack or the Core-3 bundle into per-effect LV2
  bundles** — multiplies per-plugin dispatch (`findDescriptor`/`connect_port`/
  `instantiate`) on the RT block path and gives up the single-compile-unit
  maintainability the home stack was designed around.
- **Faust's `-omp`/`-sch` internal work-stealing scheduler** — aloop already pins
  one Faust program per physical core via `pthread_setaffinity_np` (home stack Core
  1, guitar+lofi-fx Core 3); layering Faust's scheduler on top fights that.
- **`-mcd`/`-dlt`** — govern only `de.delay`-family codegen, not `rwtable`.
  `dsp/loop.dsp`'s and `microrepeat.dsp`'s rings are `rwtable`, so unaffected. Only
  `delay.dsp` (`de.delay(MAXD=96000,...)`) and `reverb.dsp` (`de.delay(8192,L)`)
  use delay-line codegen and both are comfortably above the default `-mcd 16`.
- **`-clang`** — emits `#pragma clang loop vectorize(...)` pragmas; every real
  target compile here uses gcc/g++, which ignores them.
- **`-mem`** — for embedded targets with genuinely separate memory banks; the Pi 4
  is a normal Linux process with one unified heap.

## Parameter smoothing order is deliberate

`effects_runtime.dsp`'s `filterStage`/`delayStage`/`reverbStage`/`pitchStage` take
raw `hslider` values straight into `pow()`/`exp()`-bearing math with no `si.smoo`
upstream. This is intentional: `effects/home/faust/param_mapping.md` documents the
audio path as verified against per-render-constant normalized CC values with an
all-defaults byte-exact passthrough, and `filters.dsp`'s header states params are
per-render constants matching the looper's per-block piecewise-constant behavior.
Adding `si.smoo` would change transient response and break that parity guarantee.

## `bitcrush.dsp` `BITS_MAX` is 24, not 16

At `BITCRUSHAMT=0`, `BITS_MAX=16` quantizes to 16-bit resolution unconditionally —
~500x the float32 rounding floor, so the documented all-defaults byte-exact
passthrough was false (measured 1.529e-05 vs the ~3e-8 float32 floor every other
stage sits at). The real production path never round-trips through int16 (the
instrument device negotiates S32_LE/24-bit), so this was a real always-on precision
floor. `BITS_MAX=24` brings the amt=0 diff to 8.94e-08 while leaving the crushed
extreme (`BITCRUSHAMT=1`, `BITS_MIN=2`) numerically identical.

## `delay.dsp` slew recursion must have no additive drift term

`curStep(target, c) = c + (target - c)*SLEW` with `SLEW=0.0001`. A `+ 1.0` term in
this recursion (which a prior version had, mistaking a bookkeeping tautology in the
C++ reference's comment for a required correction) has fixed point
`c* = target + 1.0/SLEW` — i.e. `target + 10000` samples, a hidden constant ~208ms
floor under EVERY TIME setting, dominating the whole low end of the range.

The real C++ reference (`apcEffectsProcessor::processSends`) has no `+1`:
`newDelay = curLen + (target-curLen)*0.0001`, a plain one-pole slew. The one-sample
lag between the `letrec` state and the read tap comes from
`len[n] = newDelayFrom(target, cd[n])` (so `cd[n+1] == len[n]`), not from an
additive term.

`MIN_DELAY_MS = 1000.0/SR` (exactly 1 sample, 0.0208ms @ 48kHz) — TIME=0 maps to
the structural `max(1.0, ...)` floor `targetSamples` already enforces. TIME=1 maps
to ~1000ms. The sweep is linear and monotonic:
TIME 0/0.1/0.25/0.5/0.75/1.0 → 0.02/100.0/249.9/499.8/749.6/999.6 ms.

When measuring anything in this file, warm the ring up ~90000 samples first —
`de.fdelay`'s internal recursive state needs settling, so a cold-start measurement
reflects transient, not steady state.

## `multitranspose.dsp`: polyphonic pitch-LOCK, 6 voices

`effects/home/faust/multitranspose.dsp` is an NVOICES=6 polyphonic pitch-LOCK
stage (Digitech Whammy / Infected Mushroom Manipulator behavior): the output lands
on the exact held key regardless of what pitch is actually being played. It is
strictly additive with the existing mono SNAC engine (`fx/pitchbend`, CC52/mod
wheel, `effects/home/faust/pitch_ffi.h`), which is untouched and remains the mono
"pedal ride" lane.

**Mechanism**: `an.pitchTracker` runs on the live input ONCE per sample (one shared
instance, computed in `process`'s `with{}` and threaded down as a parameter, never
per-voice), converted to a MIDI note via `ba.hz2midikey`. Each voice's shift is
`(targetNote - detectedNote)`, glided via `si.smooth(tau2pole(glide_ms))`, gated by
`en.adsr`, and shifted by the local `xpose` (see the stdlib-buffer entry above).
`fx/xpose%d/note` carries an ABSOLUTE MIDI note target, not a relative interval.

**The transpose window must be pitch-synchronous, not fixed.** A fixed 10ms window
mistracks badly at large shift ratios (a +22-semitone lock landed ~114 cents flat;
a note-on-after-silence case mistracked by -4.69 semitones) — `xpose` is a
crossfaded delay-line shifter, not a period-locked-splice engine. Sizing the window
from the detected period (capped at 20ms) brings every case within 0.00-0.02
semitones and is also a click-safety improvement (rapid-retrigger max
sample-to-sample jump 0.23 vs 1.58 fixed-window).

**`windowFor`'s `si.smooth(...) : max(64)` double-clamp is mandatory**: smooth
BEFORE truncating to int, then re-floor. The smoother's ramp-up from a
zero-initialized register can pass through a near-zero window value, and the
shifter's internal `fmod(_, w)` on a near-zero `w` poisons its recursive delay
state with NaN permanently.

**Gain staging: fixed per-voice gain (0.6) plus `ma.tanh` soft-clip on the summed
bus.** Never a dynamic `1/sqrt(activeVoices)` renormalization — that makes the
overall harmony bus level jump every time a chord note releases (pumping). `ma.tanh`
is static and level-independent. 6 voices against a 0.95-peak input stay at 0.995
max abs.

**Settings**: glide 8ms, ADSR 3ms attack / 30ms decay / sustain 1 / 50ms release,
crossfade 50%. The 50ms release is the verified click-free value.

**Voice allocation** is a round-robin/oldest-steal allocator in `ApcGrid`
(`allocateTransposeVoice`/`releaseTransposeVoice`,
`m_transposeVoiceNote[kTransposeVoices]`): a held note reuses its own slot if
replayed, an unheld slot is preferred, and once all 6 are held the oldest-triggered
voice is stolen (its ADSR re-attacks, same as a real synth voice-steal).
`onKeybedNoteOff` releases by GATE only (`fx/xpose{v}/gate=0`), never a hard cut.
`onLiveEngageToggle` and `onClearAll` both release every held voice.

## SHIFT routing through the transpose engine

`free` (a signal input fed from `fx/monitorfold`, `audio_thread.cpp`'s
`freeXposeBuf`, `fins[20]`) crossfades BOTH which signal feeds the tracker/shifter
and which output carries the wet result, on one shared smoothed gate:

```
sigIn   = dry*(1-freeSmooth) + loopSum*freeSmooth
dryWet  = wet*(1-freeSmooth)
loopWet = wet*freeSmooth
```

At `free=0` this is exactly the dry-input pitch-lock (`loopWet=0`); at `free=1` the
whole engine — tracking and shifting — is redirected onto `loopSum` (`dryWet=0`), so
SHIFT+chord becomes a live "Whammy-on-the-loop" gesture. Sharing one `freeSmooth`
for both the source blend and the output split keeps a mid-transition sample
proportionally correct. Never add a second voice bank or a second `an.pitchTracker`
for the loop path.

`effects_runtime.dsp`'s `process` takes `loopSum` and returns TWO outputs
(`mainOut`, `loopHarmonyWet`). `loopHarmonyWet` deliberately does NOT run through
`microStage:filterStage:delayStage:reverbStage` — `loopSum` already bypasses those
on the direct-playback path. `aloop.dsp`'s `mixAndFx` splits the bus via
`fxBus : _,!` / `fxBus : !,_`; no new top-level `process()` input is needed.

## Locked pitch must REPLACE, never layer over, the original

Two complementary gates, both required:

**Dry side** (`effects_runtime.dsp`):
`dryGate = (1.0 - min(1.0,g0+g1+g2+g3+g4+g5)*(1.0-freeXpose)) : si.smoo` multiplies
`pitchStage(dry)`'s contribution. Without it, `pitchStage`'s own passthrough (SNAC
disengaged is a bare `dry` passthrough) sums under the locked wet voices and the
original pitch is always audible — a harmonizer, not a lock. It fades to ~0 when any
voice is gated AND `freeXpose` is 0, and stays at 1 when no voice is held OR
`freeXpose` is 1.

**Loop side** (`dsp/aloop.dsp`): ONE gate, never two independently-paced ones.

```
loopDirectRaw  = 1.0 - max(max(monitorFold, glitchFold), anyVoiceGated*freeXpose)
loopDirectGate = loopDirectRaw : si.smoo
filtOut        = fxOuts + loopSum*loopDirectGate + loopHarmonyWet
```

`monitorFold`/`glitchFold` are plain hsliders here with NO individual `si.smoo` —
the single downstream `si.smoo` provides the click-safe ramp, and they are already
ramped natively by `foldGain` in `audio_thread.cpp`.

The earlier two-term form (`directFoldSuppress = (1-monitorFold)*(1-glitchFold)`
multiplied by a separately-smoothed `loopDirectGate`) is a hard bug: when a voice
gates while SHIFT is held, one term rises away from its suppressed 0 while the other
falls toward 0, and their PRODUCT humps mid-transition (peaks ~0.15 at +30ms,
doesn't decay below 0.02 until ~110ms) — a real audible window of raw loop content
on EVERY note-gate, 3x the raw-loop energy under rapid retrigger. With
`anyVoiceGated*freeXpose` pinned at 1 for the whole hold, `max(...)` stays pinned and
`loopDirectGate` falls monotonically with nothing to race.

Also required for the loop side: the native `foldTarget` fix (see "SHIFT
(`fx/monitorfold`) native fold mechanism" above) — the Faust gate alone does not
close the `fin[]` fold pathway.

## Glitch/microrepeat slice length

`effects/home/faust/microrepeat.dsp`'s `sliceBlocks = max(1, int(beatBlocks /
divSafe)) * 2` with `divSafe = max(1, DIV)`. Every one of the 5 divisions
(`apc_grid.cpp`'s `div[5] = {1, 2, 4, 8, 16}`, notes 82-86) produces a slice as long
as the next-widest step used to, including `div=1`, which doubles past its own old
value — there is no floor.

**Never implement this by halving the divisor before the division**
(`divSafe = max(1, int(DIV / 2))`): `int(1/2)` floors to 0 → clamped to 1, colliding
`div=1` and `div=2` into the same slice length. Multiplying the already-computed
slice length can never collide that way. The note-to-div table in `apc_grid.cpp` is
untouched — only slice length changes, not which pad triggers which step.

## `dsp/loop.dsp` varispeed must have NO deadzone

`varispeedActive = effSpeed != 1.0` — an exact-equality check, not a float epsilon
band. `g_manualSpeedMul`/`linkSpeedRatio` both stay bit-exact `1.0f` in the genuine
no-tempo-signal case and `audio_thread.cpp` never perturbs them, so this correctly
distinguishes "no mismatch" from "a real, if tiny, mismatch".

A deadzone (e.g. `(effSpeed < 0.999) | (effSpeed > 1.001)`, or any hysteresis band)
locks `effSpeed` to a flat 1.0 read inside the band, discarding a real tempo
mismatch. `absPos` is only correct when a looper's `wrapLen` genuinely divides
evenly into the masterPhase cycle at the CURRENT session tempo, so a discarded
mismatch desyncs loopers of different lengths by different fractional amounts —
a small PERMANENT phase offset between them. Symptom: steady (non-sweeping)
phasing/comb-filtering with 2+ loopers and a Link peer at a close-but-not-identical
tempo; nudging the tempo clearly away fixes it; a single looper never shows it.

## Dead files

`effects/home/faust/mixbus.dsp` was removed (zero consumers anywhere including CI).
Its final `(ival*THRU + oval*LOOP*GATE) * MIX` hard-clip is already covered by
`audio_thread.cpp`'s real int32 write path
(`s32 = v32 > INT32_MAX ? INT32_MAX : (v32 < INT32_MIN ? INT32_MIN : v32)` before
`snd_pcm_writei`), at finer precision than that file's stale s16-domain math.

`chain.dsp` is NOT dead despite not being imported by the live chain —
`build-lv2.yml`'s `home-fx-lv2` job builds it as a packaging-reproducibility check.

`rawGlitchTap` was removed from `effects_runtime.dsp`/`aloop.dsp`/`audio_thread.cpp`
(`fouts[4]` → `fouts[3]`) as a confirmed-dead output.

## `pitch.dsp` re-applies its params every sample instead of calling them separately

`pitchTick = ffunction(float dubfx_pitch_tick(float, float, float, float), ...)` takes
the sample AND `scale`/`FORMANT`/`ENGAGED` together in one call
(`process = _, scale, FORMANT, ENGAGED : pitchTick`) rather than a separate
params-only ffunction call before the per-sample one, even though
`dubfx_pitch_tick` applies params idempotently on every call. A separate
params-only call site would receive only compile-time-constant-shaped inputs on
some paths and Faust would constant-fold it away, silently freezing the engine's
params at their first-seen value. Riding the params in on the same call as the
sample forces every call to be genuinely per-sample.

This is why the stage is bit-identical to `../looper`'s `EngineSoladSnac`: the
pitched sample is produced by that real C++ engine via the ffunction bridge, not
a Faust-side approximation of it.

## DawDreamer verification harness

Numeric/behavioral verification of `.dsp` changes uses
[DawDreamer](https://github.com/DBraun/DawDreamer)'s `FaustProcessor` — a real Linux
`libfaust` LLVM JIT with a `compile_flags` passthrough (`pip install dawdreamer`).
`test/faust-flags/` is the committed example.

Known limits:

- **The JIT refuses to link `ffunction`-declared external symbols**
  (`calling foreign function 'dubfx_pitch_tick' is not allowed in this compilation
  mode`). Harnesses that need `effects_runtime.dsp`/`aloop.dsp` stub `pitch.dsp` to
  a bare passthrough; `pitch_ffi.h` itself is never touched.
- **`FaustProcessor`'s parameter list is alphabetical, not declaration-order** —
  match hsliders by name, not raw index, when using `set_parameter`.
- **Faust constant-folds `tan()`/`pow()` of a literal at compile time.** Sweeping a
  value pinned as a Faust constant tests nothing; use real runtime `hslider`s
  matching how production wires the control.
- **Warm delay-line-bearing files ~90000 samples before measuring.**

`faust2bench` (real Linux host; its bundled `bench.cpp` needs `pwd.h`, unavailable
under MinGW) is the CPU-measurement counterpart, used by `build-binary.yml`'s
"Benchmark CPU usage" step. Standard invocation for comparisons here: 20 runs,
`-bs 64`, real shipped Faust flags, isolated via a `git stash`/rebuild A/B on the
same tree.

---

# LV2 hosting

## Never pass a bare `nullptr` for the features array

`Lv2Host::instantiate()` (`src/host/lv2_host.cpp`) must pass a real,
NULL-TERMINATED `LV2_Feature* const*` to `d->instantiate(...)`. Faust's generated
`lv2.cpp` does `for (int i = 0; features[i]; i++)` with no null-check on `features`
itself, so a bare `nullptr` derefs at `features[0]` — SIGSEGV on every plugin load.
Use `static const LV2_Feature* const kNoFeatures[] = { nullptr };`.

Wrap `instantiate()`/`activate()` in the same sigsetjmp crash-isolation watchdog
`runOne()` uses (ADR-002) — a plugin crashing during LOAD is as untrusted as one
crashing during `run()`.

## `readTtl()`'s bundle match must strip trailing slashes

lilv's resolved bundle path carries a trailing slash the passed-in `bundlePath`
never has, so a raw `bundlePath.find(bpath) == 0` prefix comparison silently and
permanently fails even for well-formed bundles. It then takes the no-port-wiring
`.so`-only fallback (logging `lilv found no plugin matching bundle`), whose first
`run()` dereferences unconnected port pointers and gets the plugin disabled by the
crash watchdog on every startup. Strip trailing slashes from both sides and compare
for exact equality.

## `setControl` must match Faust's MANGLED LV2 port symbol

Faust's `lv2.cpp` architecture (`mangle()`, in the Faust install's
`share/faust/lv2.cpp`) never emits a control's raw Faust label as the LV2
`lv2:symbol`: it replaces every non-alnum/non-underscore character (including `/`)
with `_`, then appends `"_<portIndex>"` (declaration-order index).
`hslider("fx2/FLANGEAMT", ...)` becomes `fx2_FLANGEAMT_3`.

`Lv2Host::setControl` matches by MANGLED-LABEL PREFIX
(`mangleFaustLabel(rawLabel) + "_"` as a string prefix against each port's real
symbol), so `apc_grid.cpp`'s target tables can keep the natural raw Faust labels
without hardcoding fragile per-build port indices. An exact-symbol match silently
matches nothing, permanently, with zero error output anywhere.

**Verify any new LV2-hosted Faust control target against the deployed bundle's own
`.ttl`** (`grep lv2:symbol guitar_lofi_fx.ttl`), never assume it equals the raw
`hslider()`/`button()` label.

## `Lv2Plugin::descriptor` is cached at `instantiate()` time

Not re-resolved via `dlsym` + URI-matching linear scan on every `runOne()` call —
that call runs on the RT block path (Core 1 home-fx, Core 3 user-fx) every block.

## `aloop.lv2` must be excluded from apkovl packaging

`build-lv2.yml`'s `home-fx-lv2` job compiles `dsp/aloop.dsp` — the exact Faust
source `audio_thread.cpp`'s `faustHome` already compiles natively and runs every
block — into a standalone LV2 bundle purely as a CI reproducibility/packaging check
(ADR-003). Deployed and loaded alongside `guitar_lofi_fx.lv2` it runs the whole home
stack a second time: `core_busy` jumps from ~23-30% to ~63-65% with xruns climbing
continuously. `image/lib-boot-tree.sh`'s copy step excludes it by name.

## Resonode is a separate, conditionally-called LV2 bundle, not part of the always-on home stack

`effects/home/faust/resonode_synth.dsp` used to live inside `effects_runtime.dsp`
(the always-on Core-1 Faust program) as a `component()`, driven by per-voice
note/gate/vel Faust *signal inputs*. WITNESSED real-hardware regression: even a
reduced 4-voice x 2-mode shape produced a sustained ~2ms `readi` gap (vs the
1.333ms block budget) with Resonode fully idle/unengaged — Faust has no runtime
branching to skip a stage's cost (see "Faust has no runtime branching" above),
so the whole mode bank computed every block regardless of whether the LofiFx
hold-gesture had ever engaged it.

Fixed by pulling it out entirely into its own standalone LV2 bundle
(`resonode.lv2`, `build-lv2.yml`'s `resonode-lv2` job, mirroring
`guitar-lofi-fx-lv2`'s own build exactly), loaded via a DEDICATED
`Lv2Host resonodeFx` in `audio_thread.cpp` from `AudioConfig::resonodeDir`
(`/effects/resonode`, never `homeDir`/`userDir` — `lib-boot-tree.sh`'s home-FX
`find` explicitly excludes `resonode.lv2` by name so the general
`homeFx`/`userFx` hosts never load it). `resonodeFx.process()` is only ever
called when `fx/resonode/engaged` reads true — the genuine cost elimination
this whole change was for, verifiable at the C++ call site rather than trusted
to a Faust-side crossfade.

**Per-voice note/gate/vel became LV2 control ports (`hslider`), not signal
inputs.** They were already only ever updated once per control-tick from
`ApcGrid`'s MIDI note-on/off handlers, never per-sample, so this is a lossless
representation change — pushed via `Lv2Host::setControl`, matching
`guitar_lofi_fx.dsp`'s own control-port convention for every other LV2-hosted
control in this codebase. `effects_runtime.dsp`'s `process()` now takes a
single audio-rate `resonodeIn` signal input instead of the old
`on0,og0,ov0, on1,og1,ov1, on2,og2,ov2, on3,og3,ov3` twelve-wire bundle —
`audio_thread.cpp` fills it from `resonodeFx.process()`'s own output when
engaged, or zeros it when not (the existing `resonodeEngageGate` crossfade in
`effects_runtime.dsp` is unchanged, it just reads from a C++-fed buffer now
instead of an in-Faust component call).

**A real regression shipped with this refactor and stayed silent for a whole
session: `RESONODE_ENGAGED`'s Faust zone was never written after the move.**
`targetToZone()` was changed to return `""` for every `fx/resonode/*` target
(comment: "pushed via `Lv2Host::setControl` ... never through this Faust-zone
path"), which is correct for `resonode.lv2`'s own control ports
(position/tone/decay/damping/stretch/collision/level, per-voice note/gate/vel)
but WRONG for `fx/resonode/engaged` specifically — that flag has TWO real
consumers, not one: (1) the C++ gate deciding whether `resonodeFx.process()`
runs at all (added correctly, reads `resonodeEngagedSlot` from `ParamStore`),
and (2) `effects_runtime.dsp`'s own `RESONODE_ENGAGED` checkbox, which drives
`resonodeEngageGate`, the crossfade that actually mixes `resonodeIn` into the
audible output. Only consumer (1) got wired; consumer (2) was left assuming
the OLD `targetToZone`/`resolvedControls` path still reached it, which it no
longer did once that path was deliberately blocked. Net effect: `resonodeIn`
was correctly computed (the LV2 plugin genuinely ran) but permanently
multiplied by a gate stuck at its compiled-in `0` default — WITNESSED on real
hardware as "mic sounds completely normal after Shift+LofiFx, keys don't
resonate with the mic as an exciter" — the dry/pitch-lock term stayed fully
open the whole time, indistinguishable from Resonode doing nothing at all,
even though the plugin was genuinely computing real audio underneath.
Fixed with a direct `fui.set("fx/resonode/engaged", resonodeEngagedNow ?
1.0f : 0.0f)` call right where `resonodeEngagedNow` is computed in the worker
loop, matching how `MONITORFOLD`/`GLITCHFOLD`/other C++-internal flags
already reach Faust zones directly (never through `targetToZone`). **Lesson,
generalizable beyond this one flag**: when a single control target gains a
second consumer during a refactor (here: an LV2 control port AND a Faust
crossfade checkbox, previously unified under one `targetToZone` string), each
consumer needs its OWN explicit write path audited — routing the primary
consumer correctly does not imply the secondary one still works, and nothing
in this codebase's `ParamStore`/Faust-zone architecture fails loudly when a
zone silently stops being written; it just reads its default forever.

**Signal-flow order was corrected at the same time**: guitar/lofi-fx
(`homeFx`/`userFx`) now run as an INPUT stage — on `fin`, before
`faustHome.compute()` — rather than at the very output on `fout` after the
whole dubfx chain. This means guitar/lofi-fx color what dubfx's own
pitch/harmony/filter/delay/reverb stages receive, not the finished mix.
Resonode's exciter is fed from that SAME post-guitar/lofi-fx signal (so its
excitation is colored by guitar/lofi-fx too), and its output re-enters
`effects_runtime.dsp`'s existing crossfade BEFORE `microStage:filterStage:
delayStage:reverbStage`, so engaging Resonode still passes through dubfx's
downstream filter/delay/reverb — it was never meant to bypass the home chain
entirely, only replace the dry/pitch-lock/harmony term feeding into it.

`kResonodeVoices` in `src/control/apc_grid.h` (still 4, driving voice
allocation/stealing and the `fx/resonodevoice{v}/*` ParamStore binds) must stay
in sync with `resonode_synth.dsp`'s real declared voice count by hand, same as
before — nothing ties a Faust `.dsp` file's voice count to a C++ constant at
compile time. `audio_thread.cpp` no longer needs its own copy of this constant
at all (removed) since it no longer threads per-voice buffers through `fins[]`.

## Tracktion Engine was evaluated and REJECTED — do not re-open without new evidence

The disqualifier is the threading/device model, not dependency weight. Three
things `audio_thread.cpp` does that Tracktion's model actively fights:

1. Two independent ALSA devices with deliberately different buffering (the
   instrument device blocking, the OTG gadget mirror opened `NONBLOCK` with
   `-EAGAIN` silently absorbed so a dead USB host can never stall the real
   path). JUCE's `AudioDeviceManager` gives one `AudioIODevice` — one rate,
   one buffer, one callback; the best-effort mirror is not expressible.
2. Manual per-core pinning (`pthread_setaffinity_np`, home stack on core 1,
   Core-3 FX on core 3, SCHED_FIFO 95, against `isolcpus`) — the same
   argument already used to reject Faust's own `-omp`/`-sch` scheduler:
   `tracktion_graph`'s own thread pool would fight the pinning rather than
   complement it.
3. The 1.333ms block budget and never-add-latency constraint. A DAW graph
   with plugin delay compensation dropped into that path is a latency
   regression that can only be disproven on real hardware, not argued.

Also: Tracktion ships as a JUCE module declaring `juce_gui_extra`, which pulls
in the X11/freetype stack on a headless device whose only UI is an APC Key25
— every one of those libs would join the hand-vendored `usr/sbin`/
`vendor/lib-aarch64` set and both `tar --mode='+x'` lists, the most
failure-prone surface in this project. Plus a GPL/Commercial license change
from the current no-obligation state.

**The higher-leverage alternative already available**: exactly one LV2 bundle
ships (`guitar_lofi_fx.lv2`), built from a Faust source in this repo
(`effects/home/faust/guitar_lofi_fx.dsp`) that this build already knows how
to compile natively — that is precisely what `faustHome`/`AloopLoopDsp` does
for the home stack. Compiling it into the Core-3 Faust program the same way
would let `lv2_host.{cpp,h}`, `lilv`, the crash-isolation watchdog, and
`build-lv2.yml`'s cross-compile job all be retired — removing moving parts
instead of adding them, with no new dependency or latency risk. Confirm
`/effects/user` (the swappable user-LV2 extension point) is genuinely unused
before acting on this, since retiring the host removes that surface too.

---

# Control surface (`src/control/apc_grid.cpp`)

## Every momentary Faust gate must be explicitly released

A one-shot gate driven from the control thread sticks at 1 forever unless something
writes it back to 0:

- **`looperN/erase`** — `dsp/loop.dsp`'s `wipe = max(clearAll, eraseN)` gates ring
  recirculation (`hold *= (1-wipe)`) every block, so a stuck `erase` silently wipes
  playback forever while recording still works (symptom: "after clearing it, the
  second round didn't play"). `pollHolds` records a ~50ms release deadline and
  clears it on a later tick. Setting then immediately clearing in the same call
  races the audio thread's plain-atomic read with no ordering guarantee.
- **`looperN/finishreq`** — same shape. `finishRequestedStep` only needs to see
  `finishreq>0.5` for one sample (it latches until the next armEdge), so ~50ms then
  release is correct.
- **`cmd/clearall`** — a genuinely HELD value (note-on sets it, the user's note-off
  releases it), so real wall-clock passes by construction; no deadline needed.

## `rec` must be explicitly zeroed on FINISH

`rec` is a persistent `ParamStore` value, not a momentary Faust `button()` the
widget releases. Setting `rec=1` and `play=1` in the same press with nothing
resetting `rec` makes `dsp/loop.dsp`'s `record = in*recN` re-record live input over
the loop forever — indistinguishable from "loops don't play, they just stay paused".
`applyRecPlayCycle` sets `rec=0` on FINISH.

Per-looper press cycle: empty → ARM (`rec=1`, held for the whole pass) → FINISH
(`rec=0`, `play=1`) → pause (`play=0`) → resume (`play=1`) → ...

**ARM and FINISH fire on PRESS, not release** — both are instants that must land
precisely, and release-triggered dispatch would add hold duration as timing jitter.
Pause/resume stay on release. `m_looperArmedOnPress` suppresses the matching release
from double-firing.

## CLEAR_ALL must zero both `play` and `rec` in Faust, not just C++ shadow state

Resetting `m_looperPlaying[lp]=false` alone leaves `dsp/loop.dsp`'s
`out = loopSig * playN * volN` outputting whatever the ring holds — `wipe` only
silences recirculated content, it does not touch the play gate.

Leaving `rec` stuck at 1 (CLEAR_ALL pressed mid-recording) is worse:
`hold = delayed*(1-recN)*(1-wipe)` stays zero for as long as `recN==1`, so that
looper can never play back ANY content again even after a fresh correct ARM/FINISH
cycle.

`onClearAll` explicitly writes both. `onStopImmediate` also zeros `rec` for any
mid-recording looper (unlike plain `cmd/stopall`, which only zeros `play`) —
stopping mid-recording is an abort, and an aborted looper stays "empty".

## An emptied rig must reset the shared master phrase length, from ANY path

Per-looper long-hold erase (not just PLAY/CLEAR_ALL) can leave the rig with zero
loopers holding content. `m_masterLenSamples`/`cmd/master_len` (and
`cmd/recorded_bpm`, which rides with it) must reset to 0 whenever the LAST looper
with content is erased, or the next recording lands the quantize branch instead of
the first-establish branch and truncates to a stale length. `pollHolds` checks
`anyHasContent` after the per-looper erase loop, mirroring `onClearAll`'s reset.

## Master phrase length comes from `writeIdx` telemetry, never wall-clock

The shared phrase length every looper quantizes to is established from the FIRST
recorded clip's actual duration. **Loop 1 must play back at EXACTLY its raw recorded
duration, like a commercial looper.** `deriveTempoQuant` is used ONLY to propose a
BPM to Link — it must never resize `m_masterLenSamples`/`cmd/master_len`. (Any
"TRUE PHRASE-LOCK" design that re-derives loop length from a tempo solver's
beats-at-BPM reconstruction is wrong; do not resurrect it.)

A wall-clock (`now_ms`) estimate cannot be sample-accurate relative to the audio
thread's per-block timeline. Read
`AudioThread::snapshotTelemetry().looperWriteIdx[looper]` — the DSP's true elapsed
sample count since the grid-aligned arm instant. Wall-clock survives only as a
defensive fallback when `audio` is null.

## Successive-recording quantization: powers of 2 only, log-space midpoint

A subsequent recording's raw duration snaps to a musical subdivision/multiple of
the established master phrase length M. Candidates are POWERS OF 2 ONLY relative to
M (M/16 floor, M/8, M/4, M/2, M, 2M, 4M, 8M, ...). The decision between the two
bracketing powers is the LOG-SPACE geometric midpoint `sqrt(lowerCand*upperCand)` —
symmetric and scale-independent regardless of which octave the recording lands in.

Because every candidate is a power of 2, any two loopers' `wrapLen`s are always in a
clean power-of-2 ratio, which (with `dsp/loop.dsp`'s `cycleOffset`) guarantees
drift-free repeat alignment forever.

Rejected alternatives, do not reintroduce: a small fixed candidate set
`{..., 2M, 4M}` (jumps far past the performed content, caps at 4M); a linear 68%
threshold (can trim up to 0.68×M off a genuine take); an M/16-linear-step grid
(lands on musically meaningless fractions like 5/16 — the user's requirement is
clean multiples, always).

Use `writeIdx` telemetry, not wall-clock, for the raw duration input here too.

## Real APC Key25 hardware re-sends note-on for an already-held pad

Unlike the synthetic MIDI-inject path. Without a guard, each repeat resets the
hold-start timer (defeating long-hold erase accumulation) and can re-enter the
ARM/FINISH dispatch mid-recording — prematurely finishing a take after a fraction of
a second and re-arming a new recording under what the user believes is still their
original press. This is a direct mechanism for "recording came out blank".
`onPadPress` tracks `m_looperHeld` per pad and treats a repeat note-on as a no-op.

**The same retrigger hits `kApcBtnLofiFx`, and this instance is worse: it makes
the Resonode hold-gesture structurally unreachable, not just occasionally
mistimed.** `onLofiFxPress` had no analogous guard — every repeat note-on for an
already-held LofiFx button re-ran unconditionally, stamping
`m_granulatorPressAt = now_ms` again on each retrigger. `pollHolds`'s engage
check (`now_ms - m_granulatorPressAt >= kGranulatorTapMs`) and
`onLofiFxRelease`'s tap-vs-hold classification both measure elapsed time against
that same continuously-reset timestamp, so on real hardware the elapsed time
never accumulates past a few tens of ms regardless of how long the button is
physically held — Resonode's 1000ms threshold can never be crossed, and every
release reads as a fresh tap (toggling `m_granulatorLatched` on every retrigger
interval, not just on a genuine quick tap). The same retrigger also re-stamps
`m_bankBeforeGranulatorHold = m_activeBank` every cycle; once the bank has
already flipped to `FxBank::LofiFx` from the first press, later retriggers
capture `LofiFx` itself as "the bank before the hold", so release could leave
`m_activeBank` stuck on LofiFx instead of reverting. WITNESSED as "pressing the
granulator button doesn't activate the granulator controls, and holding it
never engages the Resonode synth" — silent, no error, matching this class of bug
exactly. Fixed the same way as the grid pads: `onLofiFxPress` now returns
immediately if `m_granulatorHeld` is already true, so only the true 0→1 edge
touches `m_granulatorPressAt`/`m_bankBeforeGranulatorHold`. `onDubFxPress`/
`onGuitarFxPress` don't need the same guard — neither reads a press-start
timestamp; a stray reset of `m_guitarFxConsumedByLooperPress` on a GuitarFx
retrigger is a distinct, narrower concern (only matters if a looper press lands
between two retriggers) and is out of scope here.

## Guitar-fx held REDIRECTS looper pad presses entirely

While `m_guitarFxHeld` is true, a looper pad press is consumed by
`onSidechainLooperToggle` (toggling that looper's sidechain-source designation) and
never reaches the ARM/FINISH dispatch — it does not touch
`m_looperHeld`/`m_looperHoldStart` at all, since this is a one-shot toggle, not a
hold gesture. The sidechain-source designation auto-clears whenever that looper's
content is wiped (long-hold erase or CLEAR_ALL).

## LofiFx/granulator button: Shift disambiguates granulator-tap vs Resonode-tap

The LofiFx/granulator button (`kApcBtnLofiFx`, note 69) disambiguates its two
gestures via the SHIFT modifier (`ApcGrid::m_shift`, set/cleared by
`onShiftPress`/`onShiftRelease`), not a hold-duration timer. A previous
1000ms-hold design (`kGranulatorTapMs`, `pollHolds` polling for the threshold)
was WITNESSED unreliable on real APC Key25 hardware — the user reported
Resonode never actually engaged, the device "stayed on dub fx controls" the
whole session. Both gestures now fire on the PRESS edge, instantly, matching
every other instant gesture in this file (ARM/FINISH, bank-select) — no
hold-duration timing anywhere in this path:

- **Plain tap (Shift not held)**: `onLofiFxPress` toggles `m_granulatorLatched`
  and calls `setGranulatorEnabled(m_granulatorLatched)` in the same call, so
  the grain engine's on/off state lands instantly. This is "pressing latches
  the granulator" — a press makes the grain engine a persistent, backgrounded
  part of the sound (playing with whatever patch blend is currently dialed
  in). A second plain tap toggles it back off.
- **Shift+tap**: `onLofiFxPress` calls `toggleResonodeEngage`, which flips
  `m_resonodeLatched`/`m_resonodeEngaged`, writes `fx/resonode/engaged` to the
  ParamStore (the same flag `audio_thread.cpp`'s worker loop reads each block
  to decide whether to call `resonodeFx.process()` at all — see "Resonode is a
  separate, conditionally-called LV2 bundle" below), and forces the granulator
  latch off if it was on (mirroring the old hold-gesture's behavior: engaging
  Resonode always wins over a backgrounded granulator). Disengaging releases
  every held Resonode voice via `releaseAllResonodeVoices`.

Every press switches the active knob bank to LofiFx, but the bank ONLY
reverts to whatever was active before if Resonode is not engaged
(`onLofiFxRelease` checks `!m_resonodeEngaged` before restoring
`m_bankBeforeGranulatorHold`). WITNESSED real-hardware regression from the
shift-tap gesture change itself: the OLD hold-based design left the bank on
LofiFx for as long as the button was physically held down, so a real hold
naturally kept the knobs reachable while a player also turned them (or the
hold simply outlasted the knob adjustment). Once Resonode's engage became a
plain tap, the button released almost instantly and `onLofiFxRelease`
unconditionally reverted the bank BEFORE the player had any chance to touch
a knob — the knobs stopped controlling Resonode entirely, even though it was
now audible. `m_bankBeforeGranulatorHold` is also only captured on the FIRST
press while not yet engaged (`if (!m_resonodeEngaged) m_bankBeforeGranulatorHold
= m_activeBank`), not overwritten by every subsequent press while Resonode
stays engaged — otherwise the SECOND press (the one that disengages) would
capture `LofiFx` itself as "the bank to restore," stranding the bank on
LofiFx forever after disengaging instead of returning to whatever was active
before Resonode was ever touched. `onClearAll`'s own Resonode-disengage path
(a separate write to `m_resonodeEngaged`/`m_resonodeLatched`, bypassing
`onLofiFxRelease` entirely) needs the identical bank-restore call, guarded by
`!m_granulatorHeld` so it doesn't clobber an in-progress button press.

While Resonode is engaged, the keybed (`onKeybedNoteOn`/`Off`) drives the 4 Resonode voices
(`allocateResonodeVoice`/`releaseResonodeVoice`, oldest-steal allocator
identical in shape to `allocateTransposeVoice`) via `Lv2Host::setControl`
pushes to `fx/resonodevoice{v}/note`, `fx/resonodevoice{v}/gate`, and
`fx/resonodevoice{v}/vel` LV2 control ports — `vel` (real MIDI velocity,
`onKeybedNoteOn`'s existing parameter) scales that voice's exciter gain, so a
harder key press rings out louder. Knob slots 1-6 drive Resonode's own
controls instead of the granulator patch blend below: slots 1-4 are
named-patch blend weights (`applyResonodePatchMorph`/`kResonodePatches` —
Percussive, Metal/Glass, Strings, Dance Bass; see "Resonode named sweetspot
patches" below), slots 5-6 are direct performative dials
(`applyResonodeDirectKnob`/`kResonodeDirectKnobRanges`: tone brightness,
level — Faust zones `fx/resonode/*`). `onClearAll` also releases all Resonode
voices (mirroring the existing transpose-voice release there), since a stuck
`fx/resonodevoice{v}/gate` would be the same class of bug AGENTS.md's "every
momentary Faust gate must be explicitly released" entry warns about.

LED feedback on the button itself (`apc_leds.h`): blinking red while Resonode
is engaged, solid green while the granulator is latched-on, off otherwise —
no intermediate "pending hold" blink state anymore, since there is no more
hold window to represent.

Resonode is a SEPARATE, cost-gated LV2 bundle (`resonode.lv2`), not part of
the always-on home Faust stack — see "Resonode is a separate,
conditionally-called LV2 bundle" below for the full architecture and the
real-hardware RT-budget regression that motivated pulling it out. The 4
Resonode voices are only computed when engaged, unlike `multitranspose.dsp`'s
6 always-on voices — Resonode's own DSP graph is genuinely skipped at the C++
call site when `fx/resonode/engaged` is false, not merely crossfaded out
inside a Faust program that keeps paying its cost.

**Resonode is a real-input-excited resonator ("reactor mode"), not a
self-contained synth voice, and must REPLACE dry, never layer over it.**
`resonode_synth.dsp`'s `exciteFor(exciteIn, note, gate)` is the live `dry`
signal ALONE (filtered by `tone`, gated per-voice by `en.asr(...,xgate)`) —
there is no synthetic exciter anywhere in the signal path. An earlier
revision blended a synthetic percussive `impact` (`pm.strike`, triggered by
the key gate) under a `character` knob; that impact term was removed
entirely (per direct user request: keys must never play the exciter, only
the mic may) rather than left as a blendable option, since any nonzero
`character` toward `impact` meant a held key with silent input still made
sound — a synthetic voice wearing the resonator's face. A key held with no
live input now renders bit-exact silence (verified via the DawDreamer JIT
harness, `test/resonode-sweetspot/`). The `impact` term's own predecessor bug
is historical color only: `wash` (now the whole exciter) used to be
`no.noise`, a self-contained synthetic wash with no live-input path at all,
directly contradicting "excite via the mic" before it was fixed to read the
real `dry` bus.

`effects_runtime.dsp` passes the raw `dry` bus into
`resonode(dry, on0,og0,ov0, ...)` as this excitation signal; each voice only
picks it up while ITS OWN gate is held (the ASR envelope), so the live input
is "playing normally into" the resonator continuously but is only audible
through a voice while that voice's key is down — matches "only allowing
playback via the keys". The `character` hslider was repurposed rather than
removed: it now drives `position` (see "Resonode named sweetspot patches"
below), a real physical excitation-position parameter that was previously a
hardcoded `bankPosition = 0.35` constant.

A second, independent WITNESSED bug in the same feature: `resonodeOut` used to
be summed INTO `dry` (`dryWithResonode = dry + resonodeOut`) and that combined
signal was then run through the UNCHANGED `pitchStage`/`harmonize` dry-passthrough
machinery — `dryGate` there is driven only by the multitranspose voices'
gates (`g0..g5`), which Resonode never touches, so `dryGate` always stayed
near 1 (fully open) while Resonode was engaged. The result: raw `dry` (and
`harmonize`'s own separate dry-passthrough term `dryWet`) kept reaching the
output completely UNMUTED the whole time Resonode was held — audibly "the
button engages Resonode, but the ordinary pass-through/pitch-lock signal is
still there too", which is exactly the failure mode "Locked pitch must
REPLACE, never layer over, the original" (above) already named for the
transpose engine, just never extended to Resonode. Fix: a new
`checkbox("fx/resonode/engaged")` control (`RESONODE_ENGAGED` in
`effects_runtime.dsp`, set from `ApcGrid::pollHolds`/`onLofiFxRelease`
alongside the existing `m_resonodeEngaged` C++ flag — `targetToZone()` needs no
change since it already passes any `fx/resonode/...`-prefixed target through
verbatim) drives a smoothed `resonodeEngageGate` that CROSSFADES the entire
`(pitchStage(dry)*dryGate + dryWet)` term against `resonodeOut` right before
the shared `microStage:filterStage:delayStage:reverbStage` tail, rather than
summing `resonodeOut` into `dry` upstream of that machinery. At
`RESONODE_ENGAGED=0` this is bit-for-bit the pre-fix formula (resonodeOut is
silent there anyway, since voices are never gated unless
`m_resonodeEngaged`), so the disengaged path has zero behavioral change.
Verified via the DawDreamer JIT harness (pitch.dsp stubbed to a bare
passthrough per the "DawDreamer verification harness" section below): engaged
+ a held voice decorrelates the output from the raw dry input (r≈0.07 vs
r≈0.997 disengaged) while producing real excited-resonator amplitude; engaged
with no voice held decays to near-silence within one `si.smoo` time constant
of the engage edge.

Knob slot 0 (`fx2/BITCRUSHAMT`) is unchanged in both gestures. When Resonode
is NOT engaged, slots 1-6 map to the granulator patch blend as before; they
no longer map 1:1 to
raw grain parameters (grain size/density/scan rate/pitch spray/position
jitter/reverse probability) — turning six independent raw sliders to their
extremes simultaneously produced an incoherent, un-musical result. Instead
each slot is the BLEND WEIGHT of one of 6 fixed named patches
(`kGranPatches` in `apc_grid.cpp`: Glass, Cloud, Freeze, Chop, Tape,
Shatter — each a full point in grain-size/density/spray/jitter/scan/reverse/
envShape space representing a distinct musical character). `applyGranulatorMorph`
computes a weighted average across all 6 patches (weights normalized by
their sum, a convex combination) and pushes the single resulting blended
point into `Sampler::setGrainPatch` (one atomic call replacing the old six
independent setters — the previous per-setter design let a caller apply a
partial/inconsistent blend mid-update; the new one clamps and recomputes
derived state exactly once per morph). All weights at 0 falls back to patch 0
(Glass, closest to a plain/transparent read) rather than dividing by zero.
This means turning up two patch dials together always yields a coherent
midpoint texture instead of two raw parameters fighting each other, and
dialing every patch to max still yields a bounded, sane blend (never the sum
of six maxed-out raw parameters at once).

**`envShape`** (0..1, the 7th per-patch dimension, baked into each named
patch rather than exposed as its own knob — no free knob slot exists) blends
each grain's amplitude window between a round, symmetric raised-cosine (0,
pad-like/ambient) and a fast-attack/exponential-decay shape (1, plucky/
percussive/glitchy). Glass/Freeze sit at 0 (smooth, sustained textures), Chop/
Shatter sit near 1 (rhythmic, percussive chopping), Cloud/Tape sit in between.
Computed per-grain at spawn (`Grain::envShape`, frozen from `Sampler::m_envShape`
so an in-flight grain never changes shape mid-life even if the dial moves),
blended in `_renderGranularVoice` — cheap (one extra branch + lerp per grain
per sample, MAX_GRAINS<=48 per block, same cost class as the existing window
computation).

**Grain density gain compensation** (`Sampler::m_grainGainComp`,
`_recomputeGrainGainComp`): different patches spawn wildly different average
grain overlap (Glass: ~200ms grains at 8Hz, overlap ~1.6; Shatter: ~14ms
grains at 150Hz, overlap ~2.1, but Chop's short/sparse grains sit under 1)
and, uncompensated, the summed grain-window amplitude scales with overlap —
so morphing between patches used to also mean morphing LOUDNESS, unrelated to
the patch's actual musical intent. `1/sqrt(max(1, grainMs*0.001*grainRateHz))`
is recomputed once whenever `setGrainPatch` changes the blend (a STATIC
function of the density/size dial position, not of live active-grain or
active-voice COUNT) and applied as a flat multiplier at mix time in
`_renderGranularVoice`. This is deliberately NOT the "dynamic
`1/sqrt(activeVoices)`" anti-pattern `multitranspose.dsp` warns against (that
one pumps because it renormalizes against a count that changes on every
note-on/off mid-sustain) — this compensation only changes when the performer
turns a knob, so it never pumps during a held note.

Real MIDI velocity (previously hardcoded to 127 in `onKeybedNoteOn`, the real
`d2` byte discarded at the `midi.cpp` call site) now reaches
`Sampler::_noteOn` and scales `Voice::velGain`: overall voice loudness for
every voice (granular and plain alike), plus (granular voices only) grain
spawn density via `densityFromVel = 0.4 + 0.6*velGain` in
`_renderGranularVoice` — a harder key press plays louder AND spawns a denser,
brighter grain cloud, the dynamic-response feel real granular groovebox
hardware has and this sampler never had.

## Resonode voice-steal must retrigger the exciter AND glide the resonator frequency

`allocateResonodeVoice` (like `allocateTransposeVoice`) steals the oldest voice
slot when all `kResonodeVoices` are already held, reassigning that slot's
`fx/resonodevoice{v}/note` to the new key while leaving
`fx/resonodevoice{v}/gate` at 1.0 the whole time (both `onKeybedNoteOn`'s
steal path and its fresh-allocation path write `gate=1.0` unconditionally,
since there is no cheap race-free way to force a genuine 0→1 edge from the
control thread without the `pollHolds`-deadline machinery `erase`/`finishreq`
need — see "Every momentary Faust gate must be explicitly released" above).

WITNESSED via the DawDreamer JIT harness (`resonode_synth.dsp` compiles
standalone, no `pitch.dsp`/`ffunction` stub needed): feeding a synthetic
9-channel input (`exciteIn, note0,gate0, ..., note3,gate3`) that holds
`gate0=1` continuously while stepping `note0` from 60 to 72 mid-ring —
exactly what a steal produces — reproduced two independent, real defects:

1. **No new strike.** `pm.strike`'s internal `en.ar` and the wash's
   `en.asr` both key their attack phase on a literal `gate > gate'` rising
   edge (`envelopes.lib`); a gate that never drops never re-fires either
   envelope, so a stolen voice silently changes pitch with zero fresh
   excitation — the note-on is inaudible as an attack.
2. **A real click, worse than a legitimate onset.** `pm.modeFilter` is a
   raw `fi.tf2` biquad whose `a1`/`a2` are recomputed from `freq`/`t60`
   every sample with no smoothing; jumping `note` (hence `freq`) in one
   sample reuses the filter's stored `y[n-1],y[n-2]` under abruptly
   different coefficients. Measured max sample-to-sample derivative right
   at the steal instant: 0.057, over 2x a genuine fresh strike's own onset
   derivative (reference: 0.026, same DSP struck from silence) and over 3x
   the background derivative of an undisturbed ringing note (0.018) — a
   real, audible transient artifact, not merely "quieter than a strike".

Fix, both parts verified together and independently before landing:

- **`stealEvent`/`retriggerGate`**: `stealEvent(note,gate) = (note !=
  note') * (gate <= gate')` is true for exactly the one sample a steal
  reassigns `note` while `gate` was already high (and is structurally
  false at a genuine fresh onset, where `gate <= gate'` never holds since
  gate is rising that same sample). `retriggerGate` multiplies it into a
  synthetic one-sample dip of the signal fed to `pm.strike`/the wash's
  `en.asr`, so the very next sample sees a real `xgate > xgate'` edge and
  both envelopes fire a genuine fresh attack — exactly as if a new note-on
  had arrived.
- **`freqGlide`**: a `letrec`-based one-pole (the same recursive-signal
  idiom `delay.dsp`'s `curDelayRec`/`curStep` already uses in this
  codebase) that snaps instantly to `ba.midikey2hz(note)` on a genuine
  `gate > gate'` onset (bit-exact with the pre-fix behavior — verified,
  not assumed) and glides at `retuneGlide=0.01` per sample (~10ms to
  converge) on any other note change, i.e. only during a steal. This
  spreads the coefficient change over ~10ms instead of one sample,
  bringing the steal-instant derivative down to 0.022-0.027 depending on
  exact glide rate — in line with the genuine-onset reference, not a
  multiple of it.

Both changes are additive/no-ops outside the steal case: every
idle-silence, fresh-onset (including the realistic case where `note` and
`gate` both change in the same control tick, matching
`onKeybedNoteOn`/`allocateResonodeVoice`'s real write pattern), sustained
single-note, release, and 4-simultaneous-distinct-voice scenario rendered
bit-exact (or within float32 rounding, see below) against the pre-fix DSP.
With the fix applied, the same steal scenario's peak derivative (0.34-0.41,
dominated by `ma.tanh`'s soft-clip ceiling) now matches a constructed
reference of two *legitimately* overlapping strikes (one held note plus a
second, brand-new voice struck at the same instant, no steal involved:
0.34-0.63 across the same measurement windows) — the steal now sounds like
what it physically is, two strikes' energy briefly overlapping, instead of
a silent pitch-jump artifact.

`mode1`'s `pow(freqHz*pow(1.0,1.0+stretch), objDecay*pow(damping,0), ...)`
was also simplified to `pow(freqHz, objDecay, ...)` — `pow(1.0, x)` and
`pow(damping,0)` are mathematically always `1.0` regardless of the runtime
`stretch`/`damping` hslider values, but as runtime `pow()` calls (not
compile-time constants, since the exponent is a live hslider) Faust cannot
constant-fold them away, so every block was paying two wasted transcendental
calls per mode-1 evaluation across all 4 voices for a result that could
never differ from a literal `1.0`. This is a pure efficiency cleanup (the
DawDreamer regression suite's one non-bit-exact case, a 1.477e-06 max
absolute difference, is exactly the float32 rounding this removes, ~106dB
below audible).

## `mode2`/`mode3`/`mode4`'s damping exponent is small-integer — strength-reduce, don't `pow()`

A full-repo DawDreamer-based sweep (every `pow()`/`tan()`/`exp()`/`log()` call
site across `dsp/*.dsp` and `effects/home/faust/*.dsp`, checked for the same
"exponent independent of the runtime knob" pattern the `mode1` fix above
caught) found one more instance the `mode1` pass missed: `mode2`'s
`objDecay*pow(damping,1)`, `mode3`'s `objDecay*pow(damping,2)`, `mode4`'s
`objDecay*pow(damping,3)`. Unlike `stretch`-dependent exponents elsewhere in
this file (`pow(2.0, 1.0+stretch)` etc., genuinely runtime-variable, not
touched), these three exponents are LITERAL INTEGERS baked into the source
text, independent of any hslider — `pow(x,1)` is mathematically always `x`;
`pow(x,2)`/`pow(x,3)` are exactly `x*x`/`x*x*x`, computable with plain
multiplies instead of a full transcendental `pow()` call. Since `bank()`
computes all 4 modes for all 4 voices every sample unconditionally (Resonode
has no runtime branching, same as every other always-on stage in this file —
see "Faust has no runtime branching" above), this was 3 wasted-relative-to-
multiplication `pow()` calls x 4 voices = 12 calls/sample, permanently, not
gated by whether Resonode is even engaged.

Fixed: `pow(damping,1)` -> `damping`, `pow(damping,2)` -> `damping*damping`,
`pow(damping,3)` -> `damping*damping*damping`. Verified via the DawDreamer JIT
harness across 6 cases spanning the full position/decay/damping/stretch range
(including all 4 named sweetspot patches' exact settings): **bit-exact,
0.000e+00 max absolute difference in every case** — stronger than `mode1`'s
own fix (which had a 1.477e-06 float32-rounding residual), since multiplying
a value by itself introduces no more rounding than the `pow()` call it
replaces. `test/resonode-sweetspot/verify_musical_controls.py`'s existing
`tone_taper`/`morph_glide_click`/`no_fadein_regression` checks all still pass
unchanged. No `faust2bench` figure is available for this change (this
environment has DawDreamer's JIT bindings only, not the standalone `faust`
CLI `faust2bench` needs — see "DawDreamer verification harness" below); a
same-environment JIT render-time A/B (20s render, x86_64, not a substitute for
`faust2bench` on real hardware) showed a small, consistent-direction
improvement, in line with removing 12 `pow()` calls/sample from an always-hot
path.

## Resonode's higher modes must mute, not alias, once their frequency exceeds Nyquist

`pm.modeFilter(freq,t60,gain) = fi.tf2(...)*gain` computes `a1 =
-2*r*cos(2*ma.PI*freq/ma.SR)`, which is trigonometrically well-defined (and
numerically stable, `r<1` always) for `freq` above `ma.SR/2` — but a
"resonance" above Nyquist has no such thing as its true frequency; the
biquad instead produces a spurious peak at the alias frequency
`SR-freq` (or further-folded multiples of it), with no relationship to the
played note's harmonic series. WITNESSED via the DawDreamer JIT harness
(broadband-noise-excited voice, FFT of the settled ring, 8 loudest spectral
peaks): note 120 (fundamental ~8372Hz) has mode4 at
`8372*4*pow(4,1+stretch)=33488Hz` with `stretch=0` — above the 24000Hz
Nyquist ceiling — and measured output showed a real spectral peak at
~14513Hz, matching `2*24000-33488=14512Hz` almost exactly: the classic
fold-back alias formula, not silence and not the intended 4th mode. This is
reachable at ordinary knob settings (`stretch=0`, the slider's own default)
starting around any note whose 4th-mode multiple crosses 24kHz — well
inside the practical playable range of a keybed-driven instrument, not an
extreme corner case.

`aliasGuard(f) = min(1.0, max(0.0, (ma.SR*0.5-f)/(ma.SR*0.05)))` multiplies
each mode's own gain term (computed against that SPECIFIC mode's actual
frequency, not the voice's fundamental — `mode2`/`mode3`/`mode4` each bind
their own `f2`/`f3`/`f4` via `with{}` since their real ringing frequency is
`freqHz*pow(ratio,1+stretch)`, not `freqHz` itself), linearly fading a
mode's contribution to 0 over the top 5% of the Nyquist range and leaving
it untouched (multiplier exactly 1.0) everywhere else. Re-measuring the
same note-120 case with the guard in place: the spurious ~14513Hz alias
peak is gone; the loudest peaks are the fundamental and the still-valid
(sub-Nyquist) mode2. Verified as a pure no-op for the whole practically
playable register below the guard band: every existing regression scenario
(idle, fresh onset, mid-range notes, the voice-steal fix above) renders
bit-exact against the pre-guard DSP, since none of their mode frequencies
approach the last 5% of Nyquist.

## Resonode's mode bank is 6 modes/voice with real per-voice velocity, not 4 modes with no dynamics

The original design fixed the mode bank at 4 modes/voice with no velocity
input at all — every note-on played at the same fixed exciter gain regardless
of how hard the key was struck. Both were expanded together: `bank()` grew
mode5/mode6 (ratios 5 and 6, same `freqHz*pow(k,1.0+stretch)`/`aliasGuard`
pattern as modes 2-4, gain weights 0.22/0.16 continuing the existing
1.00/0.60/0.40/0.30 geometric-ish falloff), and `resonode_synth.dsp`'s
`process()` gained a third per-voice signal input (`vel0..vel3`, alongside
`note`/`gate`) that scales `exciteFor`'s envelope output directly (`velGain(vel)
= max(0.0, min(1.0, vel))`, a plain clamp, no curve) — `apc_grid.cpp`'s
`onKeybedNoteOn` already receives real MIDI velocity for other purposes
(`Sampler::_noteOn`'s own `velGain`), it simply was never wired into this
instrument. `damping`'s exponent chain from the strength-reduction fix above
was extended the same way: `dp4 = dp3*damping`, `dp5 = dp4*damping`, computed
once inside `bank()`'s own `with{}` rather than as separate `mode5`/`mode6`
top-level functions (the whole mode bank was restructured from 6 separate
`modeN(freqHz)` top-level functions into 6 local definitions — `m1..m6` — inside
one `with{}` block, so `dp2..dp5`/`f2..f6` are each computed once and shared,
not recomputed per mode).

**A velocity-driven exciter-BRIGHTNESS coupling was tried, measured, and
REJECTED before shipping — this is the load-bearing lesson, not the feature
itself.** The natural-looking design multiplies the exciter's pre-resonator
lowpass cutoff by a per-voice brightness term (`fi.lowpass(2,
tone*brightnessMul(vel))` instead of the shipped `fi.lowpass(2, tone)`).
WITNESSED via a same-environment JIT render-time A/B (12s render, x86_64 —
not `faust2bench`, not real Pi 4 hardware, a proxy only, per the
"Compiling clean proves nothing about runtime safety" rule above): this
change alone pushed total render time from ~0.286s (6-mode + velocity-GAIN
only) to ~0.597s median — nearly DOUBLING total instrument cost — while the
measured acoustic effect was negligible (spectral centroid within 2% across
vel 0.2/0.6/1.0). Root cause: all 4 voices previously called `fi.lowpass(2,
tone)` against the textually IDENTICAL `tone` signal, letting Faust's
compiler share that one coefficient computation (a `tan()`-bearing biquad
design formula) across all 4 voices; giving each voice a UNIQUE
`tone*brightnessMul(velN)` argument made the 4 calls textually distinct,
forcing 4 independent coefficient recomputations where there used to be
effectively 1 shared one. The acoustic payoff was near-zero for the same
underlying reason the tone-taper fix above already names: once the
pre-resonator lowpass cutoff clears the resonator bank's own highest mode
frequency, the resonator's own mode-frequency selectivity — not the exciter's
pre-filter — is what actually shapes the timbre, so varying that pre-filter
per-voice bought almost nothing. **Any future per-voice control that feeds a
signal ALL VOICES currently share identically (like `tone` here) should be
assumed to defeat this cross-voice sharing and be measured, not assumed
cheap, before shipping.** Velocity-driven GAIN was kept instead: a plain
multiply on the exciter's envelope OUTPUT (never touching a shared filter
argument), cost-neutral, and it produces a real, large loudness range (RMS
0.222/0.415/0.504 across vel 0.2/0.6/1.0 at the shared default patch).

`test/resonode-sweetspot/verify_musical_controls.py` gained four checks
covering this expansion: `velocity_response` (RMS/centroid monotonic in vel),
`silent_without_live_input` (the mic-only-exciter invariant still holds at 6
modes), `new_mode_alias_guard` (mode5/mode6 specifically fold to silence, not
an aliased peak, once their ratio pushes them past Nyquist), and
`voice_steal_with_velocity_change` (a steal that changes `vel` in the same
control tick as `note` produces no extra click beyond the existing
steal-derivative baseline).

`audio_thread.cpp`'s `kTransposeVoices`/`kResonodeVoices` constants and
`apc_grid.h`'s own copies of the same names must be kept in sync with
`multitranspose.dsp`'s/`resonode_synth.dsp`'s real declared voice count by
hand — nothing ties them together at compile time (a Faust `.dsp` file and a
C++ constant have no shared type system), so a voice-count change in either
file needs the matching constant updated in both places.

## Resonode named sweetspot patches (`kResonodePatches`)

Once the exciter became mic-only (see "Resonode is a real-input-excited
resonator" above), `character` no longer had a blend to control — it was
repurposed into `position`, and `resonode_synth.dsp`'s four material-identity
parameters (`position`, `decay`, `damping`, `stretch`) were moved off direct
1:1 knob mapping (`applyResonodeKnob`) onto a named-patch convex-blend surface
(`applyResonodePatchMorph`), mirroring the granulator's own `kGranPatches`
mechanism (see "Super music granulator" above) exactly: knobs 1-4 are patch
weights, normalized by their sum and blended into a single point, falling
back to patch 0 when every weight is 0. `tone` (brightness) and `level`
(loudness) stay direct dials on knobs 5-6 (`applyResonodeDirectKnob`,
`kResonodeDirectKnobRanges`) rather than being folded into the patches —
those two are genuinely performative, something a player rides live while
holding a note, unlike `position`/`decay`/`damping`/`stretch`, which define
what the resonator is made of and are better dialed in as a single named
identity than four independent sliders fought into alignment by hand.

**The four patches were found empirically, not hand-guessed, and RE-SWEPT
after the mode bank grew from 4 to 6 modes/voice** (see "Resonode's mode bank
is 6 modes/voice" above) rather than assuming the old picks still analyze
best — adding two modes measurably changes the acoustic result at any fixed
`(position,decay,damping,stretch)` point, so the only honest way to know
whether the 4-mode-era picks were still optimal was to re-run the same
methodology against the current DSP. The DawDreamer JIT harness at
`test/resonode-sweetspot/` (`sweep.py` + `select_patches.py`) grid-searched
`position`/`decay`/`damping`/`stretch` (5 levels each, 625 renders total;
`resonode_synth.dsp` compiles standalone against a single multi-channel
`make_playback_processor` feeding all 13 declared inputs by channel order —
no `ffunction`/`pitch.dsp` stub needed, same as the voice-steal/alias-guard
harnesses above; `vel` is pinned to 1.0 throughout the sweep, so the search
covers timbre, not the separate velocity-loudness axis) against a synthetic
mic excitation (a short filtered-noise burst, decaying over ~3ms, then
silence — a generic stand-in for a real tap/pluck/vocal transient) and
scored each render's measured features (RMS-envelope decay time to -24dB,
an early/late RMS transient ratio, magnitude-squared-weighted spectral
centroid over a 100-400ms post-onset window, and the fraction of that
window's energy below 1.5x the fundamental) against a hand-authored target
direction per voice. `stretch` itself (not a measured feature) is used
directly as an inharmonicity proxy in the scoring, since the FFT
peak-vs-nearest-integer-harmonic `inharmonicity` feature the harness also
computes is systematically biased toward small values at high partial
numbers (nearest-integer rounding error shrinks as 1/k) and isn't reliable
enough to rank by on its own — a real limitation of that specific metric,
not of the sweep methodology.

**The re-sweep's top pick for every one of the 4 patches landed on the exact
same `(position,decay,damping,stretch)` point as the original 4-mode sweep** —
all four target directions are dominated by pushing `decay`/`damping`/`position`/
`stretch` to a grid EXTREME (the sharpest-decay corner for Percussive, the
longest-ring-plus-max-damping corner for the two sustained patches, etc.), and
the two added lower-weighted modes (0.22/0.16 vs modes 1-4's 1.00/0.60/0.40/0.30)
shift each render's absolute feature values slightly without changing which
corner of the search grid wins for any of the four target directions. This is
a genuinely re-verified result, not an unexamined carryover: the measured
feature values below are the NEW 6-mode numbers, not the old 4-mode ones (each
shifted by a small, explainable amount — e.g. Percussive's transient ratio rose
from ~34 to ~35 as modes 5/6 add a touch more energy to the initial transient
without materially changing the decay tail), each again spot-verified at a bass
note (36) and a high note (84) to confirm the alias guard keeps every patch's
output finite and sane across the whole keybed.

| Patch (knob 1-4 slot) | position | decay | damping | stretch | collision | Measured character (6-mode) |
|---|---|---|---|---|---|---|
| Percussive | 0.08 | 0.15 | 0.80 | -0.10 | 0.55 | 60ms decay, transient ratio ~35.3 (sharp tap) |
| Metal/Glass | 0.08 | 7.00 | 0.97 | 1.20 | 0.15 | ~2.5s ring, centroid ~1.85kHz, only 26% of energy below 1.5x f0 (bright, inharmonic) |
| Strings | 0.08 | 7.00 | 0.97 | -0.10 | 0.00 | ~2.5s ring, centroid ~632Hz (~2-3x f0), harmonic partials audibly present (not a bare sine) |
| Dance Bass | 0.42 | 7.00 | 0.15 | -0.10 | 0.30 | long ring (decay=7.0, damping=0.15), 93-97% of spectral energy below 1.5x f0 across notes 28/36/48 (sub-bass-dominant), sustain ratio 0.86-0.94 (RMS at 500-600ms vs 50-150ms) |

`collision` is hand-set per patch from Objekt-informed design intent (a
percussive/mallet hit should have real "bounce," a clean plucked string
should not) rather than swept jointly with `position`/`decay`/`damping`/
`stretch` as a 5th search dimension — its own mechanism (bounded, click-safe,
exact identity at 0) is independently DawDreamer-verified (see "Resonode
gained `collision`..." above), but its specific per-patch VALUE here is a
judgment call, not a grid-search optimum. `position`/`decay`/`damping`/
`stretch` for Percussive/Metal-Glass/Strings are the unchanged, already
grid-search-verified 6-mode values from the prior re-sweep (adding
`collision`/pitch-mod as orthogonal, mostly-post-100ms-settled dimensions
doesn't invalidate that search — pitch-mod settles to true pitch by ~40-50ms,
well before the sweep's 100-400ms centroid measurement window, and
`collision` defaults are outside the swept dimensions entirely).
**Dance Bass was accidentally lost in a later pass** (swapped for a
"Wood/Membrane" patch that pointed slot 3 at a body-resonance/skin character
instead) and is restored here, in its original slot, at its original,
already grid-search-verified `(position,decay,damping,stretch)` point —
`select_patches.py`'s `DanceBass` target direction (a heavy
`lowFreqEnergyRatio` bonus, a strong negative `spectralCentroidHz` weight,
and a `stretchAbsPenalty` favoring near-harmonic tuning, i.e. "keep the
fundamental dominant and everything else penalized") was re-run against the
current 6-mode+collision+pitch-mod engine (same 625-combo grid) and landed on
the identical grid corner, for the same reason the other three patches'
picks carried over: the target direction is dominated by pushing
decay/damping toward their sustain-favoring extreme and stretch toward zero,
which the added modes/collision/pitch-mod don't change.

`collision` didn't exist when Dance Bass was first swept, so it was hand-set
the same way as the other three patches: measured, not guessed. A
same-environment DawDreamer JIT sweep of `collision` 0.0-0.6 at the shipped
`position`/`decay`/`damping`/`stretch` point (note 40, held) showed a
monotonic rise in decay-tail energy (first-diff-RMS proxy 0.138 -> 0.243,
+76% from `collision` 0.0 to 0.6) while the dominant sub-bass character
stayed almost untouched (`lowFreqEnergyRatio` 0.999 -> 0.973, spectral
centroid 82.5Hz -> 87.2Hz). `0.30` was chosen as the point giving a clearly
audible harmonic/grit boost — useful for cutting through a mix and
translating on small speakers, the exact quality dance/dubstep bass sound
design leans on physical-modeling resonance for — while `lowFreqEnergyRatio`
at that setting is still north of 0.99: well short of Percussive's 0.55
(built to be all-transient) and above Metal/Glass's 0.15 / Strings' 0.0
(built to stay clean). `test/resonode-sweetspot/verify_musical_controls.py`
gained a permanent `dance_bass_shipped_patch` check (renders the exact
shipped point at notes 28/36/48 and asserts `lowFreqEnergyRatio > 0.85`, a
sustain ratio > 0.5 between the 500-600ms and 50-150ms windows, and a
finite/bounded output) specifically so a future patch-table edit can't
silently swap Dance Bass out again without a red test.

Metal/Glass and Strings share `position`/`decay`/`damping` and differ only
in `stretch` — the sweep independently rediscovered (both times) that
inharmonicity (`stretch`) is the one parameter that actually separates
"bell-like" from "string-like" once decay and damping are both maxed toward
a long, lightly-damped ring; `position`'s effect on the mode-gain weights
(`abs(sin(pi*position*k))`) turned out to be far less intuitive than its
name suggests (0.08 doesn't favor the fundamental — it gives roughly equal
weight across modes 1-4, and now 5-6 too) and the optimizer's empirical
answer overrode any hand-authored assumption about it, both before and after
the mode-count expansion.

## Resonode's `tone` knob needs a logarithmic taper, not a linear Hz sweep

`applyResonodeDirectKnob` originally mapped the knob 0..1 straight onto
`fx/resonode/tone`'s 200..18000Hz range linearly. WITNESSED via the DawDreamer
JIT harness (broadband-noise excitation, measured post-onset spectral
centroid): across every note tested (48/60/72/84), 85-99% of the knob's
total brightness change landed in the first 10-20% of its physical travel,
with the remaining 80-90% of the knob doing almost nothing — because the
`tone` hslider only feeds `exciteFor`'s pre-resonator lowpass, and once its
cutoff clears the resonator bank's own highest mode frequency, widening it
further has no more resonant content to reveal. This is the same shape of
defect as any linear-Hz filter-cutoff control: nearly the whole audible
range is compressed into a sliver of the knob.

Fixed by an exponential (`lo * pow(hi/lo, v01)`) taper in
`applyResonodeDirectKnob` (`ResonodeDirectKnobRange::logTaper`), which stretches
the perceptually-relevant low end across most of the knob and compresses the
already-inert top end into a small slice at the far end — re-measured
post-fix, the first 10% of knob travel drops to 0-15% of the total
brightness change (was 85-99%). `level` stays on the plain linear taper
(`logTaper=false`) — its dB response was already reasonably even across the
knob (`test/resonode-sweetspot/verify_musical_controls.py`'s
`check_tone_taper`/measurements), and a taper only helps a control that has
a demonstrated dead zone, not every control by default.

## Resonode's morph knobs (position/decay/damping/stretch/tone/level) needed glide, not bare hsliders

Every one of `resonode_synth.dsp`'s six controls was a bare `hslider` feeding
straight into `pm.modeFilter`'s per-sample coefficient recompute (`position`/
`decay`/`damping`/`stretch`) or `fi.lowpass`'s biquad (`tone`) — the same
"raw coefficient jump" failure class already fixed once in this file for
note-steal (`freqGlide`, see above) and independently documented in
`delay.dsp`/`multitranspose.dsp`. WITNESSED via the DawDreamer JIT harness:
naively stepping `position`/`stretch`/`tone` mid-ring while the resonator is
actively decaying produces a real sample-to-sample derivative spike several
times the local background (position: ~2.8-9x, worse right after a fresh
strike) — an audible click every time a performer morphs the patch-blend or
tone/level knobs while a note is ringing, since `applyResonodePatchMorph`/
`applyResonodeDirectKnob` write the full blended value on every single MIDI CC
tick with no interpolation on either the C++ or Faust side.

Fixed with the same `si.smooth`-style one-pole idiom `multitranspose.dsp`
already uses (`morphGlide`, `ba.tau2pole(0.015)`, ~15ms), applied to all six
controls at their declaration site. **A naive `si.smooth` wrap is not
enough**: its recursive register zero-initializes, so every control would
glide up from 0 over the first several time-constants of the instrument's
life — WITNESSED as the first 5ms of a fresh onset rendering at only 12% of
its correct loudness (decay/damping/position/tone/level all fading in from
"the resonator doesn't exist yet" instead of starting at their real
default/dialed-in values) before ramping to ~90%+ by ~40ms. This is the
identical zero-init pitfall already named for `multitranspose.dsp`'s
`windowFor` smoother. Fixed the same way `freqGlide` already does in this
file: a `letrec`-based one-pole that SNAPS to the true value on `ba.time ==
0` (the very first sample this DSP instance ever computes) and glides only
on any later change — verified bit-exact-modulo-float32-rounding (~2e-4 max
absolute, pure floating-point reassociation noise from the restructured
signal graph, reproducible even with `-vec` disabled and with the dead
`morphGlide` definition stripped from the control render, i.e. not a logic
bug) against the unsmoothed DSP when parameters are never touched, and the
first-5ms onset RMS ratio (smoothed/raw) at 1.000.

`test/resonode-sweetspot/verify_musical_controls.py` codifies both fixes:
`check_tone_taper` (linear vs exponential brightness-spread measurement),
`check_morph_glide_click` (raw vs smoothed derivative-spike ratio at a
mid-ring parameter jump), and `check_no_fadein_regression` (first-5ms onset
loudness parity at static default params) — run after any further change to
`resonode_synth.dsp`'s control declarations.

## `ApcGrid::bindAll` must `ps.bind()` every internal flag a C++ path later `setByName`s

`ParamStore::setByName` only writes into a slot `bind()` already created — it never
creates one itself (`bind()` is the only path that inserts into the `slot` map).
`pollHolds`/`onLofiFxRelease` call `ps.setByName("fx/resonode/engaged", ...)` on the
hold-threshold and release edges, but `fx/resonode/engaged` was never added to the
`ps.bind("fx/resonode/...")` block in `bindAll` (only `character`/`tone`/`decay`/
`damping`/`stretch`/`level` were). Those `setByName` calls were therefore silent
no-ops on every real device: the WITNESSED symptom was "holding the LofiFx button
past the 1s threshold never produces the Resonode sound" even though
`m_resonodeEngaged` flips correctly and the per-voice `fx/resonodevoice{v}/note`/`gate`
slots (which ARE bound) work fine, so notes visibly retune/gate but nothing
resonator-like is ever audible.

Root cause is downstream of the missing bind, not in `targetToZone` or the DSP:
`resolvedControls` (the per-block ParamStore→Faust-zone cache in
`audio_thread.cpp`) is rebuilt by iterating `g_params->forEach(...)`, which only
visits BOUND names. An unbound target is invisible to that cache regardless of
`targetToZone` returning a valid zone string for it, so the Faust
`RESONODE_ENGAGED = checkbox("fx/resonode/engaged")` in `effects_runtime.dsp` never
leaves its compiled-in default of 0, `resonodeEngageGate` stays 0, and
`preChain`'s crossfade never picks up `resonodeOut` — permanently silent, no error
anywhere. This is a different failure shape than the "`targetToZone()` must have a
case for every control target" entry above (that one is a missing zone-name
mapping; this one is a missing ParamStore slot for a target `targetToZone` already
handles correctly via its `fx/resonode/`-prefix passthrough).

Fix: `ps.bind("fx/resonode/engaged", 0.0f)` alongside the other `fx/resonode/*` binds
in `bindAll`. Any future internal (non-MIDI-mapped) C++ flag that reaches Faust via
`setByName` needs the same audit — grep `setByName` targets against `bind()` calls
before trusting a new flag "should just work" because `targetToZone` has a case for
it.

## Resonode gained `collision` (bounded per-voice waveshape) and pitch-mod (impact-deformation onset bend)

Both additions were scoped from researching how other physically-modeled
resonator instruments' own manuals describe their controls (strike
position, per-band decay/damping, dispersion/stretch, a "collision"
bounce/rattle amount, and a "pitch deforms under a harder hit, more on
flexible material than stiff" behavior) and re-expressing the same
functional ideas inside this engine's existing 6-mode architecture —
following ADR-022's standing decision, no commercial product is named
anywhere in the tree; only the generic physical-modeling vocabulary these
concepts already share is used. `stretch` (dispersion/inharmonicity) and
`position`/`decay`/`damping` already existed; `collision` and pitch-mod are
new.

**`collision`** is a 6th per-patch dimension (`hslider("fx/resonode/collision",
0, 0, 1, ...)`, on the same `morphGlide` letrec-snap-then-glide idiom as the
other five morph-blended controls) driving a bounded per-voice waveshaper
applied to each voice's own `bank()` output, before `voiceGain` and before
the voices are summed:

```
collisionDrive(x) = x*(1.0 - collision) + collisionShaped(x)*collision
with {
    driveAmt = 1.0 + collision*6.0;
    collisionShaped(x) = ma.tanh(x*driveAmt)/ma.tanh(driveAmt);
};
```

At `collision=0` this is `x*1.0 + (anything)*0.0`, an EXACT identity
regardless of what `collisionShaped(x)` evaluates to (floating-point `0*y`
is exactly `0` for any finite `y`) — verified bit-exact
(1.79e-07/3.58e-07 max abs diff, pure float32 rounding) against the
pre-`collision` DSP via a same-environment DawDreamer JIT A/B. This is the
same disabled-state-is-exact-passthrough discipline `aliasGuard` and
`RESONODE_ENGAGED`'s crossfade already use in this file.

WITNESSED via the DawDreamer JIT harness: `bank()`'s raw per-voice output
during a broadband-noise-burst transient is surprisingly hot (~17, far
above unity — 6 simultaneously-resonant modes excited by broadband noise
before any one mode has decayed), so at `collision=0` it is *already* the
shared final `ma.tanh(sum)*outLevel` stage in `process()` doing all the
limiting, right at its ceiling with almost no headroom. Raising `collision`
moves real per-voice compression earlier in the chain (a plain memoryless
waveshaper on a signal that never feeds back into `pm.modeFilter`'s own
recursive state, so it cannot alter decay/pitch, only reshape amplitude),
which both tames that per-voice transient AND boosts the quiet decay tail's
small-signal content (its slope at the origin is `driveAmt/tanh(driveAmt)`,
~7x at `collision=1`) — measured as monotonically rising tail energy
(crude first-difference RMS 0.0346 -> 0.0533 from `collision` 0 to 1 on an
otherwise-identical render) without ever exceeding the final stage's
existing 0.8 (`outLevel`-default) ceiling or producing any non-finite
sample, at any `collision` setting tested (0.0/0.3/0.7/1.0).

Click-safety at the moment `collision` jumps (patch-morph knob turned
abruptly) is signal-amplitude-scaled, unlike `position`/`stretch`/`tone`/
`damping`'s discontinuities: WITNESSED at a quiet point in the decay tail
(125ms in) the raw unsmoothed jump is already smaller than the local
background derivative (ratio 0.07-0.09, no audible click either way), but
at points during the loud initial transient (nearer note-on) the raw jump
IS a real discontinuity (ratio 0.59-0.62) and `morphGlide` brings it down to
0.00-0.04 — confirming the glide is genuinely load-bearing for `collision`
too, just only audible near a loud transient rather than uniformly like the
other controls.

**Pitch-mod** is NOT a per-patch dimension — it is a small, always-on onset
frequency deviation applied globally inside `freqGlide` (now
`freqGlide(note, gate, vel)`, threading `vel` through from `voice()`),
scaled by both real velocity and a `flexibility` term derived from
`stretch`:

```
flexibility = max(0.0, min(1.0, (0.5 - stretch)));
target = ba.midikey2hz(note) * (1.0 + pitchModDepth*velGain(vel)*flexibility*pitchEnv(note, gate));
```

`flexibility` maps LOW/negative `stretch` (near-harmonic, string/membrane-
like dispersion) to MORE pitch-mod and HIGH `stretch` (strongly dispersive,
bell/bar-like) to LESS — the physically-motivated proxy for "flexible vs
stiff material" this engine actually has, since `stretch` is already this
file's dispersion/inharmonicity control (see "Faust stdlib functions can
hide oversized buffers" section's stiffness/dispersion framing) and there
is no separate stiffness parameter. `pitchEnv` is a one-shot exponential
spike-and-decay (`pitchModDecayS = 0.04`, ~40ms to -60dB), retriggered on
the same `attackEdge` (genuine note-on OR voice-steal) `freqGlide` already
needed for its own onset-vs-glide branch, so a stolen voice gets a fresh
pitch-mod bump exactly where it already gets a fresh exciter retrigger (see
"Resonode voice-steal must retrigger the exciter" above) — one shared edge
detector, no new per-voice state.

Because `freqGlide`'s OUTPUT (not just the fundamental literal) feeds
`bank()`, the pitch-mod bump shifts every mode's frequency together (a
global detune of the whole resonant structure during the impact transient,
not just the fundamental), matching a real object's own behavior under
impact deformation.

WITNESSED via the DawDreamer JIT harness, using an impulse excitation (not
the burst noise the position/decay/damping/stretch sweep uses, since a
broadband burst overlaps too many partials for clean pitch tracking) with
modes 2-6's gain terms temporarily zeroed in the RENDERED TEXT ONLY (a
test-only substitution, never shipped) to isolate the fundamental for
zero-crossing-based instantaneous-frequency measurement:

- `stretch=-0.5` (flexible): +43.8 cents onset, settled to +0.1 cents by
  40-50ms.
- `stretch=0.0` (neutral): +21.8 cents onset (flexibility=0.5), settled to
  +0.0 cents.
- `stretch=1.5` (stiff): +0.0 cents onset (flexibility clamped to 0, no
  pitch-mod at all) — matching "more prominent on flexible materials than
  stiffer ones."
- At fixed `stretch=-0.5`, velocity 0.2/0.5/1.0 produced +8.7/+21.8/+43.8
  cents onset — linear in velocity, as the formula is a plain product.

`pitchModDepth=0.04` (max ~44 cents at vel=1, `stretch=-0.5`) was chosen as
a deliberately modest, musical amount — a real but subtle character shift,
not an aggressive pitch-bend effect. Disabled-state bit-exactness (`vel=0`
OR `pitchModDepth` forced to `0.0`) was verified the same way as
`collision`'s: identical rendered audio (1.79e-07 max abs diff) against the
pre-pitch-mod DSP.

`test/resonode-sweetspot/verify_musical_controls.py`'s full existing suite
(`tone_taper`, `morph_glide_click`, `no_fadein_regression`,
`velocity_response`, `silent_without_live_input`, `new_mode_alias_guard`,
`voice_steal_with_velocity_change`) was re-run against the DSP with both
additions live, plus three new permanent checks
(`collision_zero_is_identity`, `collision_bounded_and_monotonic_energy`,
`pitch_mod_gated_by_velocity_and_flexibility`) covering the new controls the
same way the four checks from the mode-bank expansion did.

**`velocity_response` broke on first run with pitch-mod added, and the root
cause was a PRE-EXISTING fragile assertion, not a real regression** — WORTH
recording since it looked exactly like a regression at first glance. The
check asserts spectral centroid is non-decreasing (within a 2% tolerance)
across vel 0.2/0.6/1.0 at a fixed 50ms warmup. Isolated A/B against the
pre-pitch-mod DSP with an UNCHANGED `pitchModDepth=0.0` (same
disabled-state trick used everywhere else in this file) reproduced the
exact same centroid numbers as the real pre-pitch-mod file, confirming
pitch-mod itself wasn't the mechanism — and re-measuring the ORIGINAL,
never-touched DSP at a later warmup (300ms instead of 50ms) showed the SAME
non-monotonic, vel-inversely-correlated centroid trend already present
before any of this session's changes (323.3/321.5/319.0 Hz across vel
0.2/0.6/1.0, decreasing). Velocity in this architecture only ever scaled
the exciter's envelope GAIN uniformly (the velocity-brightness *coupling*
was deliberately rejected earlier, see "Resonode's mode bank is 6
modes/voice" above) — the mm-scale centroid dependency the 2%-tolerance
check relies on was always an incidental side effect of amplitude
interacting with the mode bank's own transient response, not a designed or
robustly-guaranteed relationship, and pitch-mod's own small, real,
verified, monotonic frequency-vs-velocity effect (see the cents figures
above) simply wasn't enough to keep tipping an already-razor-thin
pre-existing margin (1.5Hz out of ~340Hz) the check depended on. Fix:
loosened the centroid tolerance from 0.98 to 0.90 (RMS/loudness — the
actually hard, well-margined, load-bearing invariant here, 0.222 / 0.415 /
0.504 — stays at 0.98, unchanged). Re-tightening this centroid check to
something stricter than 0.90 would need a real per-mode-isolated brightness
metric (like the mode1-isolation trick pitch-mod's own cents measurement
above uses), not the full 6-mode mix this check measures.

`ApcGrid::bindAll` needs `ps.bind("fx/resonode/collision", 0.0f)` alongside
the other five `fx/resonode/*` binds — per the "`ApcGrid::bindAll` must
`ps.bind()` every internal flag" entry below, `applyResonodePatchMorph`'s
new `ps.setByName("fx/resonode/collision", ...)` call would otherwise be a
silent no-op on real hardware exactly like the historical `fx/resonode/
engaged` bug. Pitch-mod needs no new bind — it has no C++-side control at
all, it is computed entirely inside the DSP from the existing `vel` signal
input and the existing `stretch` hslider.

## CC53 formant constants (must match `../looper` exactly)

Deadzone 60-68, range ±1 unshifted / ±3 shifted, formula
`((data2-64)/63.0)*range`. (Not 62-65, not ±1.5, not `/63.5`.)

---

# Storage: continuous USB-drive ring recording

`src/storage/usb_recorder.{h,cpp}`. Nothing in this tree handled USB mass-storage
detection or mounting before it; `src/usb/f_uac2-gadget.sh` is a completely
different USB role (peripheral/gadget mode on the micro-USB port vs. host mode on
the USB-A ports a flash drive plugs into).

**RT side**: `UsbRecorder` owns a fixed, heap-allocated `int16_t` ring (5 seconds).
`audio_thread.cpp`'s worker calls `pushBlock(prevFiltOut.data(), N)` every block,
next to `g_sampler->captureBlock(...)` — the same post-fx tap point. The producer is
a single-atomic-counter SPSC ring (`std::atomic<uint64_t>` write/read counters, not
raw indices, so full-vs-empty is unambiguous) that NEVER blocks or allocates: if the
consumer has fallen behind, `pushBlock` advances the read counter itself (dropping
oldest samples) and increments an overrun counter. Drop, never block.

**Control side**: all file I/O (mount detection, WAV chunk writing/rotation) happens
in `UsbRecorder::poll()`, called from `main.cpp`'s existing 5 Hz control loop
alongside `telem.publish()`/`remote.poll()` — deliberately NOT a dedicated pthread,
matching `Telemetry`/`RemoteControl`'s shape. The ~200ms cadence absorbs a blocking
USB write; the 5-second ring absorbs a slow iteration. Chunks are fixed-size and
cyclically `O_TRUNC`-reopened, so the ring bounds disk usage by construction with no
eviction pass.

**Mount detection is a `stat()` device-id comparison** (`isMounted()`: the mount
point's `st_dev` differs from its parent's exactly when something is mounted there,
the same technique `mountpoint` uses), not `/proc/mounts` parsing.

**Config**: `[storage]` in `config/aloop.conf` — `usb_record`, `usb_mount_point`
(default `/media/aloop-usb`), `usb_chunk_minutes` (10), `usb_chunk_count` (6),
following `loadConfig()`'s existing sscanf pattern in `src/main.cpp`.
`effectiveChunkCount()` shrinks the ring to fit smaller drives via `statvfs`.

**Automount**: `src/usb/usb-automount.sh` (mdev hotplug) +
`src/usb/usb-automount-setup.sh` (local.d bootstrap). The setup script APPENDS two
rules to `/etc/mdev.conf` if not already present — never overwrites, since Alpine's
stock `mdev.conf` drives the base system's own device-node population. Because
`local.d` (boot runlevel) runs AFTER `mdev -s`'s sysinit coldplug scan, an
already-inserted drive would be missed, so the setup script does its own explicit
coldplug pass over `/dev/sd[a-z][0-9]*` after installing the rule.

Mount attempts: no `-t` first (kernel auto-detection), then explicit
`-t vfat`/`ext4`/`exfat`/`ntfs`. **exFAT/NTFS userspace tools are almost certainly
NOT in the minimal Alpine RPi tarball's repo**, so only kernel-native FAT32/ext4 is
expected to work without further vendoring. UNVERIFIED on real hardware, along with
the mdev.conf rule syntax and real USB-drive enumeration on the Pi 4's USB-A ports.

`./opt/aloop/usb-automount.sh` and `./etc/local.d/25-usb-automount.start` are
registered in BOTH `_exec_paths` and `_nb_exec_paths`.

---

# Faust Libraries reference

Faust Libraries is the standard DSP library collection for the Faust language.
Prefer the Markdown sources over the built HTML for LLM-friendly content.

### Core entrypoints
- [Libraries index](https://faustlibraries.grame.fr/libs/): Index of all library
  reference pages.
- [Standard functions](https://faustlibraries.grame.fr/standardFunctions/): Core
  standard functions used across the libraries.
- [Overview](https://faustlibraries.grame.fr/organization/): High-level
  organization and structure of the library.
- [Motion functions](https://faustlibraries.grame.fr/motion_functions/):
  Motion-related functions and reference.

### Library map
- Each library has a dedicated reference page under `doc/docs/libs/` (Markdown
  source) and `/libs/` (HTML site).

### Markdown sources (authoritative)
- [Libraries index (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/libs/index.md)
- [Libraries example (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/refs/heads/master/doc/docs/libs/basics.md)
- [Libraries folder (API)](https://api.github.com/repos/grame-cncm/faustlibraries/contents/doc/docs/libs)
- [Standard functions (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/standardFunctions.md)
- [Overview (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/organization.md)

### Scope
- This section documents the Faust **libraries** only. Compiler-flag guidance lives
  in the "Faust compiler flags" sections above; the reference for those is
  [faustdoc.grame.fr/manual/optimizing/](https://faustdoc.grame.fr/manual/optimizing/).

### Optional
- [Contributing](https://faustlibraries.grame.fr/contributing/)
- [Community](https://faustlibraries.grame.fr/community/)
- [About](https://faustlibraries.grame.fr/about/)

@.gm/next-step.md
