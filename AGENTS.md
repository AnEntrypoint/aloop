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

## Guitar-fx held REDIRECTS looper pad presses entirely

While `m_guitarFxHeld` is true, a looper pad press is consumed by
`onSidechainLooperToggle` (toggling that looper's sidechain-source designation) and
never reaches the ARM/FINISH dispatch — it does not touch
`m_looperHeld`/`m_looperHoldStart` at all, since this is a one-shot toggle, not a
hold gesture. The sidechain-source designation auto-clears whenever that looper's
content is wiped (long-hold erase or CLEAR_ALL).

## Objekt-style granulator: hold-to-engage + patch morph

The LofiFx bank (`kApcBtnLofiFx`, note 69) is a momentary hold gesture, not a
tap-select bank button like dub-fx/guitar-fx. `onLofiFxPress` immediately
switches the active bank to LofiFx and calls `setGranulatorEnabled(true)`;
`onLofiFxRelease` reverts the active bank to whatever it was before the press
and calls `setGranulatorEnabled(false)`. This replaces an earlier SHIFT+tap
permanent toggle — the granular texture is now present only while the
performer physically holds the button, matching how a real Objekt-style
groovebox is played (press-and-hold a gesture, release to drop back to plain
sample playback), and keeps the 7 physical knobs pointed at the granulator's
own controls only for the duration of that hold.

Knob slot 0 (`fx2/BITCRUSHAMT`) is unchanged. Slots 1-6 no longer map 1:1 to
raw grain parameters (grain size/density/scan rate/pitch spray/position
jitter/reverse probability) — turning six independent raw sliders to their
extremes simultaneously produced an incoherent, un-musical result. Instead
each slot is the BLEND WEIGHT of one of 6 fixed named patches
(`kGranPatches` in `apc_grid.cpp`: Glass, Cloud, Freeze, Chop, Tape,
Shatter — each a full point in grain-size/density/spray/jitter/scan/reverse
space representing a distinct musical character). `applyGranulatorMorph`
computes a weighted average across all 6 patches (weights normalized by
their sum, a convex combination) and pushes the single resulting blended
point into the Sampler's existing 6 setters. All weights at 0 falls back to
patch 0 (Glass, closest to a plain/transparent read) rather than dividing by
zero. This means turning up two patch dials together always yields a
coherent midpoint texture instead of two raw parameters fighting each other,
and dialing every patch to max still yields a bounded, sane blend (never the
sum of six maxed-out raw parameters at once).

Real MIDI velocity (previously hardcoded to 127 in `onKeybedNoteOn`, the real
`d2` byte discarded at the `midi.cpp` call site) now reaches
`Sampler::_noteOn` and scales `Voice::velGain`: overall voice loudness for
every voice (granular and plain alike), plus (granular voices only) grain
spawn density via `densityFromVel = 0.4 + 0.6*velGain` in
`_renderGranularVoice` — a harder key press plays louder AND spawns a denser,
brighter grain cloud, the dynamic-response feel real granular groovebox
hardware has and this sampler never had.

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
