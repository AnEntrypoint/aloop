# aloop — agent debugging caveats

Hard-won gotchas from live debugging on the real Pi 4 hardware (192.168.137.100,
root/aloop). Read this before touching the device or its build/deploy pipeline —
every entry here cost real time to discover once; don't rediscover it.

## Multi-board release architecture: BOARD-parameterized lib-boot-tree.sh

`image/lib-boot-tree.sh` is the one source of truth for every board's boot tree,
dispatched by a `BOARD` env var (`pi3`/`pi4`/`pi5`/`opi-prime`, default `pi4`).
`boot_tree_apkovl` (the aloop payload — binary, LV2, services, vendored libs) is
100% shared and unconditional: it is architecture-independent aarch64 userspace
content regardless of board. Only `boot_tree_fetch` (firmware/kernel/DTB) and
`boot_tree_config` (boot cmdline/USB-gadget config) dispatch per board, since
those are the genuinely board-specific pieces.

**Capability matrix** (`board_supports_usb_gadget`/`board_wifi_irq_name` in
`lib-boot-tree.sh` are the authoritative source, not this table — check the code
if either ever needs to change):

| Board | SoC | Boot chain | USB-audio gadget | WiFi chip |
|---|---|---|---|---|
| pi4 (+CM4, Zero2) | BCM2711, quad Cortex-A72 aarch64 | Pi firmware, FAT partition | dwc2 peripheral — real UAC2 gadget | Broadcom brcmfmac |
| pi3 | BCM2837, quad Cortex-A53 aarch64 | Pi firmware, FAT partition | none — no OTG-capable controller | Broadcom brcmfmac |
| pi5 | BCM2712, quad Cortex-A76 aarch64 | Pi firmware, FAT partition | none — RP1 southbridge USB is host-only | Broadcom brcmfmac |
| opi-prime | Allwinner H5, quad Cortex-A53 aarch64 | Armbian-sourced U-Boot (raw SD sectors) + ext4 root + extlinux.conf | unproven | Realtek RTL8723BS |

**Orange Pi Prime uses Allwinner H5, not H3** — an easy premise error (H3 powers
the cheaper Orange Pi PC/One/Zero/Lite boards; the Prime specifically is H5,
confirmed via the mainline kernel's own `sun50i-h5-orangepi-prime.dts`). This
matters: H5 is 64-bit Cortex-A53 aarch64, architecturally much closer to the Pi
4's aarch64 toolchain than the 32-bit ARMv7 Cortex-A7 H3 would have been — no
32-bit userland detour needed, Alpine's existing aarch64 packages apply directly.

**USB-audio-gadget mode is UNPROVEN on Orange Pi Prime, not confirmed working.**
The Allwinner H5 uses a MUSB dual-role controller (`sunxi-musb` glue,
`drivers/usb/musb/sunxi.c`) on the micro-USB OTG port (the 3 full-size USB-A ports
are host-only EHCI/OHCI, wired to a separate controller — they can never do
gadget mode). Generic Linux `libcomposite`/`f_uac2` gadget audio is
architecture-independent so nothing in principle blocks UAC2 once peripheral mode
is confirmed working, but no source found this session directly confirms `f_uac2`
has ever been run on H3/H5 — this is a real, open feasibility gap. Until proven,
`board_supports_usb_gadget` returns false for `opi-prime` and the fallback audio
path is the board's built-in analog codec (3.5mm line-in/mic-in,
line/headphone-out) run as a normal ALSA HOST device — a materially different,
lower-risk architecture than gadget-mode UAC2, but abandons the "looks like a USB
soundcard to a laptop" design. If a future session proves gadget-mode UAC2 works
on real Orange Pi Prime hardware, update `board_supports_usb_gadget` to add
`opi-prime` to the true case and this note.

**Orange Pi Prime's boot chain is structurally incompatible with the Pi's
FAT-partition firmware model** — Allwinner's BootROM reads a raw SPL/U-Boot image
at a FIXED RAW SD SECTOR OFFSET (`dd ... seek=8`, 1K blocks) before any partition
table exists; there is no `config.txt`-equivalent firmware file. `lib-boot-tree.sh`
handles this via `boot_tree_fetch_opi`/`boot_tree_config_opi`: it downloads
Armbian's official `dl.armbian.com/orangepiprime/Trixie_current_minimal` **stable
redirect URL** (verified real this session) — never a resolved
`github.com/armbian/community/releases/...` asset URL, since Armbian's rolling
trunk builds move that version string on every build — decompresses it, reads the
image's own real partition table via `sfdisk` (never assumes a fixed offset),
extracts the raw pre-partition-1 region as the U-Boot blob, and real-loop-mounts
the ext4 root partition to pull the kernel/`sun50i-h5-orangepi-prime.dtb`/initrd
out of `/boot`. `boot_tree_config_opi` writes a U-Boot-style
`/boot/extlinux/extlinux.conf` (the same distro-boot mechanism syslinux/grub2
bootloaders use) carrying the SAME isolcpus/RT kernel-cmdline tuning as the Pi
boards' `cmdline.txt`, since that's a real, board-independent kernel parameter —
only the delivery mechanism differs. `image/build-image.sh`'s `opi-prime` branch
assembles a raw-U-Boot-at-sector-8KiB + ext4-root-partition image instead of the
Pi boards' FAT32/mtools image; this needs real root (`sudo losetup`/`mount`) and
only runs in CI (or any real Linux host with root), never on this Windows dev
host. `image/validate-image.sh` mirrors this split: checks the `eGON.BT0` SPL
magic at the real write offset + an MBR type-83 partition + loop-mounts the ext4
root to check `extlinux.conf`/dtb/apkovl content, instead of the Pi boards'
mtools/FAT32 checks.

**Orange Pi Prime has no netboot path — SD-card-flash-only.** Unlike the Pi's
native TFTP+HTTP netboot firmware (VideoCore EEPROM, boot-order NETWORK),
Allwinner's BootROM requires U-Boot to already be resident on local media
(SD/eMMC/SPI) before PXE/TFTP can even be reached — a bespoke DIY setup, not a
drop-in equivalent. `build-image.yml`'s netboot-build/validate/SD-zip steps are
all conditionally skipped for `BOARD=opi-prime`; only its raw `.img.gz` is
produced and released.

**Orange Pi Prime's WiFi is Realtek RTL8723BS, not Broadcom brcmfmac.**
`kernel/rt-tune.sh`'s IRQ-steering (which keeps WiFi/network interrupts off the
isolated audio cores) matches by driver-name substring, not fixed IRQ numbers, so
it needed `rtl8723bs` added to its grep pattern alongside the existing `brcmfmac`
— any Ableton Link multicast behavior tuned against Broadcom's driver on the Pi
boards should be re-validated against this different driver/chip if Link sync
accuracy is ever questioned on Orange Pi Prime specifically.

## `lib-boot-tree.sh`/`build-netboot.sh` boot-tree assembly: durable facts behind the shared apkovl/cmdline/gadget code

**`boot_tree_apkovl` must stamp `.default_boot_services`.** WITNESSED live on a
real Pi 4: shipping our own apkovl with runlevels already populated silently
opts OUT of Alpine's own `rc_add modloop sysinit` gate (which also enables
devfs/dmesg/mdev/hwdrivers — the whole hardware-bring-up layer), because that
gate is conditioned on `[ -f "$sysroot/etc/.default_boot_services" -o ! -f
"$ovl" ]` — i.e. it only fires for a fresh/no-apkovl boot unless the apkovl
itself carries this marker asking init to still enable them (init removes the
marker after reading it, a documented one-shot Alpine mechanism). Without it,
`/lib/modules` stayed empty, `/proc/asound` never existed, and
`/sys/kernel/config/usb_gadget/` could not be created, even though
`modloop-rpi` was fetched successfully.

**`aloop`'s OpenRC service needs `rc_ulimit="-l unlimited -r 95"`, not a
`local.d` `ulimit` call.** `kernel/rt-tune.sh` sets `ulimit -l unlimited`
(memlock, needed for `mlockall(MCL_FUTURE)`), but that runs inside a
`local.d/*.start` script which OpenRC's `local` service `eval`s in a
transient subshell that exits once `local`'s own `start()` returns — the
ulimit change never reaches the separately-started `aloop` process
(WITNESSED: `ulimit -l` on a booted device showed the 8192 KB default, not
unlimited). `rc_ulimit` is OpenRC's own per-service ulimit mechanism, read by
`openrc-run.sh` itself immediately before it execs `command` — the correct
place for a limit the service's own process needs.

**`aloop`'s `depend()` needs `after autoap`, matching esp-idf-link's own
interface-settle defenses.** `aloop` constructs `ableton::Link` (and its UDP
multicast socket) during startup. With both services declaring only `after
local`, OpenRC was free to start them in either order or in parallel, so Link
could open its socket before `autoap` had brought `wlan0` up. `../esp-idf-link`
treats this exact race as real and defends against it twice (a 500ms
interface-settle before constructing Link, plus re-asserting IGMP membership
for ~10s after every connection). Ordering `aloop` after `autoap` is the cheap
half of the same defense; `src/main.cpp` additionally waits for the interface
to carry an address before starting Link.

**The apkovl must vendor alsa-lib + the whole lilv stack as real `.so` files,
never rely on `apk add` at boot.** WITNESSED live: the device's only reachable
apk repo is the ~100-package minimal set bundled in the Alpine RPi tarball
(no CDN fallback) — none of `alsa-lib`/`lilv-libs`/`serd-libs`/`sord-libs`/
`sratom`/`zix-libs` are in it, so the aloop binary could never dynamically
link (telemetry never came up after a full successful boot). Fixed by
bundling the real musl-aarch64 `.so` files directly under `usr/lib/`
(`vendor/lib-aarch64/`, fetched from the exact Alpine 3.20 CDN versions CI
builds against).

**alsa-lib also needs its own DATA tree (`/usr/share/alsa/alsa.conf`), not
just `libasound.so`.** WITNESSED live: with `libasound.so.2` vendored but no
`alsa.conf`, calling `snd_pcm_open("default", ...)` segfaults deep inside
alsa-lib's config parser — `"default"` is an ALIAS defined in `alsa.conf`,
with nothing to resolve it against otherwise (confirmed via alsa-lib's own
stderr: `Cannot access file /usr/share/alsa/alsa.conf`). The whole
`vendor/share-alsa/` data tree (~340K) is vendored rather than guessing which
of `alsa.conf`'s `@hooks`/includes are load-bearing.

**Every `cmdline.txt`/`extlinux.conf` APPEND write must stay a single line —
the Pi firmware and U-Boot both read only line 1.** An embedded newline
silently truncates every kernel param after it. WITNESSED: an early version
appended the RT cmdline fragment raw, leaving an embedded newline between the
stock Alpine `cmdline.txt` (which already ends `\n`) and the appended RT
fragment — this dropped `isolcpus` and, for netboot, `ip=dhcp`/
`alpine_repo`/`modloop`/`apkovl` entirely, and the Pi never ran the
initramfs DHCP, dropping to an emergency shell. Every writer collapses both
halves via `tr '\n' ' '` + `tr -s ' '` before re-emitting one line with a
single trailing newline — this discipline is repeated in
`boot_tree_config`/`boot_tree_config_opi`/`build-netboot.sh`'s netboot-cmdline
and `NETBOOT_DEBUG` steps, and both `validate-image.sh`/`validate-netboot.sh`
assert it by counting newlines.

**`build-netboot.sh`'s netboot-root publish is a staged-directory atomic
`mv`, never `rm -rf` + populate-in-place.** `image/serve-netboot-win.js` can
rebuild the netboot root while a Pi is actively TFTP/HTTP-fetching from it
(WITNESSED: a real Pi 4's boot-chain fetch was served concurrently with a
live rebuild) — an in-place `rm -rf`/`cp -a` leaves a window where the served
tree is empty or half-copied, and an in-flight read can get a spurious
"not found" or a truncated file. `mv` between two directories on the SAME
filesystem is a single atomic `rename(2)` (POSIX-guaranteed), so the staging
directory is built as a SIBLING of the real output dir (same parent, same
filesystem) — never under `mktemp -d`'s `$WORK`, which typically lands on a
different mount and would silently degrade the swap to a non-atomic
copy+delete.

**The netboot root must be `chmod -R a+rX`'d after copy.** The Alpine tarball
ships `boot/initramfs-rpi` as mode 600 (root-only), and `cp -a` preserves
that. A TFTP server runs unprivileged (dnsmasq drops to `nobody`), so without
this fix it gets "Permission denied" on the initramfs and the Pi boots a
kernel with no initramfs, panicking "unable to mount root fs".

## `src/usb/f_uac2-gadget.sh`: configfs UAC2 setup facts

Replaces looper's hand-rolled UAC2 bring-up (ADR-008) — the kernel's `f_uac2`
function lays out isochronous USB microframes correctly by construction,
eliminating the buzz/crackle/-4608 corruption class the bare-metal looper had
to find and fix by hand. Runs at boot from `/etc/local.d` after `libcomposite`
loads.

The gadget presents a STEREO wire (`c_chmask`/`p_chmask = 0x3`, L+R) matching
looper's own UAC2 exactly — `audio_thread.cpp`'s `wireCh` handling averages
capture L/R down to mono for the Faust DSP and duplicates the mono result
onto both channels on playback, so the host sees a normal stereo soundcard
while the DSP itself stays mono internally.

**`req_number` (the f_uac2 driver's own isochronous USB request queue depth,
separate from ALSA's `buffer_size`/`period_size`) must be raised from the
kernel default of 2 to 4.** WITNESSED live: the default of 2 silently capped
ALSA's negotiated `buffer_size` at 256 frames no matter what
`audio_thread.cpp`'s `hw_params` requested, producing hundreds of xruns/sec
once the ALSA period was tightened to match `block_size` (the `aloop.conf`
`audio_device` fix). Raising `req_number` to 4 gives the gadget's own USB
transfer queue the same headroom the ALSA-side fix intended, so the two
actually compound instead of one silently overriding the other.

## No comments in code, ever — self-explanatory code replaces them

Same absolute policy as `../gm`'s own AGENTS.md. A name, a function boundary,
an extracted variable, or a small type IS the explanation — prefer
renaming/restructuring over annotating, every time the urge to comment
appears. No inline, block, or doc comments anywhere (C++, Faust `.dsp`,
JS, shell, YAML, config). A multi-line or paragraph-long comment is the
same violation at higher volume, not a lesser one — explaining a "why" is
not an exemption; the urge to explain is itself the signal that a name or
structure is doing too little work, so restructure instead of narrating
around the gap.

Hardware quirks, historical root-causes, design-decision rationale, and
any fact that genuinely needs to survive across sessions belongs in THIS
file (AGENTS.md) or `.wfgy/lessons.md`, never inline next to the code —
this file is exactly where hardware quirks and memorables go.

A comment encountered anywhere — pre-existing, another session's, a
vendored copy — is converted to self-explanatory code the moment it's
seen, same turn, not left for a later cleanup pass: read it, understand
what it was compensating for, fix that root cause so the comment's
content becomes redundant, then delete it. "Already there, not part of
this task" is not an exemption — one sighting spawns a full-tree sweep of
that file (and, when time allows, nearby files sharing the same pattern).

## Never add audio-path latency to fix anything

The pre-LOFI baseline (commit `4cb6587`) was measured **100% hitch-free** on
real hardware. The existing ~7ms block latency is already considered too much
by the user — it must never grow, not even temporarily, not even to work
around an unrelated bug. If a fix seems to require a bigger ALSA
buffer/period, more block lag, or any added buffering stage, stop and ask
before landing it. Any audio glitch found after this baseline is a real
regression to root-cause, not a hardware limit to negotiate around.

## The REBOOT:<token> UDP listener lives INSIDE the aloop process

`config/aloop.conf`'s `[remote] token=` enables a `udp/4446` listener
(`src/control/remote_control.cpp`) that accepts `REBOOT:<token>` and reboots
the Pi — but this listener is part of the `aloop` binary itself. **If `aloop`
has crashed, nothing is listening, and `image/aloop-reboot.js` silently does
nothing at all** (no error, no timeout — the UDP packet just goes nowhere).
`/etc/init.d/aloop`'s `respawn_max=0` means OpenRC will NOT restart a crashed
`aloop` on its own either, so a crashed device stays crashed indefinitely
unless something else reboots it.

**Symptom this caused, live**: several consecutive `aloop-reboot.js` calls
appeared to "not pick up" a freshly-deployed binary — in reality the device
never rebooted at all; it kept running the last binary that successfully
booted, for over 20 minutes across multiple deploy attempts, while every
`REBOOT:<token>` packet vanished into a dead process.

**Always verify a reboot actually happened before trusting any "device state"
observation**: check `cat /proc/uptime` (should be small/recent) and
`md5sum /opt/aloop/aloop` against the binary you just deployed. Do this
BEFORE reading logs or drawing conclusions from device state — a stale
device silently produces stale, misleading data that looks like a fresh
test.

**If `rc-service aloop status` shows `crashed`, `REBOOT:<token>` cannot work.**
Use a real SSH-triggered reboot instead: `node ssh-exec.js 192.168.137.100
"reboot"` (see the JS SSH client below). Only use the UDP REBOOT path once
`aloop` is confirmed actually running.

## SSH access: use the JS client, never Windows ssh.exe or sshpass

Password auth (root/aloop), not key auth. The user has explicitly rejected
both bare Windows `ssh.exe` (pops up asking for a password) and an
sshpass-wrapping approach — use a pure-JS `ssh2`-based client instead
(`npm install ssh2` in the scratchpad, then a small script that does
`new Client().connect({host, port:22, username:'root', password:'aloop', ...})`).
A fresh netboot generates a new host key every boot, which breaks raw
`ssh`/known_hosts but doesn't affect `ssh2` (it doesn't consult
`~/.ssh/known_hosts` the same way).

## The device runs Alpine/musl/aarch64 — glibc/x86_64 build artifacts silently fail to load

`build-lv2.yml`'s original `faust2lv2 <file>.dsp` step ran on bare
`ubuntu-latest`, compiling the `.so` with the HOST's own g++ (glibc/x86_64).
The device (Alpine, aarch64) can dlopen the resulting `.so` with no
bundle-discovery error, but it fails at actual load time:
`Error relocating .../foo.so: unsupported relocation type 7`. This meant
**every** home-FX LV2 effect (not just anything added later) had never
actually reached the device's audio path, ever — CI staying green only ever
meant "the x86_64 build compiled," never "the plugin runs on target."

Fix pattern (see `.github/workflows/build-lv2.yml`): `faust2lv2`'s own script
(`which faust2lv2`) cleanly separates concerns — `faust -i -a lv2.cpp ...`
emits a self-contained `.cpp` (verified: only libc/libstdc++/lv2/boost
includes, no Faust dependency), a `$HOST_CXX` compile+run of that same `.cpp`
emits the plugin's `.ttl` metadata (runs on the host only, never touches
target arch/libc), and only the FINAL `-shared .so` link needs to target the
real device. Cross-compile that one step inside a real Alpine aarch64
container via `docker/setup-qemu-action` + `docker run --platform linux/arm64
alpine:3.20`, matching `build-binary.yml`'s own already-proven pattern.
Verify the result is genuinely target arch:
`objdump -p foo.so | grep NEEDED` should show `libc.musl-aarch64.so.1`, never
`libc.so.6`.

**Passing `CPPFLAGS` (which contains escaped quotes, e.g.
`-DPLUGIN_URI=\"...\"`) into a nested `docker run ... sh -c "..."` string via
shell interpolation loses the escapes across the nested-shell boundary** —
`PLUGIN_URI` expands to bare unquoted text and the compiler tries to parse
the URL as code (`'https' was not declared in this scope`). Pass such values
via `docker run -e VAR="$VAR"` instead of string-interpolating them into the
outer script.

## `actions/upload-artifact@v4`'s `path:` wildcard-vs-literal behavior

`path: effects/home/*.lv2` (a wildcard match) zips the matched directory
WITH its own basename preserved inside the archive. `path:
effects/home/guitar_lofi_fx.lv2` (a literal, non-wildcard single-directory
path) zips that directory's CONTENTS flattened at the zip root instead,
silently dropping the `.lv2/` wrapper. A CI job that changes from wildcard to
literal path for no functional reason will silently break every downstream
consumer that expects the `.lv2/` directory to exist inside the artifact.
Always use the wildcard form for LV2 bundle artifacts.

## LV2 hosting: never pass a bare `nullptr` for the features array

`Lv2Host::instantiate()` (`src/host/lv2_host.cpp`) must pass a real,
NULL-TERMINATED `LV2_Feature* const*` array to `d->instantiate(...)` — never
a bare `nullptr`. Faust's generated `lv2.cpp` architecture does
`for (int i = 0; features[i]; i++)` with no null-check on `features` itself,
so a bare `nullptr` deref's immediately at `features[0]` — genuine SIGSEGV
(exit 139), reproducible on every single plugin load. This bug existed since
before this session but never surfaced because no LV2 plugin had ever
successfully dlopen()'d until the musl/aarch64 CI fix above landed — so
fixing the CI packaging bug is what turned a previously-silent bug into a
crash. Use `static const LV2_Feature* const kNoFeatures[] = { nullptr };` (a
real, valid, empty-but-terminated array) instead.

Also wrap `instantiate()`/`activate()` in the same sigsetjmp crash-isolation
watchdog `runOne()` already uses (ADR-002) — a bad plugin crashing during
LOAD is just as much "untrusted code" as one crashing during `run()`, and
taking the whole process down during load is strictly worse than degrading
gracefully.

## `core.autocrlf=true` on this Windows clone silently corrupts shell scripts

Editing/re-checking-out any `.sh`/`.start` file on this Windows machine can
silently convert its line endings to CRLF. Alpine's busybox ash chokes on
`#!/bin/sh\r` (breaks the shebang lookup) and every line's trailing `\r`
merges into the next token (`illegal option -`, `: not found`). This broke
`kernel/rt-tune.sh`'s boot-time CPU-governor pin with **zero visible error
anywhere in the pipeline** — CI stayed green (nothing there runs the
script), the packaging step (`image/lib-boot-tree.sh`) just copies bytes —
the only symptom was the real device silently still running `schedutil`
instead of `performance`.

Fixed with a repo-level `.gitattributes` forcing `eol=lf` on `*.sh *.start
*.conf *.yml *.yaml Makefile cmdline.txt config.txt usercfg.txt` (already
committed). If a shell script is ever edited and behaves strangely on the
device despite looking correct in the editor, check for CRLF first:
`file path/to/script.sh` should say "with CRLF line terminators" if
corrupted; fix via `rm path/to/script.sh && git checkout -- path/to/script.sh`
(the checkout re-applies the now-correct `.gitattributes` rule).

## The netboot self-update pipeline: two different rebuild paths, easy to confuse

- **Automatic path**: `image/serve-netboot-win.js` (run elevated, needs
  `GITHUB_TOKEN`/`gh auth token` and `PI_TOKEN` env vars) polls
  `build-binary.yml`/`build-lv2.yml`'s latest green run on `main` every 30s,
  downloads BOTH artifacts into `.netboot-update-work/{bin,lv2}`, and calls
  `image/build-netboot.sh` itself when the combined SHA changes. It tracks
  state in `.netboot-update-sha` (format `<binSha>:<lv2Sha>`) — if this file's
  content already matches the latest state, the poll loop does nothing, ever,
  even if `.netboot-serve/`'s actual content is stale/wrong/manually
  overwritten.
- **Manual path** (used for A/B testing specific commits, e.g. bisecting a
  regression): `ALOOP_BIN=<path> LV2_DIR=<path> OUT=.netboot-serve
  NETBOOT_SERVER=192.168.137.1 bash image/build-netboot.sh` rebuilds
  `.netboot-serve/` directly from arbitrary local artifacts, bypassing the
  poll loop entirely.

**Always verify the ACTUAL deployed binary/bundle checksum matches what you
intended, immediately after every manual rebuild, BEFORE rebooting** —
`tar -xzf .netboot-serve/aloop.apkovl.tar.gz -C <fresh-empty-dir>
./opt/aloop/aloop && md5sum <fresh-empty-dir>/opt/aloop/aloop` vs the source
binary. Extracting to stdout (`-O`) or reusing a not-freshly-emptied
extraction directory can silently compare against stale leftover files from
an earlier extraction and give a false-positive match. This was the actual
cause of several confusing "wrong binary keeps getting deployed" incidents
in one session — the rebuild and the checksum were both real, but a stale
comparison target made a correct rebuild look like it had failed, OR (the
more dangerous direction) masked a genuinely stale device that never
rebooted at all (see the REBOOT-listener caveat above — always cross-check
`/proc/uptime` too, not just the checksum, since a checksum match against
`.netboot-serve/` proves the SERVER state, not that the DEVICE actually
picked it up).

When manually testing an old/historical commit for bisection purposes, that
commit's binary won't have current fixes — expect it to crash if it predates
the nullptr-features fix above and any LV2 bundle is present in
`/effects/home` or `/effects/user`; this is expected, not a new bug.

**The automatic path's SHA-tracking is blind to changes in the packaging
scripts themselves** (`image/lib-boot-tree.sh`, `image/build-netboot.sh`) —
it only compares `build-binary.yml`/`build-lv2.yml`'s own latest green-run
SHAs, and neither workflow lists `image/**` in its trigger `paths:`.
WITNESSED live: a real fix to `lib-boot-tree.sh` (excluding a
duplicate-DSP `.lv2` bundle from what gets packaged — see the
`aloop.lv2` entry below) was committed and pushed, but the auto-updater's
`.netboot-update-sha` never changed (neither workflow re-ran), so
`.netboot-serve/`'s actual packaged content silently stayed on the OLD,
unfixed packaging indefinitely — a device that rebooted from netboot
picked the stale image back up, undoing a fix that had already been
verified live via manual on-device patching minutes earlier. Any change to
the packaging scripts themselves (not just `dsp/`/`effects/`/`src/dsp/`
content) requires a **manual** `.netboot-serve/` rebuild
(`ALOOP_BIN=<path> LV2_DIR=<path> OUT=.netboot-serve
NETBOOT_SERVER=192.168.137.1 bash image/build-netboot.sh`, using
already-downloaded `.netboot-update-work/{bin,lv2}` artifacts if the
binary/LV2 content itself hasn't changed) — do not assume the automatic
poll loop will ever pick it up on its own.

## Diagnosing periodic audio stalls: always add wall-clock timestamps, not just magnitudes

A gap-logging line that only fires when the gap exceeds some threshold
(`[diag-gap] readi gap=X ms`) has line-count density that is NOT a reliable
proxy for real elapsed time — quiet stretches between events are invisible
in the log, and a burst of activity can look identical in line-count to a
long quiet stretch with one big spike. Always log a wall-clock timestamp
(`clock_gettime(CLOCK_MONOTONIC, ...)`, printed as `t=<sec>.<ms>`) alongside
the magnitude — this is what let a "seems like it's happening often" symptom
resolve into a hard, provable "fires at almost exactly a 1.000-second
period" measurement.

**Update (2026-07-25): the ~1Hz periodic stall documented above no longer
reproduces** on the fully-fixed device (all 3 fixes below applied, live,
survived a real reboot). Re-ran `test/hardware/bisect-1hz-stall.js` (after
fixing two real bugs in the tool itself, see below) three times: 20s and
60s captures with the Core-3 LV2 host both active and disabled (identical
in both configurations — 2 minor diag-gap lines each, zero "big" ≥10ms
events), plus a clean 90-second direct log capture showing ZERO diag-gap
lines at all. Given the stall's own documented signature is "fires at
almost exactly a 1.000-second period," a 60s+ clean window is decisive —
at the previously-measured rate it would have fired ~60+ times. No
specific mechanism was proven (this is an A/B-disappeared result, not a
root-cause trace), but the most likely explanation is that it was caused
or exacerbated by one of the 3 bugs below — most plausibly the MIDI-remap
loop's growing per-block cost (bug #1), which could plausibly have
produced periodic pressure spikes as its cost scaled with bound-control
count, or contention from the pre-fix `aloop.lv2` duplicate-DSP load (bug
#3) intermittently competing for the same core. Do not re-open this as
"still unresolved" without a fresh live re-test first — the evidence
currently on hand says it is gone.

Two real, unrelated bugs were found and fixed IN the bisection tool itself
while re-running it: (1) `writeFileContent()`'s SFTP `rename()` call failed
with a bare `Failure` status when the destination already existed (it
always does — `/etc/aloop.conf` is never missing) — SFTPv3's rename has no
POSIX atomic-overwrite guarantee; fixed by `unlink()`-ing the destination
first. (2) The "is `disable_core3_lv2` already active" detector's regex
matched the shipped config's own commented-out documentation line
(`# disable_core3_lv2 = 1      # DIAGNOSTIC ONLY: ...`), producing a
false-positive warning on every normal, unmodified config; fixed by
anchoring the regex to line-start with no leading `#`.

## Severe continuous readi()-slowdown (NOT the ~1Hz periodic stall above — a distinct, separate issue, now resolved): a per-block string/map-lookup path scaling with control count, not USB hardware

A different symptom from the periodic-stall section above: `readi()` itself
taking 2.2-2.7ms against a 1.333ms expectation, continuously (not
periodically), with xruns climbing without bound, reproducible at idle with
nothing recording or playing. An initial investigation pass wrongly
concluded this was an inherent USB full-duplex isochronous-transfer-timing
hardware limitation, "not something fixable in software" — that conclusion
was wrong. The real cause: `worker()`'s "apply remappable controls" block
(`src/dsp/audio_thread.cpp`) called `targetToZone()` (a `std::string`
allocation + `snprintf`) then two string-keyed map lookups
(`ParamStore::get` by name, then `FaustUI::set`'s `zones.find` with an O(n)
linear suffix-scan fallback) for **every bound MIDI/effect control, every
single audio block** (750/sec) — unlike `sidechainSrcSlot` a few lines
above it (already a correct resolve-once-per-slot cache), this exact path
was never converted, so its cost scaled directly with how many
controls/effects have been bound over the project's life. The block's own
comment claimed this was already cheap and alloc-free; it was neither —
never trust an in-repo comment's claim of "already optimized" over actually
reading what the code does (see the "verify the spec" lesson below).

**How this was actually found** (not by guessing harder at the USB
hypothesis, but by checking a specific, previously-unchecked signal):
`/proc/<tid>/schedstat`'s `sum_exec_runtime` showed the RT audio-worker
thread on-CPU ~95% of wall-clock uptime, but with only ~46 voluntary
context switches/sec — a thread genuinely blocking-and-waking once per
`readi()` call at 750 blocks/sec should show ~750 voluntary sleeps/sec, not
46. A thread that's supposedly blocked in a real hardware wait sleeps; one
that's actually spinning/working shows exactly this low-voluntary-switch,
high-on-CPU signature. That pointed straight at the **untimed** span
between `readi()` returning and the already-instrumented `compute()` timer
starting — the remap loop lives exactly there.

**Fix**: cache each bound target's resolved `(ParamStore slot, Faust zone
float*)` pair once, rebuilt only when `ParamStore::count` grows (a new
mapping gets bound) — the identical lazy-resolve-while-unresolved
discipline `sidechainSrcSlot` already used. The per-block hot path is now a
flat array walk: one atomic load + one pointer store per control, zero
string construction, zero map lookups, zero allocation.

**Verified live on the real device**: xruns went from 39047 (accumulated,
continuously climbing) to 0 and holding over a clean 20+ second window;
zero new `[diag-gap]` lines over that same window (previously continuous,
multiple per second); the audio-worker thread's `/proc/<tid>/stat` `state`
field changed from `R` (running/spinning) to `S` (sleeping) between blocks.
The user's original reported symptom (first recording ending up "a
fraction" of what was played) was independently verified resolved via
byte-level MIDI injection (`tcp/9401`): a real 5128ms press-to-press
interval produced a recorded `wraplen` of 246079 samples against an
expected 246144 — a 65-sample (~1.35ms) difference consistent with ordinary
MIDI-dispatch scheduling jitter, not truncation.

A separate, real regression was found while verifying this on the live
device, then fixed in two more steps — tracked separately, do not conflate
with the readi-slowdown fix above, which is independently resolved
regardless of either issue below:

1. Both home LV2 plugins faulted and got disabled by the crash-isolation
   watchdog on their very first `runOne()` call, every single startup,
   after logging `lilv found no plugin matching bundle — falling back to
   .so-only load (no port wiring)`. Cause: `readTtl()`'s bundle-matching
   check (`src/host/lv2_host.cpp`) was a raw `bundlePath.find(bpath) == 0`
   prefix comparison; lilv's resolved bundle path carries a trailing slash
   the passed-in `bundlePath` never has, so the comparison silently and
   permanently failed even for well-formed bundles — taking the no-port-
   wiring fallback, whose first `run()` call then dereferences unconnected
   port pointers (the same class of bug as the nullptr-features fix
   elsewhere in this file). Fixed by stripping trailing slashes from both
   sides before an exact-equality compare.

2. Once that fix let both plugins actually load with real port wiring,
   `core_busy` jumped from the normal ~22-27% baseline to ~63-65% with
   xruns climbing continuously — a NEW regression exposed only by fixing
   #1 (a crashed-and-disabled plugin costs ~nothing; a genuinely running
   one costs whatever its real DSP does). Isolating each plugin (moving
   one at a time out of `/effects/home`, restarting, remeasuring) showed
   `aloop.lv2` alone reproduces the high `core_busy`/climbing xruns:
   `build-lv2.yml`'s `home-fx-lv2` job compiles `dsp/aloop.dsp` — the exact
   same Faust source `audio_thread.cpp`'s `faustHome` already compiles
   NATIVELY and runs every block — into a standalone LV2 bundle, purely as
   a CI reproducibility/packaging check (ADR-003). It was never meant to
   run a second time in the live effects chain; `image/lib-boot-tree.sh`'s
   packaging step copied every `*.lv2` it found under `LV2_DIR` with no
   distinction, so `aloop.lv2` ended up deployed and loaded right alongside
   `guitar_lofi_fx.lv2` (a genuinely standalone effect with no dependency
   on `aloop.dsp`/`loop.dsp`). Fixed by excluding `aloop.lv2` by name from
   that copy step. Verified live: with `aloop.lv2` removed and
   `guitar_lofi_fx.lv2` kept, `core_busy` returned to ~23-30% with xruns at
   0, held over a clean 15+ second window — `guitar_lofi_fx.lv2`'s own real
   DSP cost (genuinely running for the first time after fix #1) is
   sustainable on its own.

## Real hardware over asking the user to reproduce input

Per the `gm` skill's own standing rule: prefer byte-level MIDI injection
(`tcp/9401`, "synthetic MIDI bytes for scripted reproduction" —
`src/control/midi.cpp`) or SSH-based log/state inspection over asking the
user to physically press buttons/turn knobs, whenever the bug can be
reproduced that way. Reserve `AskUserQuestion` for the physical step only
once a byte-level substitute has genuinely been attempted and either proven
impossible for that bug class (depends on real analog qualities: audible
sound quality, real timing jitter, genuine electrical behavior) or the user
has explicitly said they want to verify by ear/feel themselves.

## Verify the SPEC before trusting the code's own comments as ground truth

`src/control/apc_grid.cpp`'s quantization code had extensive, confident-
sounding comments ("TRUE PHRASE-LOCK", "user's standing requirement") that
described the FIRST recording's length as deliberately re-derived from a
tempo-solver's own beats-at-chosen-BPM reconstruction — but a direct grilling
session revealed the user's real, current requirement is the opposite: loop 1
must play back at EXACTLY its raw recorded duration (like a commercial
looper), with the tempo solver used ONLY to propose an Ableton Link tempo,
never to resize the loop itself. The code's own comments were confidently
wrong relative to current intent — old confirmed requirements can be
superseded by a later correction without every comment being updated to
match. Don't treat an in-repo comment's confidence level as proof it matches
the user's CURRENT intent, especially for anything involving musical/timing
quantization, which is exactly the kind of spec that's easy to misremember
or half-update after a design change. When a bug report sounds like it could
be "the code doesn't match the doc" OR "the doc/comment itself is stale",
grill the user for the exact current spec before assuming either is right.

## Stay grounded in what this system actually is

This is a real-time C++/Faust audio looper running on real ALSA hardware
with a real Pi 4, real USB devices, and real human gestures on a real MIDI
controller. Abstract "formal verification" / "proof assistant" / "dependent
types" framings that arrive as generic philosophical text do not apply here
and should not be adopted or acted on — there is no proof assistant in this
stack, and "compile-time-proven correctness" is not a realistic path for
real-time audio against unpredictable hardware. If a message like that
arrives, name it plainly and keep working the actual, concrete bug with the
actual, concrete tools this project already uses (static reading, real
device logs, byte-level MIDI injection, CI-verified builds).

## Faust Libraries

Faust Libraries is the standard DSP library collection for the Faust
language. This file points to the most useful documentation for LLM-assisted
use.

These docs are authored in Markdown in the repository and built into HTML
for the website. When possible, prefer the Markdown sources for clean,
LLM-friendly content.

### Core entrypoints
- [Libraries index](https://faustlibraries.grame.fr/libs/): Index of all
  library reference pages.
- [Standard functions](https://faustlibraries.grame.fr/standardFunctions/):
  Core standard functions used across the libraries.
- [Overview](https://faustlibraries.grame.fr/organization/): High-level
  organization and structure of the library.
- [Motion functions](https://faustlibraries.grame.fr/motion_functions/):
  Motion-related functions and reference.

### Library map
- Each library has a dedicated reference page under `doc/docs/libs/`
  (Markdown source) and `/libs/` (HTML site).

### Markdown sources (authoritative)
- [Libraries index (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/libs/index.md):
  Index of all library docs in Markdown.
- [Libraries example (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/refs/heads/master/doc/docs/libs/basics.md):
  Example of a library Markdown source.
- [Libraries folder (API)](https://api.github.com/repos/grame-cncm/faustlibraries/contents/doc/docs/libs):
  Raw API listing of library Markdown files.
- [Standard functions (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/standardFunctions.md):
  Source Markdown for standard functions.
- [Overview (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/organization.md):
  Source Markdown for the overview page.

### Scope
- This file documents the Faust **libraries**. It does not cover the Faust
  language tutorial or compiler internals (see the optimizing-compiler notes
  in this file's own "Faust DSP compiler optimization pass" section, added
  separately, for that).

### Optional
- [Contributing](https://faustlibraries.grame.fr/contributing/): How to
  contribute to the libraries.
- [Community](https://faustlibraries.grame.fr/community/): Community and
  support information.
- [About](https://faustlibraries.grame.fr/about/): License and copyright
  information.

## Faust DSP compiler optimization pass — what shipped, what was rejected

Following [faustdoc.grame.fr/manual/optimizing/](https://faustdoc.grame.fr/manual/optimizing/),
a pass was made to apply every safe, behavior-preserving optimization from
Faust's own optimizing-compiler manual plus the native C++ hot path. What
shipped:

- **`-vec -fun -dfs -vs 32 -nvi`** added to every real `faust` invocation
  (`build-local.sh`, `.github/workflows/build-binary.yml`'s `loop.cpp`
  codegen, both jobs in `.github/workflows/build-lv2.yml`). Pure
  codegen-strategy flags — vectorized codegen, function inlining,
  depth-first scheduling, no-virtual C++ backend (Faust's own docs call
  `-nvi` "especially useful in embedded devices context", directly on point
  for the Pi 4). Same `.dsp` source, same signal graph — only how the
  compiler schedules/inlines it changes.
- **`Lv2Host`'s `LV2_Descriptor*` is now cached** at `instantiate()` time
  (`Lv2Plugin::descriptor`) instead of being re-resolved via `dlsym` +
  URI-matching linear scan on every single `runOne()` call — that call runs
  on the real-time audio block path (Core 1 home-fx + Core 3 user-fx) every
  block, so eliminating a repeated symbol-scan from the hot path was a safe,
  zero-risk win.
- **`-ct 0`** (disable Faust's table range-checking) added to all 3
  invocation sites. Verified safe by tracing every `rwtable` index in this
  codebase by hand: `dsp/loop.dsp`'s `readIdx0`/`readIdx1` are always
  `... % wrapLen` (explicit modulo, provably in `[0,wrapLen)`) with
  `writeIdx` clamped to `MAXLEN-1`; `effects/home/faust/microrepeat.dsp`'s
  `wpos`/`rpos` are likewise always clamped against `MR_MAX`/modulo'd
  against `sliceLen` before use. Every table access in every `.dsp` file
  this codebase compiles is software-bounded by existing logic already —
  this hand-trace *is* the "explicit per-table proof" this file's own
  earlier draft of this section said would be required before shipping.
  `guitar_lofi_fx.dsp` has no `rwtable`/`rdtable` at all, so the flag is
  unconditionally safe there too.
- **`kFoldStep/N` hoisted out of `audio_thread.cpp`'s per-sample SHIFT/
  glitch fold-gain ramp loop** (was recomputed via division up to 4x per
  sample; both operands are block-constant) — a direct application of the
  Faust manual's "multiply rather than divide" principle to the native C++
  hot path, not just the Faust-generated code.

What was evaluated and explicitly REJECTED (not silently skipped):

- **`ba.tabulate`-ing `filters.dsp`'s `pow(1000.0, cutoff)` (SVF
  cutoff-to-Hz) and `pitch.dsp`'s `pow(2.0, SEMIS/12.0)` (pitch-shift ratio)**:
  both files' own headers explicitly claim an *exact port*/*bit-identical*
  match to the original hardware DSP. `ba.tabulate` (even its `.cub`
  cubic-interpolation mode) is inherently an approximation — introducing it
  would trade a real, explicitly-claimed fidelity guarantee for a CPU saving
  that, in `pitch.dsp`'s case, is negligible anyway (the stage's dominant
  per-sample cost is the `dubfx_pitch_tick` ffunction call into the real C++
  pitch engine, not the one `pow()` feeding it). Any future push to tabulate
  either of these must first prove the tabulation error is smaller than the
  hardware-parity tolerance already established for that file, not just
  benchmark the CPU win in isolation.
- **Splitting the home Faust stack (`aloop.dsp`/`loop.dsp`/
  `effects_runtime.dsp`) or the Core-3 guitar+lofi-fx bundle into multiple
  separate per-effect LV2 bundles** "for performance/modularity": this would
  multiply per-plugin dispatch overhead (`findDescriptor`/`connect_port`/
  `instantiate` once more per split-out effect) in the exact code path
  once under live suspicion for the ~1Hz stall (see "Diagnosing periodic
  audio stalls" above — no longer reproduces as of the 2026-07-25 update in
  that section, but the multiply-dispatch-overhead argument against
  splitting still stands on its own), directly risking the
  never-add-latency constraint, and gives up the proven single-Faust-
  compile-unit maintainability the home stack was deliberately designed
  around ("change a knob mapping or a stage in Faust, rebuild, done" —
  `aloop.dsp`'s own top-of-file comment). Do not re-attempt this without
  re-deriving the same tradeoff from scratch.
- **Faust's own internal multicore/`-omp`/`-sch` work-stealing scheduler**:
  not applicable here. aloop already manually pins one Faust program per
  physical core via `pthread_setaffinity_np` (home stack on Core 1,
  guitar+lofi-fx on Core 3) — a coarser-grained, already-proven
  parallelization strategy. Layering Faust's own internal scheduler on top
  would fight that existing pinning architecture rather than complement it.
  (Also: this specific concern turned out not to be documented on the
  `optimizing/` manual page at all — it's covered, if anywhere, by a
  different part of Faust's docs not consulted this pass.)

Investigated and found not applicable (corrected a wrong initial premise):

- **`-mcd`/`-dlt` (delay-line threshold tuning)**: these flags govern only
  Faust's `de.delay`-family delay-line codegen, NOT the `rwtable` primitive
  — confirmed directly against the manual. `dsp/loop.dsp`'s 20×
  `MAXLEN=48000*60` rings and `microrepeat.dsp`'s ring are both `rwtable`,
  so entirely unaffected by these flags. Only `delay.dsp`
  (`de.delay(MAXD=96000,...)`) and `reverb.dsp` (`de.delay(8192,L)`, L up to
  4057) actually use delay-line codegen, and both are comfortably above the
  default `-mcd 16` threshold already — Faust's defaults (`-mcd 16`,
  `-dlt <INT_MAX>`) are already the memory-efficient choice for these sizes,
  with no measured bottleneck to justify a change. No action taken.

**`-mapp` — SHIPPED, then REVERTED after a real-hardware SIGSEGV; NOT
shipped as of this entry.** An earlier pass "verified and shipped" this
flag based on a synthetic x86_64 A/B harness (built natively via
`C:\faust` + MinGW g++, reusing the exact `FaustUI`/`FaustDspBase` shim
`audio_thread.cpp` uses, driven through a 9280-sample synthetic cycle
covering first-recording arm, finish-quantization EXTEND, varispeed
engage/disengage, and dual-loop playback) that showed byte-identical
output, 0 diff, identical md5sum with and without `-mapp`. That result
was real but insufficient: it was later WITNESSED live on the actual Pi 4
that the combination of real aarch64 codegen and real (non-synthetic,
genuinely varying) audio content triggers a 100%-reproducible SIGSEGV
(`si_addr=0x0`) inside `AloopLoopDsp::compute()` the instant real audio
first reaches the DSP — something the synthetic x86_64 harness never
exercised and could not have caught. A crash bisection ruled out `-ct 0`
first (removing it alone did not stop the crash), then removing `-mapp`
alone fixed it (confirmed live: `compute()` returns normally every block,
service stays `started`, held stable 20+s, zero xruns). `-mapp` is
deliberately NOT passed at any of the 3 real Faust invocation sites
(`build-binary.yml`'s `loop.cpp` codegen, both jobs in `build-lv2.yml`)
going forward. Lesson: a synthetic-signal x86_64 A/B, however thorough,
does not substitute for a real-hardware, real-signal test before shipping
a numeric-approximation flag — this is the same class of gap the
CI-green-but-never-loads musl/aarch64 lesson elsewhere in this file
warns about, just for arithmetic behavior instead of linkage. Any future
attempt to re-add `-mapp` needs a fresh live Pi 4 test with real audio,
not a reuse of this old synthetic result. The Windows-specific
stack-vs-heap caveat from the original harness still applies to any
future standalone harness: `AloopLoopDsp` is ~232MB (`sizeof()`) — 20
loopers × `MAXLEN` rwtable storage each — so it must be heap/
static-allocated, never stack-allocated (a stack allocation blew the
default 1MB Windows thread stack immediately, `STATUS_STACK_OVERFLOW`,
before a single sample was even processed).

**Real measured CPU improvement — closed the loop from theoretical to
proven.** Added a `faust2bench`-based benchmark step to CI
(`build-binary.yml`'s "Benchmark CPU usage" step, real Ubuntu Linux host —
`faust2bench`'s own bundled `bench.cpp` needs `pwd.h`, which doesn't exist
under MinGW, so this genuinely cannot run on Windows). 20 runs each, `-bs 64`
(matching aloop's real block size), same `dsp/aloop.dsp` source:
- **Baseline** (no flags): DSP CPU ≈ **4.27%**, ≈44.9 MBytes/sec.
- **Shipped at the time of this benchmark** (`-vec -fun -dfs -vs32 -nvi
  -ct0 -mapp`): DSP CPU ≈ **4.12%**, ≈47.5 MBytes/sec. `-mapp` was later
  reverted (see the `-mapp` entry above) after a real-hardware SIGSEGV;
  the actual currently-shipped flag set is `-vec -fun -dfs -vs32 -nvi
  -ct0` (no `-mapp`), not re-benchmarked separately, but `-mapp`'s own
  measured contribution here was a minor slice of the total win — the
  codegen-strategy flags (`-vec -fun -dfs -nvi`) are the dominant
  contributor and remain shipped.

A real, reproducible ~3.5% relative CPU reduction and ~6% throughput
increase from the flag change altogether (same DSP math on the synthetic
harness, verified byte-identical output there) — measured on the CI runner's x86_64 core,
not the Pi 4's aarch64 Cortex-A72 specifically, but the same flags apply
there via `-mcpu=cortex-a72` (also shipped, see below) and the underlying
codegen-strategy win (fewer scalar loop overheads, inlined functions, no
vtable indirection) transfers across architectures even if the exact
percentage doesn't.

**`-mcpu=cortex-a72`** (the Faust manual's `-march=cpu` advice, aarch64
form) added to every real target-compile step: `src/CMakeLists.txt` and
both LV2 `.so` link steps in `build-lv2.yml`. Deliberately NOT added to the
two native-host `.ttl`-metadata-generation `g++` compiles in
`build-lv2.yml` — those run on the CI runner's own x86_64 to print static
metadata and must never target aarch64.

**`-fm def` — still NOT shipped**, correctly, not as a hedge: it's a
strictly broader flag than `-mapp` (adds sin/cos/tan/atan/exp/log/pow/sqrt
approximations, touching `filters.dsp`'s `tan()`, `reverb.dsp`,
`compressor.dsp`'s `exp()`/`log10()`/`pow()`, and `pitch.dsp`'s `pow()` —
not just the floor/ceil/fmod/remainder class `-mapp` covers), so it needs
its own separate proof, not a free ride on `-mapp`'s result. Blocked on this
machine by a genuine toolchain artifact: Faust's own bundled
`faust/dsp/fastmath.cpp` uses `dllexport`/`EXPORT` linkage that MinGW g++
rejects for internally-linked symbols on Windows — unrelated to aloop's own
code, and irrelevant to the real target (Alpine musl/aarch64 via CI, a
different toolchain that may not hit this at all). Needs either a
Linux-side build of the same harness or genuine on-device verification
before it can be shipped with the same rigor `-mapp` now has.

**Update: the Linux-side harness this entry called for now exists**
(`test/faust-flags/`, using [DawDreamer](https://github.com/DBraun/DawDreamer)'s
`FaustProcessor` — a real Linux `libfaust` LLVM JIT backend with a
`compile_flags` passthrough) and the verdict is a hard NO, worse than
suspected: `-fm def` doesn't diverge numerically, it **segfaults on every
real-usage case** (87/87: `filters.dsp`'s `tan()`/`pow()` swept across its
real `HPCUT`/`LPCUT`/`LPRES` hslider ranges, `compressor.dsp`'s
`exp()`/`log10()`/`pow()` swept across `COMPRESSAMT`, `pitch.dsp`'s
`pow(2,SEMIS/12)` formula swept across `SEMIS`). Root cause: `-fm def`
generates calls to `fast_tanf`/`fast_powf`/etc. from `faust/dsp/fastmath.cpp`
— an architecture file meant to be compiled alongside `-lang cpp` text
output, not baked into `libfaust` itself (confirmed via
`nm thirdparty/libfaust/.../libfaustwithllvm.a`: zero `fast_*` symbols). The
LLVM JIT backend emits calls to symbols nothing resolves; `compile()` reports
success and the crash only happens once `render()` actually executes the
generated code — the same "compiling clean proves nothing about runtime
safety" trap as the `-mapp` SIGSEGV above, except this one reproduces on any
input, no real hardware or real audio content needed. A first pass of this
harness swept `HPCUT`/`LPCUT`/`SEMIS` as literal Faust constants (matching
how the standalone files pin them) and got 0/87 differences — which turned
out to mean `-fm def` never even ran: Faust constant-folds
`tan()`/`pow()`-of-a-literal away entirely at compile time. Only after
switching to real runtime `hslider`s (matching how `effects_runtime.dsp`
actually wires these controls in production) did every case crash. This does
not by itself prove the real `-lang cpp` → musl/aarch64 pipeline would crash
too (JIT symbol resolution and static linking are different failure modes,
and nothing currently compiles `fastmath.cpp` into the aloop binary either
way), but it closes off "verify it here first" as a safe path and confirms
`-fm def` needs its own real link-time proof, not just a numeric diff,
before ever being added to a real invocation site. See
`test/faust-flags/README.md` for the full writeup.

Resolved (parameter-smoothing order): **no change made, confirmed
deliberate.** `effects_runtime.dsp`'s `filterStage`/`delayStage`/
`reverbStage`/`pitchStage` take raw `hslider` values straight into
`pow()`/`exp()`-bearing math with no `si.smoo` upstream — this is NOT an
oversight. `effects/home/faust/param_mapping.md` explicitly documents that
the audio path is verified against per-render-constant normalized CC
values, with the all-defaults state producing a byte-exact passthrough
(verified `maxAbs=0`); `filters.dsp`'s own header states params are
compile-time/per-render constants "matching the looper's per-block
piecewise-constant param behaviour." Adding `si.smoo` would change the
transient response when a knob value changes between renders, directly
contradicting this already-verified hardware-parity requirement. Left
as-is.

## `apc_grid.cpp`'s rec/play cycle: `rec` must be explicitly zeroed on FINISH

`rec` is a persistent `ParamStore` value, not a momentary Faust `button()`
the widget itself releases. An earlier version set `rec=1` AND `play=1` in
the same press with nothing ever resetting `rec` back to 0 — `dsp/loop.dsp`
(`record = in*recN`) then re-recorded live input over the loop forever,
which looks identical to "not playing" from outside ("loops don't play,
they just stay paused"). `applyRecPlayCycle` (`src/control/apc_grid.cpp`)
now explicitly sets `rec=0` on FINISH. The real per-looper press cycle is:
empty → ARM (`rec=1`, held for the whole recording pass) → FINISH (`rec=0`,
`play=1`) → pause (`play=0`) → resume (`play=1`) → ...

ARM and FINISH must fire on PRESS, not release (both are "the exact instant
must land precisely" cases — release-triggered dispatch would add hold
duration as timing jitter to the start or end of the take). Pause/resume
stay on release. `m_looperArmedOnPress` suppresses the matching release
from double-firing the tap.

## `apc_grid.cpp` master-phrase length: always read writeIdx telemetry, never wall-clock, for sample-accurate boundaries

Ported from `../looper`'s `masterLoopBlocks` design: the shared phrase
length every looper quantizes to is established from the FIRST recorded
clip's own actual duration, not a fixed default. `dsp/loop.dsp`'s ring
length used to be driven only by the Link-synced branch, so a standalone
(no Link) recording left it frozen at the Faust-compiled default (1s),
truncating/looping real recordings short.

User's explicit, current spec (supersedes an earlier "TRUE PHRASE-LOCK"
design that re-derived the loop length from the tempo solver's own
beats-at-BPM reconstruction — that old design is WRONG, do not resurrect
it): loop 1 must play back at EXACTLY its raw recorded duration, like a
commercial looper. A wall-clock (`now_ms`) estimate can never satisfy this
— it isn't sample-accurate relative to the audio thread's own per-block
timeline. Fix: read `AudioThread::snapshotTelemetry().looperWriteIdx[looper]`
(the DSP's own true elapsed-sample count since the real grid-aligned arm
instant) instead of a wall-clock estimate; wall-clock is kept only as a
defensive fallback when `audio` is null (should not happen in practice).
`deriveTempoQuant` is used ONLY to propose a BPM to Link — it must never
resize `m_masterLenSamples`/`cmd/master_len` itself.

## `apc_grid.cpp` successive-recording quantization: 4 design rounds, final spec is power-of-2-only + log-space midpoint

Mirrors `../looper`'s `_calcQuantizeTarget`: a subsequent recording's raw
duration snaps to a musical subdivision/multiple of the established master
phrase length M, rather than collapsing to exactly M regardless of hold
duration. History of what was tried and why each round was replaced:

1. **Small fixed candidate set** `{..., 2M, 4M}`, nearest-raw-distance pick.
   Bug: a big gap between 2M and 4M meant anything past ~3M jumped all the
   way to 4M, recording far past the performed content ("took a piece of
   the end"); also capped at 4M with no way to reach 8M/16M/64M takes.
2. **Linear 68% threshold** between brackets. Bug: a fixed linear fraction
   of a full-octave span could trim up to 0.68×M off a genuine recording
   (e.g. a real 1.5M take cut down to M).
3. **M/16-linear-step grid** (any multiple of M/16). Rejected live by the
   user: "loops should always be clean multiples... guaranteed no matter
   how many times they play" — a linear M/16 grid could still land on
   musically-meaningless fractions like 5/16 or 11/16.
4. **Final/shipped**: candidates are POWERS OF 2 ONLY relative to M (M/16
   floor, M/8, M/4, M/2, M, 2M, 4M, 8M, ...). Decision between the two
   bracketing powers of 2 is the LOG-SPACE (geometric) midpoint —
   `sqrt(lowerCand*upperCand)` — not a linear threshold: this is symmetric
   and scale-independent regardless of which octave the recording lands in.
   Because every candidate is a genuine power of 2, any two loopers'
   `wrapLen`s are always in a clean power-of-2 ratio, which (combined with
   `dsp/loop.dsp`'s `cycleOffset` fix) guarantees perfect drift-free repeat
   alignment forever (a power-of-2 ratio between two phase-locked rings can
   never phase-drift).

Also applies: use `writeIdx` telemetry (not wall-clock) for the raw
duration input here too, for the same press-to-grid-tick timing-gap reason
as the first-recording case above.

## `apc_grid.cpp`: real APC Key25 hardware re-sends note-on for an already-held pad

Unlike the synthetic MIDI-inject test path, real hardware can re-send
note-on for a pad that's physically still held down. Without a guard, each
repeat unconditionally reset the hold-start timer (defeating long-hold
erase accumulation) and, far worse, could re-enter the ARM/FINISH dispatch
mid-recording — prematurely finishing a take after only the repeat interval
(a fraction of a second) and immediately re-arming a new recording on what
the user believes is still their original press. This is a direct
structural mechanism for "recording came out blank/near-silent," independent
of any DSP or erase-timing issue. Fix in `onPadPress`: track `m_looperHeld`
per pad and treat a repeat note-on for an already-held pad as a no-op.

## `apc_grid.cpp`: momentary Faust gates (`erase`, `finishreq`, `cmd/clearall`) must be explicitly released, never fire-and-forget

Every one-shot Faust gate driven from the control thread (`looperN/erase`,
`looperN/finishreq`) needs an explicit delayed release, or it sticks at 1
forever with nothing else ever writing it back to 0:

- **`erase` stuck at 1**: `dsp/loop.dsp`'s `wipe = max(clearAll, eraseN)`
  gates ring recirculation (`hold *= (1-wipe)`) every block. A fire-and-forget
  `erase=1` silently wiped playback on every subsequent block forever —
  recording still worked (unaffected by wipe), so the symptom was "after
  clearing it, the second round didn't play after recording." Fix:
  `pollHolds` records a release deadline (~50ms, many DSP blocks later) and
  clears it on a later tick — setting then immediately clearing in the same
  call would race the audio thread's plain-atomic read with no ordering
  guarantee (could read 0 and never observe the wipe at all).
- **`finishreq`**: same momentary-pulse shape. `dsp/loop.dsp`'s
  `finishRequestedStep` only needs to see `finishreq>0.5` for one sample
  (it latches into `finishRequested` until the next armEdge), so holding it
  ~50ms then releasing is correct — it does not need to still be 1 by the
  time the DSP-side target is actually reached.
- **`cmd/clearall`**: a genuinely HELD value (note-on sets it, the user's
  own later note-off releases it) — real wall-clock time passes in between
  by construction, so this one doesn't need the deadline pattern.

## `apc_grid.cpp` CLEAR_ALL: resetting the C++ shadow state does NOT stop the Faust DSP — both `play` and `rec` gates must be explicitly zeroed too

Two rounds of the same bug class, found live on real hardware:

1. `onClearAll` reset `m_looperPlaying[lp]=false` (C++ shadow only) but never
   told Faust to stop: `dsp/loop.dsp`'s `out = loopSig * playN * volN` kept
   outputting whatever the ring held, gated by `playN`, which was never
   zeroed — "clearing doesn't stop them." `wipe` only silences the ring's
   own recirculated content, it does not touch the play gate. Fix: explicitly
   `setLooper(ps, lp, "play", 0.0f)` in the clear path, not just the shadow.
2. Same bug for `rec`: CLEAR_ALL pressed WHILE a looper was mid-recording
   left that looper's Faust `rec` zone stuck at 1 forever (shadow reset only).
   `hold = delayed*(1-recN)*(1-wipe)` stays zero for as long as `recN==1`, so
   that looper could never play back ANY content again, even after a fresh,
   otherwise-correct ARM/FINISH cycle — "loops don't play, only passthrough."
   Fix: explicitly zero `rec` too, matching the `play` fix.

Same file's `onStopImmediate` also explicitly zeros `rec` for any
mid-recording looper (unlike plain `cmd/stopall`, which only zeroes `play`)
— stopping mid-recording is an abort, and an aborted looper stays "empty,"
never "has content."

## `dsp/loop.dsp`'s `clear`/`speed` engine-globals: Faust's `par()` re-elaborates UI primitives at EVERY instantiation site, hoisting the declaration alone does not collapse the zone count

`oneLooper` is instantiated 20× via `par(i, NLOOPERS, vgroup(...))`. A first
fix attempt hoisted the `button()`/`hslider()` declarations for `clear`/
`speed` outside the `par`/`vgroup` and passed them in as parameters —
this looked like it should produce one shared zone, but Faust's `par()`
re-elaborates whatever UI primitives sit inside an argument expression at
EACH of its 20 sites (confirmed via generated C++: still 20 separate zones,
one per "looper N" vgroup, even after that fix). The real fix removes
`clear`/`speed` as Faust UI zones entirely and threads them in as plain
`process()` signal inputs instead (like `prevFiltIn`) — `audio_thread.cpp`
writes them into `fins[2]`/`fins[3]` every block. `par()` cannot duplicate a
plain signal input since there's no UI primitive to re-elaborate.
`cmd/clearall` now correctly wipes every looper's ring in one write.

## `apc_grid.cpp`: an emptied rig must reset the shared master phrase length, from ANY path that can empty it

Per-looper long-hold erase (not just the PLAY/CLEAR_ALL button) can also
leave the rig with zero loopers holding content. `m_masterLenSamples`/
`cmd/master_len` (and `cmd/recorded_bpm`, which rides with it) must reset
to 0 whenever the LAST looper with content is erased this way, or the next
recording reuses the stale phrase length from before — witnessed live as
"the second loop became a continuation of what the first loop was set up to
do instead of starting a new song," landing the quantize branch (not the
first-establish branch) and truncating to the wrong length from the wrong
point. `pollHolds` checks `anyHasContent` after the per-looper erase loop
and resets both values if the rig just went empty, mirroring `onClearAll`'s
own reset.

## `dsp/effects_runtime.dsp`'s old `fx/bank` 3-way crossfade cost all 3 banks every block regardless of selection — removed, not fixed in place

The old design used one in-Faust `fx/bank` selector zone to crossfade
between Dub/Guitar/Lofi-Fx effect chains. WITNESSED live on a real Pi 4:
Faust has no runtime branching, so the crossfade computed all 3 full effect
chains every single block regardless of which was "selected" — a real
~7pp `core_busy` regression causing continuous audio dropouts.
`effects_runtime.dsp` is restored to its pre-LOFI dub-only chain with no
`fx/bank` zone; Guitar and Lofi-Fx moved to their own permanent Core-3 LV2
bundle (`guitar_lofi_fx.dsp`), always active, never gated by a selector.
`ApcGrid`'s 3-bank fx control surface (`onDubFxPress`/`onGuitarFxPress`/
`onLofiFxPress`) is now a pure UI/state change — each bank press only flips
which knob-target table the next CC touch reaches (`m_activeBank`) and
starts an LED flash; there is nothing left to re-push to Faust on a bank
switch, since Guitar/Lofi-Fx targets are LV2 controls on the always-active
Core-3 bundle, not a Faust zone that needs redirecting.

## `apc_grid.cpp` CC53 formant constants were verified wrong against `../looper`, corrected to match exactly

Direct cross-codebase comparison found the prior aloop constants both off
from looper's real values: deadzone was 62-65 here vs looper's real 60-68,
and the unshifted range was ±1.5 here vs looper's real ±1.0 (looper: default
±1 "musical territory", SHIFT expands to ±3). Also the normalization formula
itself used `/63.5` (differently centered) vs looper's real
`((data2-64)/63.0)*range`. `onFormantCC` now matches looper exactly:
deadzone 60-68, range ±1 unshifted / ±3 shifted, `/63.0` formula.

## `apc_grid.cpp` sidechain-pump gesture: guitar-fx held REDIRECTS looper pad presses entirely, does not layer on top

While `m_guitarFxHeld` is true, a looper pad press is consumed entirely by
`onSidechainLooperToggle` (toggles that looper's sidechain-source
designation) and never reaches the normal ARM/FINISH dispatch — it does not
touch `m_looperHeld`/`m_looperHoldStart` at all, since this is a one-shot
toggle gesture, not a "hold this pad" gesture. The sidechain-source
designation auto-clears whenever that looper's content is wiped (long-hold
erase or CLEAR_ALL) — a source tied to specific recorded content shouldn't
silently survive the content being erased.

## FaustUI shim: `addHorizontalBargraph`/`addVerticalBargraph` must register zones too

`src/dsp/audio_thread.cpp`'s hand-written `FaustUI` shim (the param-binding
`UI` Faust's generated code calls into) had `addHorizontalBargraph`/
`addVerticalBargraph` as empty no-ops, so every `hbargraph()` zone (level/
writeidx/wraplen, for all 20 loopers) was never inserted into `zones` at
all. Every `fui.get("looperN/level"/"writeidx"/"wraplen")` call therefore
missed the exact-match `find` AND fell through to the full O(n) linear
suffix-scan over the entire `zones` map, every time, for nothing — 60 wasted
full-map scans per audio block (3 fields x 20 loopers), 750 blocks/sec =
45,000 wasted linear scans/sec on the RT audio thread, permanently starving
`snd_pcm_readi` of CPU time between iterations. WITNESSED: telemetry's
level/wraplen fields read back all-zero on a live, actively-playing looper.
Fix: register these exactly like every other control type (`zones[full(l)]
= z`).

## `targetToZone`'s `fx/bank` mapping was silently missing

`src/dsp/audio_thread.cpp`'s `targetToZone()` (control-map target name →
Faust zone label) had no case for `fx/bank` (the LOFI 3-bank fx-crossfade
control, a straight passthrough since `effects_runtime.dsp` declares
`nentry("fx/bank", ...)` under its own literal name, not a renamed control
label). WITNESSED live: `ApcGrid::pushBankValuesToZones` wrote
ParamStore's `"fx/bank"` target correctly, but the generic forEach-push
loop's fallthrough `return ""` meant it never reached the real Faust zone —
bank-select buttons updated C++ state but the DSP's own bank-crossfade
never saw the change, staying pinned on Dub forever. Fixed by adding the
passthrough case.

## Faust `par()`-replicated UI controls silently duplicate per instance — use signal inputs instead

A `UI` control (`button()`/`hslider()`) passed as an argument into a Faust
function that `par()` instantiates N times gets RE-ELABORATED (UI
declaration included) at each of the N call sites — even when the
declaration text is hoisted outside the `par`/`vgroup` — silently producing
N duplicate zones instead of one shared zone. WITNESSED via the generated
C++ (`build/loop.cpp`): `grep -c '"speed"'`/`'"clear"'` both returned 20
(once per looper's own vgroup) even after an earlier fix attempt (commit
`382e775`) that looked correct in the `.dsp` source — that fix was cosmetic
and never actually collapsed to one zone, which is why half/double-speed
and clear-all kept only affecting one of 20 loopers even after landing. Fix:
thread the value through as a plain **signal input** to `process()` instead
(a wire, not a UI primitive — nothing to duplicate). `dsp/loop.dsp`'s
`oneLooper` now takes `clearAll`/`speedMul`/`masterPhase`/`masterLen`/
`sidechainEnv` this way; `audio_thread.cpp` fills each corresponding buffer
with a block-constant value every block (`std::fill`) rather than calling
`fui.set()`.

## `AloopLoopDsp` (the Faust home stack) must be heap-allocated, never a stack-local

`sizeof(AloopLoopDsp)` is ~320 MiB (20 loopers x `MAXLEN=48000*60`, 60s
delay-line rings each). WITNESSED live on a real Pi 4 (gdb + a real core
dump, `-g -O0` debug build): declaring it as a stack-local inside `worker()`
SIGSEGV'd at `setRealtimeSelf`'s very first local-variable stack write — no
pthread stack size (musl's small default, or an explicit 8 MiB, both tried)
could ever be large enough; the frame was simply unmapped from the moment
the thread's stack pointer moved to make room for a 320 MiB local later in
the same function. Fixed via `std::make_unique<AloopLoopDsp>()` — a
one-time allocation at thread startup, never in the per-block RT hot path,
so it carries none of the no-malloc-in-the-callback risk the rest of this
file is written to avoid. The `Sampler` (~5.3MB) is heap-allocated the same
way, for the same class of reason, well under the threshold that made it
mandatory for `AloopLoopDsp` but still worth keeping off the thread stack.

## Instrument USB audio device is S32_LE, not S16_LE — and ALSA silently ignores a wrong format request

The M-Audio AIR 192|4 (and most class-compliant USB audio interfaces) only
supports S32_LE (24-bit data left-justified in a 32-bit word — confirmed
via `/proc/asound/card0/stream0`: "Format: S32_LE, Bits: 24"); there is no
S16_LE fallback. WITNESSED live: requesting `SND_PCM_FORMAT_S16_LE` via
`snd_pcm_hw_params_set_format` previously succeeded at the `hw_params()`
call (no error returned) while the device silently negotiated S32_LE
anyway — the return value was never checked, so the mismatch went
undetected until it produced loud static, because the code still
normalized captured samples by the 16-bit divisor (32768) on 32-bit-wide
data (values ~65536x too large before Faust ever saw them). Fixed: buffer
type is `int32_t`, normalization divisor is `2147483648.0f`
(INT32_MAX-equivalent), and the negotiated format is read back via
`snd_pcm_hw_params_get_format` and compared against what was requested,
warning loudly on mismatch instead of silently corrupting audio. The OTG
gadget mirror is a genuinely separate S16_LE device (`f_uac2-gadget.sh`
sets `c_ssize`/`p_ssize=2` explicitly), so the two output paths need
separate wire buffers in their own native formats — never share one.

## ALSA playback stream needs `start_threshold` lowered to one period, or it never leaves PREPARED

The hw_params default `start_threshold` for a PLAYBACK stream is the full
`buffer_size`. WITNESSED live on a real Pi 4:
`/proc/asound/.../pcm0p/sub0/status` stayed stuck in `PREPARED` forever,
because this codebase's block loop only ever writes one N-frame period per
`snd_pcm_writei()` call and immediately blocks on the next capture read, so
the ring never reached a full `buffer_size` of queued frames to cross the
default threshold — meanwhile CAPTURE (which starts as soon as any data is
available, not gated on a full buffer) ran fine, so the two streams
silently desynced and playback underran on every write. Fixed by
`snd_pcm_sw_params_set_start_threshold(pcm, sw, period)` (one period, not
the full buffer) so playback triggers on the very first `snd_pcm_writei()`,
matching capture's own behavior.

## ALSA period/buffer sizing: 2 periods (256 frames) xruns constantly on this USB gadget PCM; 4 periods is the minimum that holds

WITNESSED live on a real Pi 4: an initial 2-period buffer (256 frames
total, the ALSA minimum) produced 690 xruns within seconds on the
instrument-device PCM — too tight for a USB gadget path, where each
read/write also rides USB's own transfer-scheduling jitter on top of this
thread's SCHED_FIFO jitter (unlike a bare-metal build with no OS/USB-stack
contention at all). 4 periods (at the real `block_size` N per period) keeps
the same per-period latency-determining granularity while giving the ring
enough slack to absorb that jitter. The OTG gadget mirror uses even looser
timing on purpose (period = 4xN, buffer = 4x that) since it doesn't need
tight latency, just enough buffer that its own scheduling jitter doesn't
underrun constantly.

## `masterPhaseBuf` must ramp per-sample within the block, not step at block boundaries

WITNESSED live: "loops now sound bitcrushed." Root cause: `masterPhaseBuf`
(the phrase-lock shared-clock signal input) was filled via `std::fill()`
with the same block-start value across all N samples — correct for
genuinely block-constant commands like `clearBuf`/`speedBuf` (momentary,
slow-changing), but `dsp/loop.dsp`'s `absPos` formula treats `masterPhase`
as this looper's actual per-sample READ POSITION at `effSpeed==1.0`.
Holding it constant for a whole 64-sample block meant `readIdx0`/`readIdx1`
never advanced within a block, only jumping 64 samples at each block
boundary — a stepped/aliased readback pattern, audibly indistinguishable
from bitcrushing. Fixed: ramp smoothly within the block (`masterPhaseBuf[i]
= masterPhaseSamples + i`, wrapped at `masterLen`) so it behaves as a real
per-sample position signal, not a step control.

## Sampler capture must read from a separate pre-mix buffer, not `fin`, to avoid self-recording

User requirement: SHIFT should route loop content into sample recording the
same way input and effects already get recorded into samplers. First fix
attempt (caught before shipping): moving `captureBlock` to run on `fin[]`
after the SHIFT/glitch fold reintroduced a genuine self-recording bug,
because by that point `fin[]` also contains this block's own
`renderInto`-mixed sample-playback voices — a sample recorded while another
sample/drum hit is playing would record itself. Real fix:
`src/dsp/audio_thread.cpp` snapshots `captureFin = fin` BEFORE
`renderInto()` ever touches `fin`, applies the identical SHIFT/glitch fold
to `captureFin` independently in the same fold loop, and captures from
`captureFin`. Net effect: `captureFin` = dry input + folded loop content,
deliberately excluding this block's own sampler playback; `fin` = dry input
+ sampler voices + folded loop content (the correct DSP-facing signal). The
two must stay structurally separate buffers, not a single buffer read at
two different times.

## Recording must tap a dedicated post-fx Faust input, never fold a post-fx signal into `fin`

Feeding any post-effects tap into `fin` (the live dry/input signal) makes
it become next block's `dsp` input again, which then flows through `fx`
AGAIN every block — stages reprocessing their own prior output produces a
fast, aliased whine (WITNESSED-BROKEN, originally found via the glitch-only
`prevGlitchTap` wiring). Fixed by giving `loop.dsp`'s `process()` a
dedicated second input (`prevFiltIn`) that ONLY the record-capture term
consumes (`record = prevFiltIn * recN`) — this input never joins the
dry/live path, so it structurally cannot flow back through `fx` on this or
any later block. `audio_thread.cpp` feeds this from `prevFiltOut`, a
snapshot of the PREVIOUS block's fully-effected mix (`rawFiltTap`, one of
`aloop.dsp`'s `process()` outputs), always the full effects chain
regardless of SHIFT state (user requirement: recording must always capture
the fully-effected signal, not raw pre-fx input). This replaced an earlier
separate glitch-only tap: `prevFiltOut` already contains post-glitch
content one block later since `microStage` is upstream of `filterStage` in
`effects_runtime.dsp`, making a separate glitch term redundant/
double-counting once both were live.

## Flush-to-zero/denormals-are-zero must be set explicitly on the audio thread — no portable C++ API on ARM

Denormal (subnormal) floats occur naturally in any decaying IIR
filter/feedback loop asymptotically approaching zero (reverb tails, delay
feedback, envelope followers). On both ARM and x86, denormal arithmetic can
be 10-100x slower than normal-range floats because the FPU falls back to a
microcoded slow path. There is no portable C++ standard API for this on
ARM (the compiler intrinsic `_MM_SET_FLUSH_ZERO_MODE` only exists for x86
SSE); `src/dsp/audio_thread.cpp`'s `setFlushToZero()` sets the AArch64
FPCR's FZ bit directly via inline assembly (`mrs`/`msr fpcr`) on
`__aarch64__`, and the SSE intrinsics on x86. Applied once at thread
startup before any DSP compute runs — a startup-only cost, no per-block
overhead. Became relevant when the LOFI feature added 3 parallel bank
chains (dub/guitar/lofi-fx) computed every block regardless of audibility
(Faust has no runtime branching — `select2`/`ba.if` choose among
already-computed signals, they don't skip computing them), tripling the
places a silent/near-silent signal can spend sustained time in denormal
territory.

## Two ALSA devices, never conflate them: instrument device is the real path, OTG gadget is a best-effort mirror

`src/dsp/audio_thread.cpp`'s `worker()` opens TWO distinct PCM devices,
matching the historical hardware split: the instrument device (default
`hw:0,0`, e.g. the M-Audio AIR 192|4) is the real tight-latency
capture+playback path a musician actually hears — `cap`/`play` are always
this device, opened blocking, retried up to 30 times at 1s intervals if the
interface isn't plugged in yet. The OTG gadget (`f_uac2`,
`hw:UAC2Gadget,0`) is a best-effort MIRROR of the same processed output,
opened NONBLOCK so a missing/non-streaming host on the other end can never
stall or desync the instrument device's real-time path. `-EAGAIN` on the
OTG write is expected/silently skipped (ring still has enough queued, not
an error); any other negative return triggers a one-shot recover, and if
the device is gone for good, later blocks' errors keep being silently
absorbed rather than ever blocking or crashing the RT path. A failed OTG
open at startup is silent-degrade-only — the instrument-device path is
already fully functional without it.

## Sampler capture must tap the fully-effected post-fx signal, including glitch/microrepeat

User requirement: sample recording must capture the loop content AFTER the
whole effects chain (pitch/glitch/filter/delay/reverb), not a pre-fx
snapshot — the same fully-effected signal the loopers themselves record
from. `src/dsp/sampler/sampler.h`'s own header comment previously
documented `captureBlock` as reading `fin` (dry input) plus the SHIFT/
glitch fold applied to a COPY of `fin` — a pre-fx snapshot, missing
filter/delay/reverb and, before the chain reorder, even glitch itself.

Fixed in `src/dsp/audio_thread.cpp`'s `worker()`: `captureBlock` now reads
from `prevFiltOut` (the same one-block-lagged, fully-effected snapshot the
loopers' own `prevFiltIn` record-only input already uses — see the
"Recording must tap a dedicated post-fx Faust input" entry above) instead
of the pre-fx `captureFin` buffer, which was removed entirely (it had no
other reader once this changed). Same one-block-lag discipline as the
looper record path: never fed back into `fin`, so it cannot re-enter `fx`
on any later block.

## SHIFT-hold recording latency compensation: `recordStartPhaseOffset` is
biased backward at FINISH when SHIFT (`fx/monitorfold`) was held during the take

CORRECTED (a first implementation pass of this feature wrongly gated on
`fx/pitchbend_engaged`, the SNAC pitch/varispeed engine, instead of SHIFT
— caught live: user held SHIFT during a recording and reported the
looper's playback was audibly LATE relative to the other loops, i.e. the
recorded content lagged behind where it should sit). The two controls are
genuinely distinct: `fx/pitchbend_engaged` (mod-wheel/absolute-pitch/
keybed-note gestures) drives the SNAC pitch-shift engine
(`effects/home/faust/pitch_ffi.h`), while `fx/monitorfold`
(`ApcGrid::onShiftPress`/`onShiftRelease`) is the SHIFT-fold glitch
gesture — only the latter is the one that needs compensating here.

SHIFT's own real mechanism (`src/dsp/audio_thread.cpp`'s `worker()`):
`fin[i] += prevLoopSum[i] * combinedFold` feeds the PREVIOUS block's loop
output back into this block's input whenever `fx/monitorfold` is engaged
(ramped over 16 samples via `kFoldStep`, negligible next to the block-size
lag) — `prevLoopSum` is always exactly one block (`g_cfg.blockSize`, 64
samples by default) behind the live signal. Since loopers record from
`prevFiltOut`/`prevFiltIn` (already one block lagged on its own, see the
sampler-capture entry above), SHIFT engagement adds this same block-size
lag into what's actually captured, on top of the baseline pipeline lag —
recorded content ends up effectively delayed relative to the live
performance, which is exactly the symptom reported ("behind" the other
loops).

Fix: `dsp/loop.dsp`'s `latencyBiasN = hslider("latencybias", 0, -MAXLEN,
MAXLEN, 1)` is subtracted from `masterPhase` at the exact instant
`recordStartPhaseOffset` latches (`recordStartPhaseOffsetStep(prev) =
ba.if(finishEdge, masterPhase - latencyBiasN, prev)`). A smaller
`recordStartPhaseOffset` makes `absPos = wrapAbs(masterPhase -
recordStartPhaseOffset + cycleOffset, wrapLen)` LARGER for the same
`masterPhase` — i.e. playback reads further ahead into the ring at any
given moment, catching the content up to compensate for it having arrived
late. `src/control/apc_grid.cpp`'s `applyRecPlayCycle` writes this zone at
FINISH: `kShiftFoldBlockLatencySamples` (64) if
`m_looperShiftHeldDuringTake[looper]` was ever set true during the take,
else 0. The flag is sampled every `pollHolds` tick (not just at ARM/
FINISH) against `fx/monitorfold`, so a SHIFT engagement at ANY point
mid-take is captured, not only if it happened to be held at the exact ARM
or FINISH instant, and is reset to false at ARM. This mirrors the
`finishtarget` zone's own established per-looper-hslider pattern exactly
(a genuine `par()`-replicated UI zone, confirmed via generated C++ showing
20 distinct instances, not the shared-signal-input class of bug
documented elsewhere in this file) — no new signal-input wiring was
needed since this value only needs to change once per take, unlike
`effSpeed`/`clearAll`/`masterPhase`, which need per-sample ramping.

## Glitch/microrepeat steps widened one notch: divisor halved in
`sliceLen`, not the note-to-div mapping

User's explicit spec: every one of the 5 glitch/microrepeat divisions
(notes 82-86, `apc_grid.cpp`'s `div[5] = {1, 2, 4, 8, 16}`) should produce
a slice as long as the NEXT-widest division currently does — `div=16`'s
slice becomes as long as the old `div=8`'s, `div=8` as long as old `div=4`,
etc.; `div=1` (already the widest) has nowhere further to go and stays
put. `effects/home/faust/microrepeat.dsp`'s `sliceLen = (masterLoopBlocks
/16/div)*64` formula is what actually determines slice length — the
note-to-div table in `apc_grid.cpp` is untouched, so which physical pad
triggers which relative step doesn't change, only how long each resulting
slice is.

Fix, ROUND 2 (round 1 was wrong, caught live): the first attempt used
`divSafe = max(1, int(DIV / 2))` — halving the divisor BEFORE the
division. This collapsed `div=1` and `div=2` to the identical `divSafe=1`
(`int(1/2)=0` floors to `1` via the `max` guard; `int(2/2)=1` is already
`1`), so the widest and second-widest glitch steps produced the SAME
slice length — user confirmed this live ("the widest glitch and the
second widest glitch is the same length"). The reasoning that `div=1`
should be a no-op (nothing wider above it) was wrong: the user's actual
spec is every step, including the current widest, shifts one notch
wider — there is no floor.

Fixed, correctly: `sliceBlocks = max(1, int(beatBlocks / divSafe)) * 2`
with `divSafe` back to the original `max(1, DIV)` — multiply the
ALREADY-COMPUTED slice length by 2 instead of halving the divisor going
in. A multiply after the integer division can never collide the way
halving the divisor before it did (there's no floor step for it to hit).
`div=16` now lands exactly on old `div=8`'s slice length (matches round
1's intent for every step except the top), and `div=1` now genuinely
doubles past its own old value instead of staying pinned — no two
adjacent steps can ever produce the same `sliceBlocks` again. Verified:
native Faust compile clean standalone AND as part of the full production
`dsp/aloop.dsp` stack (this file is imported by `effects_runtime.dsp`'s
`microStage` and separately by the constants-baked `chain.dsp` reference
file — both pick up the fix automatically since neither declares its own
copy of the divisor logic).

## aloop <-> esp-idf-link mesh: paired invariants (change BOTH or the mesh splits)

aloop (Pi 4) and `../esp-idf-link` (ESP32, the "ticker" box) form ONE ad-hoc
single-AP mesh so Ableton Link's multicast peer discovery reaches every
device. There is no credential provisioning: exactly one device hosts the
open SSID `ticker` and everyone else joins it as a station. Every value
below exists in BOTH trees — changing it in one project alone silently
stops the two from meshing, with no error on either side.

| Invariant | aloop | esp-idf-link |
|---|---|---|
| Mesh SSID | `src/net/config/hostapd.conf` `ssid=ticker`, `wpa_supplicant.conf` `ssid="ticker"` | `main.cpp` `wifi_scan_best_bssid("ticker")` / `wifi_start_link_ap("ticker")` |
| Auth | open (`key_mgmt=NONE`; `wpa=` lines commented out) | `wifi_connect_sta("ticker", "")` (empty password) |
| AP address / DHCP | `192.168.4.1/24`, dnsmasq `.2-.20` | `esp_netif_set_ip_info` `192.168.4.1/255.255.255.0` |
| Channel | `hostapd.conf` `channel=6` | SoftAP ch6 |
| Link multicast | Link's own hardcoded `224.76.78.75:20808` | same (hardcoded in Link itself — cannot drift) |
| Link quantum | `link_bridge.cpp` `quantum = 16.0` | `main.h` `#define LINK_QUANTUM 16.0` |
| Start/stop sync | `link_bridge.cpp` `enableStartStopSync(true)` | `main.cpp` `g_link->enableStartStopSync(true)` |
| Host election | lowest MAC/BSSID wins | lowest MAC/BSSID wins |

`PHRASE_BEATS 64.0` in esp-idf-link is NOT the Link quantum — it is that
project's own transport-correction/SPP boundary (16 bars). It intentionally
differs from `LINK_QUANTUM 16.0` and does not affect phase agreement.

### Why the host election is MAC-ordered, not "host if scan found nothing"

Two devices cold-booting together can each scan before the other's AP
exists, so a naive "nothing found -> host" makes BOTH host, producing two
isolated L2 domains Link can never cross. Both projects instead hold for a
duration strictly monotonic in their own MAC (lowest MAC ≈ 0s, highest
≈ 6s), rescanning every second during the hold and joining the instant a
peer's AP appears. This gives a total order needing no cross-device
visibility during the hold. A genuinely lone device just hosts when its
own hold expires. Both supervisors additionally yield if another `ticker`
AP with a strictly-lower BSSID appears — but never while clients are
attached, since that would drop peers mid-session.

### `autoap.sh` was structurally broken before this pass (all WITNESSED)

Four independent defects, any one of which alone prevented meshing:
1. **Wrong SSID.** It hosted `aloop`, never `ticker` — the two projects
   hosted different networks and could never see each other.
2. **No joinable network.** `wpa_supplicant.conf` had ZERO active
   `network={}` blocks (the only `ssid=` line was inside a commented-out
   example), so `known_net_available()` could never associate and the
   script always fell through to hosting.
3. **AP-mode rescan matched a placeholder.** The switchback test built its
   pattern with `grep -oE 'ssid="[^"]*"' wpa_supplicant.conf` — grep does
   not skip comments, so the pattern file contained the literal
   `YourHomeWiFi`. In AP mode it scanned for a network that cannot exist.
4. **That same line was a hard syntax error on the device.** It used
   `grep -qFf <(...)` — process substitution is a bashism; `autoap.sh` is
   `#!/bin/sh` = busybox ash on Alpine. Verified: a POSIX shell rejects it
   with `Syntax error: "(" unexpected`. So the AP→STA path could not run at
   all. Keep this file POSIX-clean; check with `dash -n src/net/autoap.sh`.

### By-the-book Ableton Link integration checklist (both projects)

Derived from a full audit of both trees against Link's own header docs. Check
any change to either project's Link usage against this list.

- **Thread-correct session-state API.** `captureAppSessionState()` /
  `commitAppSessionState()` from any non-audio thread;
  `captureAudioSessionState()` / `commitAudioSessionState()` from the audio
  thread ONLY. aloop deliberately calls only the App variants and hands the
  audio thread a lock-free double-buffered `LinkSnapshot` instead (ADR-005) —
  that is a legitimate alternative, but it means audio-side beat/phase is up
  to one control-tick stale, so shortening that interval is the lever if
  phase accuracy is ever questioned.
- **`enableStartStopSync(true)` must be paired with actually reading
  `isPlaying()` AND with setting it.** A peer that enables the capability but
  only consumes transport is half-wired; one that never calls
  `setIsPlaying()` is invisible to peers' transport. aloop now does both
  (`LinkSnapshot::isPlaying` + `LinkBridge::setTransportPlaying`, published on
  every play-state edge from `ApcGrid`). esp-idf-link only CONSUMES, and that
  is correct for it — it has no local play/stop control at all, it translates
  the session's transport into outgoing MIDI Start/Stop for downstream gear.
- **The three notification callbacks** — `setNumPeersCallback(std::size_t)`,
  `setTempoCallback(double)`, `setStartStopCallback(bool)`. Link's own header
  documents each as "invoked on a Link-managed thread" and **"Realtime-safe:
  no"**, so a callback may do bounded logging / touch atomics and nothing
  else: never allocate, never lock, never reach into the audio thread.
- **Tempo authority.** `setTempo` rewrites the tempo for EVERY peer. Calling
  it unconditionally lets two devices fight. aloop's `proposeTempo` now
  refuses when peers are already present and aloop never set the tempo
  itself; esp-idf-link only sets tempo on an explicit LTMP command.
- **Quantum is a shared constant, not a local literal.** `kLinkQuantum` in
  `src/link/link_bridge.h` and `LINK_QUANTUM` in esp-idf-link's `main.h` are
  both `16.0` and must move together.
- **Peer count belongs in telemetry, not just a bool.** `synced` (peers>0)
  cannot distinguish 1 peer from 3, which the multi-device mesh test needs.
  aloop's `status.json` now carries `link.peers` and `link.playing`.
- **Interface readiness is a real race.** Link opens its multicast socket
  during `enable()`. esp-idf-link waits 500ms before constructing Link and
  re-asserts IGMP membership for ~10s after every connection, its own comment
  noting a single join at GOT_IP can race netif readiness so membership does
  not stick. On aloop the equivalent hazard was structural: the `aloop` and
  `autoap` OpenRC services both declared only `after local` and neither
  referenced the other, so Link could start before `wlan0` had an address.
  Fixed by `depend() { after local autoap; ... }` plus a bounded
  `waitForNetworkInterface()` before `link.start()`.

### Ableton's own conformance spec lives in the tree — use it, don't guess

`build/_deps/abletonlink-src/TEST-PLAN.md` is Ableton's official Link Test
Plan (12 cases). Link's own README names compliance with it as the bar for
"apps supporting Link behave consistently", calling out *"not hijacking a
jam's tempo when joining"* specifically. Audit any Link change against it:

- **TEMPO-1..5** — tempo propagates both ways; joining a session must NOT
  change that session's tempo; enabling/disabling Link with no session must
  not change our own tempo. aloop's `proposeTempo` authority guard is what
  satisfies TEMPO-2/3.
- **TEMPO-4 range is 20..999 bpm**, and the plan exercises both ends. Checked
  by computation: aloop's follow path (`linkSpeedRatio = recordedBpm /
  sessionBpm` into `effSpeed`, clamped 0.1..8.0 in `dsp/loop.dsp`) never
  saturates inside that range — saturation begins only below ~15bpm — so
  aloop tracks the full range. `../esp-idf-link` originally clamped to
  20..400 at two sites and silently dropped anything above; widened to
  20..999 (its 24ppqn clock is beat-position-derived, and 999bpm is only
  ~12.8% of DIN MIDI bandwidth, so the ceiling was arbitrary).
- **STARTSTOPSTATE-1/2** — must BOTH listen and send. aloop now does both:
  `publishTransport` sends on every play-state edge, `applyRemoteTransport`
  follows a peer's transport with a **quantized start** ("according to its
  quantization") and an **immediate stop**. The ESP is listen-only by design
  (no local transport control; it bridges to MIDI Start/Stop).
- **BEATTIME-1/2** — no beat-time jump when enabling Link with no session,
  and an app already in a session must have no discontinuity when a peer
  joins. Worth checking against `cycleOffset`/`absPos` phase derivation.
- **AUDIOENGINE-1** — the one case with a hard number: recorded audio onset
  must align with the session pulse to **within 3 ms**. Unverified here, and
  it interacts with both the SHIFT-fold latency compensation (64 samples =
  1.333ms) and the one-control-tick staleness of the audio-thread snapshot.
  If it ever fails, the levers are a shorter publish interval or explicit
  output-latency compensation — never added buffering.

### Netboot silently outranks the SD card

WITNESSED: a correctly-written SD card looked like a broken fix because the
Pi 4 firmware prefers network boot when a netboot server is reachable. The
device fetched `start4.elf`/kernel/initramfs over TFTP and the apkovl over
HTTP, booting a 19-hour-old image that still hosted SSID `aloop` with none
of the mesh fixes. Worse, `serve-netboot-win.js` had died while holding its
`updateInFlight` guard, so `.netboot-serve/` was frozen at an old build and
`.netboot-update-sha` never advanced — the `finally` and the child's own
`REBUILD_TIMEOUT_MS` bound only the child process, not a wedged async flow.
Before trusting ANY on-device observation after an SD update, confirm which
path actually booted (check `.netboot-serve.log` for fresh TFTP/HTTP lines)
and compare the running binary's md5 against the card's.

### DHCP REQUESTs with ZERO TFTP reads means option 66 points at a dead address — not a competing DHCP server

WITNESSED, and the first diagnosis was WRONG in a way worth remembering. The
symptom: `.netboot-serve.log` fills with `[DHCP] REQUEST from <mac>` lines and
never logs a single `[TFTP]` fetch. This was initially blamed on Windows ICS
winning the DHCP race, and the proposed remedy (stop the `SharedAccess`
service, which needs elevation) would have fixed nothing.

The real cause: `serve-netboot-win.js` defaulted `SERVER_IP` to the literal
`192.168.137.1` and was launched with `--server 192.168.137.1`, but Windows
ICS had assigned the Ethernet adapter **`192.168.137.101`**. Nothing answers
on `.1`, so every ACK advertised an option-66 TFTP server that does not
exist — the Pi ACKs, times out fetching, and re-DISCOVERs forever.

How to tell the two apart in seconds, before theorising: a REQUEST whose
offered IP is the HOST's own address is the host's ICS adapter renewing its
own lease, not a rival server. `arp -a` prints the local address as
`Interface: 192.168.137.101`, and `ping` to it replies `TTL=128` (Windows)
rather than `TTL=64` (Linux). Confirm with `os.networkInterfaces()` whether
the address being advertised exists on any interface at all.

Two related traps found in the same pass:
- **The apkovl bakes the server IP at build time.** `cmdline.txt`'s
  `alpine_repo`/`modloop`/`apkovl` URLs come from `@NETBOOT_SERVER@`
  substitution, so fixing DHCP alone still fails at the HTTP stage. Rebuild
  with `NETBOOT_SERVER=<real ip>` whenever the host address changes.
- **The DHCP pool could hand out the host's own address.** `allocate()`
  computed `POOL_START + count(leases)`, so the second client was offered
  `.101` — the host itself. It now skips every reserved/local address.

`resolveServerIp()` now auto-detects the single live `192.168.137.0/24`
address and REFUSES an explicit `--server` no interface holds, failing loud
with this exact explanation rather than looping silently.

### DHCP DISCOVERs that never become REQUESTs: check the Ethernet netmask and which interface owns the subnet route

A distinct failure from the dead-option-66 case above, and it looks almost
identical in the log: repeated `[DHCP] DISCOVER from <pi-mac>` lines, ZERO
`REQUEST` lines, and — importantly — **no send error at all**, so the
`EHOSTUNREACH` tell is absent. The OFFER is being sent successfully; it is
just leaving via the wrong NIC.

WITNESSED: the static Ethernet address was set as `192.168.137.1` with a
**/16** mask (`255.255.0.0`) rather than /24, while Wi-Fi simultaneously held
`192.168.137.146/24` from a different DHCP server. Windows routes by longest
prefix match, so Wi-Fi's /24 beat Ethernet's /16 for the whole
`192.168.137.0/24` range — every reply to the `192.168.137.255` directed
broadcast went out **Wi-Fi**, where the Pi does not exist. The Pi DISCOVERs
forever because it genuinely never receives an OFFER.

Diagnose it in one command rather than guessing — this resolves the actual
egress interface, which `Get-NetRoute` alone does not make obvious when two
interfaces both have a route:

```
Find-NetRoute -RemoteIPAddress 192.168.137.255
```

If it names Wi-Fi (or anything but the Pi's NIC), that is the bug. Confirm
the mask with `Get-NetIPAddress -AddressFamily IPv4`, looking at
`PrefixLength` — `16` on the netboot NIC is wrong, it must be `24`.

Fix (both steps; the mask alone can leave a metric tie):

```
Remove-NetIPAddress -InterfaceAlias Ethernet -IPAddress 192.168.137.1 -Confirm:$false
New-NetIPAddress   -InterfaceAlias Ethernet -IPAddress 192.168.137.1 -PrefixLength 24
Set-NetIPInterface -InterfaceAlias Ethernet -InterfaceMetric 10
```

Re-run `Find-NetRoute` and confirm it now reports `Ethernet 192.168.137.1`
before restarting the server. Note the netboot server must be restarted after
any such change — it resolves `SERVER_IP` once at startup, so a running
instance keeps serving on the old address (and will log
`replies via <old-ip>`, which is the quickest way to spot a stale process).

Related trap seen in the same pass: `pkill -f serve-netboot-win` does not
always reap the listener, and the replacement then fails with
`[TFTP] bind EADDRINUSE 0.0.0.0:69` while silently falling back to a
different interface. Always confirm the ports are actually free
(`netstat -ano | grep -E ':(67|69|8080)\s'`) before concluding a restart
took effect.

### The `ticker` AP needs THREE separate things the image did not ship — all found live, in this order

`ssid=ticker` being correct in `hostapd.conf` proves nothing about whether an
AP is actually hosted. WITNESSED on a fully-booted device: `rc-service autoap
status` reported `crashed`, `wlan0` held `192.168.4.1/24`, and yet no AP
existed. Three independent causes, each of which alone is fatal:

1. **`hostapd`/`dnsmasq` were never installed.** `which hostapd dnsmasq`
   returned nothing; `hostapd: not found`. The stock Alpine RPi tarball's
   local `apks/` repo (the ONLY repo `alpine_repo=` points at — no CDN
   fallback) carries `iw` and `wpa_supplicant` but has **zero** hostapd or
   dnsmasq packages, so `apk world` could never install them. Appending them
   to the local repo is not viable either: its `APKINDEX.tar.gz` is
   RSA-signed by Alpine, and regenerating it needs an `apk` binary this
   Windows host does not have. Fixed the way the runtime libs were already
   fixed — vendor the real aarch64 binaries (`vendor/sbin-aarch64/`,
   extracted from the official `.apk`s, verified `e_machine=0xB7`) and copy
   them straight into `usr/sbin` in the overlay. `hostapd` additionally
   needs `libnl-3.so.200`/`libnl-genl-3.so.200`, which are NOT on the device
   (only libcrypto/libssl are) — both are now vendored into
   `vendor/lib-aarch64/` from the local repo's own `libnl3` package.

2. **`start_ap()` never cleared a previous hostapd.** It killed only
   `wpa_supplicant`, so any re-entry into AP mode (a role flip, an
   `rc-service autoap restart`) hit a hostapd still holding the interface and
   died with a burst of `nl80211: kernel reports: Match already configured`
   followed by `Could not set channel for kernel driver` /
   `Interface initialization failed`. **That channel error is a red herring
   — channel 6 is fine.** It was mis-diagnosed once as a regulatory-domain
   problem because ch1 happened to work on the retry; a clean re-test proved
   plain `channel=6` starts perfectly (`AP-ENABLED`) once the stale process
   is gone. Do not "fix" this by changing the channel: ch6 is a paired
   invariant with `../esp-idf-link`'s `cfg.ap.channel = 6`. `start_ap` now
   pkills dnsmasq+hostapd, waits (bounded, 20s) for the process to actually
   exit, and logs loudly instead of silently continuing if either fails.

3. **`dnsmasq` refused to start: `unknown user or group: dnsmasq`.** The
   config passed `dnsmasq --test` ("syntax check OK") yet the daemon exited
   immediately — vendoring the binary does not create the `dnsmasq` system
   user its package would have. Fixed with explicit `user=root`/`group=root`
   in `src/net/config/dnsmasq.conf`.

Verified live end to end after all three: `hostapd` and `dnsmasq` both
running, `rc-service autoap status` = `started` (was `crashed`), `wlan0` =
`192.168.4.1/24`, dnsmasq listening on `0.0.0.0:67`, `aloop` started with 0
xruns. Note `rc-service autoap status` reporting `started` is the real signal
here — a `crashed` status with a plausible-looking `ip addr` is exactly what
this failure looked like for its whole lifetime.

### Anything newly vendored into the apkovl needs adding to BOTH `tar --mode='+x'` lists, or it ships non-executable

NTFS does not carry a Unix exec bit, so `chmod +x` in the overlay is a silent
no-op on this Windows dev host. Both packaging passes compensate with an
explicit `tar --mode='+x' -rf ...` re-append listing every executable path by
name — `image/lib-boot-tree.sh` (apkovl build) and `image/build-netboot.sh`
(netboot repack). A file not named in those lists ships `-rw-r--r--`.

WITNESSED immediately after vendoring the mesh daemons: the apkovl contained
`usr/sbin/hostapd` and `usr/sbin/dnsmasq` with correct content and correct
size, but mode `-rw-r--r--` — they would have been unusable on the device
despite every other check passing. The tell is in `tar -tvzf` output, NOT in
an extraction: extracting on Windows loses the bit anyway, so **always read
modes from the archive listing, never from extracted files**.

`opt/aloop/aloop` legitimately appears TWICE in the listing (a `-rw-r--r--`
entry then a `-rwxr-xr-x` one) because the `+x` pass re-appends rather than
overwrites — this is why the existing verifier greps the LAST match. A single
`-rw-r--r--` entry with no later `-rwxr-xr-x` twin is the failure signature.

Both lists now include `$(find usr/sbin -type f ...)`, and
`build-netboot.sh` verifies hostapd/dnsmasq explicitly (missing / non-exec /
OK) rather than only checking the aloop binary — the old verifier passed
cleanly while shipping two non-executable daemons.

### Still unproven: AP-mode multicast forwarding on the Pi

Whether Link's multicast actually crosses between the Pi's own AP and its
associated stations on Broadcom `brcmfmac` is UNVERIFIED. `ap_isolate=0` is
set, which may be sufficient — but the ESP32's SoftAP needed a full
unicast relay beyond isolation (`wifi_config.cpp`'s
`link_multicast_relay_task` re-emits each Link datagram to the group, to
the AP's own IP, and unicast to every associated station, preserving the
original source IP because Link needs it for direct peer connect). Do NOT
port that relay to aloop speculatively — confirm the gap is real on real
hardware first (`docs/LINK-MESH-TESTING.md` Tests 1-3). If it is real, the
Linux-side fix is a networking-layer daemon fanning out to the dnsmasq
lease IPs alongside `hostapd`; it does not require touching
`link_bridge.cpp`.

## Fast DSP-only iteration: `image/dsp-hotdeploy.js`, skip the netboot cycle

A pure `.dsp`/Faust edit does not need `image/build-netboot.sh`'s full image
assembly or a device reboot -- `image/dsp-hotdeploy.js` pushes a commit
through CI's real musl/aarch64 cross-compile (the same `build-binary.yml`/
`build-lv2.yml` jobs the netboot pipeline already relies on -- see "The
device runs Alpine/musl/aarch64" above for why a host-built `.so` cannot
substitute for this), then SFTPs just the changed artifact onto a live
device and runs `rc-service aloop restart` (not `reboot`) over the same
pure-JS `ssh2` client the rest of this file's SSH guidance already
mandates.

Usage: `node image/dsp-hotdeploy.js --target home` (home-stack `.dsp`
changes -> `aloop-aarch64-musl` artifact -> `/opt/aloop/aloop`), `--target
guitar` (`guitar_lofi_fx.dsp` changes -> `guitar-lofi-fx-lv2` artifact ->
`/effects/home/guitar_lofi_fx.lv2/`), or `--target both`. Requires the edit
already committed and pushed (this script polls the CI run that commit
triggered, via `gh run list --workflow <file> --json headSha,...` matched
against `git rev-parse HEAD` -- it does not itself trigger a run) and `gh`
authenticated. Fails loudly if the matched run's conclusion isn't
`success`, or if `rc-service aloop status` doesn't report `started` after
the restart -- never silently declares success on a stale/crashed process
(see the REBOOT-listener and staleness caveats elsewhere in this file for
why that check matters).

**STOPS the service BEFORE overwriting `/opt/aloop/aloop`, not after.**
WITNESSED live: `sftp.fastPut` against the binary's own path while `aloop`
was still running failed with a bare `Failure` -- musl/Alpine's ETXTBSY on
a write to a currently-executing file's inode, no more specific error text
than that. The script now does `rc-service aloop stop` -> `fastPut` ->
`rc-service aloop start`, never `restart`-after-write (which implicitly,
incorrectly relied on the OLD binary still being writable while running).

This preserves every constraint the netboot cycle also preserves -- zero
added audio latency (a service restart takes the same code path as any
other `aloop` start, no new buffering), the real target ISA/libc (CI, not
this Windows host, does the actual link step), and the existing 4-core
pinning (unchanged binary/config, only the compiled artifact differs) --
while skipping the image-assembly and reboot wall-clock cost entirely for
the DSP-only edit case. It intentionally does NOT replace the netboot path
for anything touching `image/lib-boot-tree.sh`/`image/build-netboot.sh`
themselves, kernel/cmdline config, or OpenRC service files -- those still
need a real image rebuild, exactly as documented in "The netboot
self-update pipeline" above.

## `Lv2Host::setControl` must match Faust's MANGLED LV2 port symbol, never the raw Faust hslider label

WITNESSED live: "switching fx modes from dub to lofi/guitar doesn't work,
just stays on dub effects" -- and this was true from the very first commit
that added the Guitar/LofiFx bank, not a regression. Faust's own `lv2.cpp`
architecture (`mangle()`, in the Faust install's `share/faust/lv2.cpp`)
never emits a control's raw Faust label as the real LV2 TTL port
`lv2:symbol` -- it replaces every non-alnum/non-underscore character
(including `/`) with `_`, then appends `"_<portIndex>"` (the control's own
index within the plugin's port list, assigned in declaration order).
`hslider("fx2/FLANGEAMT", ...)` therefore gets a REAL port symbol of
`fx2_FLANGEAMT_3` (verified directly against the deployed bundle's own
`.ttl`: `grep lv2:symbol guitar_lofi_fx.ttl`), never `fx2/FLANGEAMT`.
`apc_grid.cpp`'s guitar/lofi-fx knob target tables use the raw Faust
labels verbatim (matching the `.dsp` source's own `hslider()` calls, which
is the natural thing to write), so `Lv2Host::setControl`'s old
exact-symbol-match scan could never succeed -- every knob CC silently
matched nothing, permanently, since this bank was first added. This is
audibly indistinguishable from "stuck on Dub" because Dub is a wholly
separate, always-audible Faust-zone effect chain layered underneath the
(silently inert) Core-3 LV2 stage -- the bank-select buttons themselves
worked correctly the whole time (confirmed via LED behavior), only the
knob-to-plugin control path was dead.

Fixed: `setControl` now matches by MANGLED-LABEL PREFIX
(`mangleFaustLabel(rawLabel) + "_"` compared as a string prefix against
each port's real symbol) rather than requiring an exact match, so it's
robust to whatever numeric port-index suffix Faust's codegen happens to
assign on a given build, without `apc_grid.cpp` needing to hardcode
fragile per-build indices. Verified live via temporary `[lv2-diag]`
logging in `setControl` itself (since removed): a real CC48 send with the
Guitar bank active produced `setControl symbol=fx2/FLANGEAMT
prefix=fx2_FLANGEAMT_ -> port=fx2_FLANGEAMT_3 value=0.7874` -- the control
value genuinely reaches `controlValues[3]`, the exact memory
`connect_port` bound to the plugin's real port.

**Any future LV2-hosted Faust control target must be verified this way**
(read the deployed bundle's own `.ttl` `lv2:symbol` lines directly, or add
temporary match-diagnostic logging) rather than assumed to equal the raw
Faust `hslider()`/`button()` label string -- this is the LV2-hosting
equivalent of the `par()`-UI-duplication and `strcmp(nullptr,...)` classes
of bug already documented elsewhere in this file: a plausible-looking
target string that silently never matches anything at runtime, with zero
error output anywhere in the pipeline.

## A stray uncommented `disable_core3_lv2 = 1` in `/etc/aloop.conf` silently kills the ENTIRE always-on guitar/lofi-fx stage, not just a bisection test

WITNESSED live, found immediately after fixing the LV2 symbol-mangling bug
above: even with `setControl` genuinely reaching the plugin's real port
(confirmed via the diagnostic logging), knob turns still produced zero
audible effect. Root cause: an earlier session's `bisect-1hz-stall.js` A/B
testing (see "Diagnosing periodic audio stalls" above) had left
`/etc/aloop.conf` with a real, uncommented `disable_core3_lv2 = 1` line
still active on the device -- this flag makes `audio_thread.cpp`'s worker
skip `homeFx.process()`/`userFx.process()` entirely every block, so
`guitar_lofi_fx.lv2` (which lives in `/effects/home`, loaded onto `homeFx`,
per the "Two ALSA devices" section's own established Core-1/Core-3
terminology) never runs its DSP at all regardless of any control value
fix -- fully silent, fully inert, with no error or warning anywhere. This
is a LIVE-DEVICE-ONLY state (the shipped `config/aloop.conf` template only
ever carries this line commented-out, as pure documentation), so it does
not reproduce on a fresh netboot/reflash -- but it can persist silently
across any number of `rc-service aloop restart`s on an already-running
device, since restart re-reads the same live `/etc/aloop.conf`, not the
repo's template. Always `grep -n disable_core3_lv2 /etc/aloop.conf` on the
live device (anchored to line-start, no leading `#`, matching the already-
fixed detector regex in the bisection tool) before spending further time
debugging "guitar/lofi effects don't do anything" as a code bug.

## DawDreamer-driven optimization pass 2: what shipped, what was verified-and-rejected, what's still open

Follow-up to "Faust DSP compiler optimization pass" and `test/faust-flags/`
above, using the same DawDreamer (`FaustProcessor`, real Linux `libfaust`
LLVM JIT, `compile_flags` passthrough) harness pattern, extended with a
native-side native-code A/B (`faust2bench`, since `-O2` vs `-O3` is a g++
flag downstream of Faust's own codegen, outside what the LLVM JIT backend
exercises). This session had no real Pi 4 access -- everything below is
either verified on a real Linux x86_64 host (DawDreamer's real libfaust, or
a real local g++/cmake build) or explicitly flagged as still needing
real-hardware confirmation, per this project's own established "compiles
clean proves nothing about runtime safety" discipline (see `-mapp`/`-fm def`
above).

**SHIPPED: `-O3` for the aloop binary itself (`src/CMakeLists.txt`), was
`-O2`.** The LV2 `.so` builds (`build-lv2.yml`) already compiled the
Faust-generated C++ at `-O3`; the main `aloop` binary -- which contains the
actual hot RT path, `AloopLoopDsp::compute()` plus all native C++ -- was
still at `-O2`, an inconsistency nobody had benchmarked. Measured via this
project's own established `faust2bench` methodology (real `dsp/aloop.dsp`,
shipped Faust flags `-vec -fun -dfs -vs 32 -nvi -ct 0`, `-bs 64`, 60 samples
per flag set, x86_64 host): `-O2` averaged 8.128% DSP CPU / 26.43 MB/s;
`-O3` averaged 8.029% DSP CPU / 27.62 MB/s -- a real, reproducible ~1.2%
relative CPU reduction and ~4.5% throughput increase, isolated to JUST the
GCC optimization level (no `-Ofast`, no `-march=native`, no fast-math --
those carry the same numeric-approximation risk class as `-mapp`/`-fm def`
and were deliberately not touched). `-O3`'s extra passes over `-O2`
(GCC's own `-ftree-vectorize`, loop unswitching/distribution/peeling,
predictive commoning) are standard behavior-preserving optimizations, not
precision-losing ones, so this carries none of `-mapp`'s risk profile --
still confirmed via a real local `cmake`+g++ build (x86_64, dev-only
`-march=native` substituted for the Pi-specific `-mcpu=cortex-a72`) that the
full binary (Link, LV2 host, MIDI, the real Faust-generated `loop.cpp`, all
of it) still compiles clean and boots identically to the `-O2` build.

**SHIPPED: removed the confirmed-dead `rawGlitchTap` Faust output**
(`dsp/effects_runtime.dsp`'s `<: (_, _)` fanout, `dsp/aloop.dsp`'s
`mixAndFx` 4th-output plumbing, `audio_thread.cpp`'s matching `fouts[4]` ->
`fouts[3]`). `effects_runtime.dsp`'s own header comment already asserted
this tap was dead (`audio_thread.cpp`'s old `fouts[1]` was populated every
block but never read again anywhere in that file); grepped the whole tree
to confirm zero other readers before removing. Not a measurable CPU win by
itself (Faust's `<:` fanout duplicates an already-computed signal, it
doesn't recompute it -- the cost was one extra buffer + one extra per-block
store), but it's real, verified-zero-risk dead-code removal in the exact
hot-path struct this session was auditing, and simplifies the interface for
the next person touching it.

**SHIPPED: cached the per-block looper telemetry zone lookups**
(`audio_thread.cpp`'s telemetry-read block, resolved once into
`looperTelemetryZones[]` right after `fui` is built, matching the EXACT
established pattern `resolvedControls`/`sidechainSrcSlot` already use
elsewhere in this same file). This is the READ-side twin of the already-
documented "Severe continuous readi()-slowdown" fix above, which only ever
fixed the WRITE side (pushing control values into Faust zones) -- the
telemetry READ path (`rec`/`play`/`vol`/`level`/`writeidx`/`wraplen`/
`readposdiag2` for 20 loopers, 140 `snprintf`+`std::map::find` calls) was
never converted and still ran on every single audio block (750/sec, so
105,000 `snprintf`+map-lookup pairs/sec). Exact-match zone registration was
already fixed (see the FaustUI bargraph-registration entry above), so this
was never hitting the O(n) linear-scan fallback -- but it was still paying
a full `snprintf` + `std::map::find` for 140 already-known, never-changing
pointers every block. Fixed by resolving all 140 `float*` zone pointers
ONCE at thread startup (with the identical exact-match-then-suffix-scan
fallback `fui.get()` itself uses, so behavior is provably unchanged even in
the zone-not-found case), then dereferencing them directly per block. No
DawDreamer needed for this one (pure native C++ pointer caching, mechanical
and behavior-identical by construction) but it was verified compiling and
booting clean via a real local `cmake` build (see above).

**SHIPPED: avoided `std::fmod` in the per-sample `masterPhaseBuf` ramp
loop** (`audio_thread.cpp`, the loop that fills `masterPhaseBuf[i]` for
`dsp/loop.dsp`'s `masterPhase` signal input -- see this file's own
"`masterPhaseBuf` must ramp per-sample" entry above for why this loop
exists and must never be reverted to a block-constant fill). The original
called `std::fmod` (double-precision) once per sample, N times/block. Since
the caller already guarantees `masterPhaseSamples` is pre-wrapped into
`[0, masterLen)` before this loop runs, and `masterLen` is virtually always
>= the block size N for any real recorded loop, the common case needs at
most ONE wrap within the block -- replaced with a cheap running
accumulator (`p += 1.0`, conditional single subtract on overflow), falling
back to the exact original `fmod`-based code only when `masterLen < N` (a
pathological loop shorter than one block, which cannot happen from normal
recording/quantization but is cheap to keep correct for). **Verified
numerically exact, not just "should be equivalent"**: a Python harness
swept `masterLen` from 1 to `MAXLEN` (`48000*60`) and `N` from 1 to 512
against thousands of phase-start values (including near-wrap and Link's
fractional-phase inputs), comparing the fast path's output to the original
formula's bit pattern via a real float32 round-trip -- 0 mismatches across
7452+ cases. This same sweep FIRST caught a real divergence in an
earlier, unguarded version of this optimization (accumulated rounding
across many wraps-per-block when `masterLen < N`, a case that cannot occur
today but would have been a silent latent bug for any future change that
shrinks the minimum loop length) -- the guard above is what closes it, not
an afterthought.

**Correctness fix found *while* verifying an unrelated performance
question, not itself a performance change: `effects/home/faust/
bitcrush.dsp`'s "byte-exact passthrough at BITCRUSHAMT=0" claim was false.**
While using DawDreamer to verify `guitar_lofi_fx.dsp`'s own header comment
("At every control's default (0.0), each stage is independently verified
byte-exact passthrough... so the WHOLE chain is byte-exact passthrough at
all defaults by construction"), a real random full-amplitude float signal
through the whole chain came back with `max_abs_diff = 1.529e-05` --
nonzero, and far above the ~3e-8 float32-round-trip floor every other
always-on stage in that chain actually sits at. Isolating each of the 8
stages individually (same DawDreamer harness, one stage at a time) pinned
it to `bitcrush.dsp` alone; the other 7 were genuinely at the float32 noise
floor. Root cause: `BITS_MAX=16` at `BITCRUSHAMT=0` quantizes to 16-bit
resolution (`step = 2/2^16`) unconditionally -- the file's own header
comment claimed this was "far finer than any float rounding noise," which
is wrong for real full-precision float audio (16-bit quantization noise is
~500x the float32 rounding floor, not below it). The stale rationale
assumed identity with "the CLI harness's own WAV writer" (a bench-only
int16 dump) -- but aloop's real production signal path never round-trips
through int16 anywhere before this stage; the instrument device negotiates
real S32_LE/24-bit (see this file's own ALSA format entry above), so 16-bit
quantization was a real, always-on, unconditional precision floor on the
live audio path even with the bitcrush knob left at its "off" default.
Fixed by raising `BITS_MAX` to 24 -- verified via the same DawDreamer
harness to bring the amt=0 diff down to 8.94e-08 (now genuinely at the
float32 noise floor, matching every other stage) while leaving the crushed
extreme (`BITCRUSHAMT=1`, `BITS_MIN=2` unchanged) numerically identical
(same unique-output-level count verified before/after). Inaudibly small
either way at 16 vs 24 bits, but it directly contradicted an explicitly
documented invariant (`param_mapping.md`'s all-defaults passthrough
requirement, referenced elsewhere in this file's own "parameter-smoothing
order" entry) and is exactly the kind of gap DawDreamer-based verification
is for -- catching a comment's confident claim that was never actually
checked against a real render.

**Investigated and NOT pursued (evidence-based, not skipped):**
- **`ba.tabulate`/further approximating `filters.dsp`'s `tan()`/`pow()` or
  `reverb.dsp`'s `decayC`/`dampC`**: already rejected in the prior
  optimization pass for the hardware-parity/bit-exactness reason (see
  above); nothing this pass found changes that tradeoff.
- **Splitting or gating `reverb.dsp`'s comb-filter compute / `guitar_lofi_fx.dsp`'s
  8 always-on stages behind their own amount parameters**: confirmed, via
  direct reading of both files (`reverb.dsp`'s own `select2`-guarded
  passthrough, `guitar_lofi_fx.dsp`'s own header history), that this is the
  SAME already-documented "Faust has no runtime branching" constraint the
  prior 3-bank-crossfade removal already fixed once system-wide --
  `select2`/`ba.if` choose among already-computed signals, they do not skip
  computing them, so there is no in-Faust way to skip a stage's cost based
  on its own runtime amount being zero. Not re-litigated; would need the
  same topology-level fix (moving a stage off-core) the guitar/lofi-fx
  redesign already applied once, not a DSP-level change.
- **`dsp/loop.dsp`'s dual-tap `ring`/`ringCeil` rwtable read (floor/ceil
  linear-interpolation taps)**: suspected as a possible double-allocated
  table (two `rwtable()` calls sharing the same write args, different read
  index) before checking -- WITNESSED via a real DawDreamer memory-delta
  test (a large synthetic table, 4 processor instances, measured RSS
  growth per instance) that Faust's compiler already shares the underlying
  table storage between the two reads (~31 MB/instance measured against a
  24 MB single-table expectation, not ~48 MB a genuinely doubled table
  would cost) -- already efficient, not a bug, no change made.
- **`-clang` (Faust's clang-specific auto-vectorization pragmas)**: checked
  against the actual toolchain this project uses for every real target
  compile -- `build-binary.yml`/`build-lv2.yml`/`src/CMakeLists.txt` all use
  `gcc`/`g++` (Alpine's `build-base` inside the aarch64 container, or the
  CI host's own `g++` for LV2), never `clang++`. This flag emits
  `#pragma clang loop vectorize(...)`-style pragmas GCC does not act on --
  zero benefit for this project's real toolchain, not evaluated further.
- **`-mem` (Faust's multi-memory-block DSP layout flag)**: aimed at
  embedded targets with genuinely separate memory banks (e.g. DSP chips
  with distinct fast/slow RAM regions); the Pi 4 target is a normal Linux
  process with one unified heap, so this has no real target to split
  across. Not evaluated further.

**Still open, needs real Pi 4 hardware to close (not evaluated further this
session, no hardware access):** whether `-O3`'s measured x86_64 win
transfers proportionally to the Pi 4's aarch64 Cortex-A72 core (the prior
optimization pass's own `-vec -fun -dfs -nvi` benchmark carries the same
caveat and was shipped anyway on the reasoning that the underlying codegen
wins transfer across architectures even if the exact percentage doesn't --
same reasoning applies here, `-O3` is even less architecture-specific than
those flags). Also open: whether the `bitcrush.dsp` fix is audible at all
(expected answer: no, by construction -- verify by ear only if the user
asks, per this file's own "Real hardware over asking the user to reproduce
input" standing rule, since a DawDreamer render diff already answers the
numeric question a byte-level test could not improve on).

## Polyphonic keybed live-pitch: ported and redesigned from lanmower/DawDreamer's DT_Whammy, replaces the old mono last-key-wins engine

`effects/home/faust/multitranspose.dsp` is a new NVOICES=6 polyphonic
chord-harmonizer stage, ported from the DT_Whammy design in
`lanmower/DawDreamer` (`tests/faust_dsp/dt_whammy.dsp`, commits `b0e05c3`/
`df93b37`/`9e6426c`) then reworked for aloop's constraints. DT_Whammy's
own `harmony_mode` is the direct ancestor: each Faust polyphonic voice
takes `freq`/`gain`/`gate`, derives `harmonyShift = hz2midikey(freq) -
root_note`, glides the shift via `si.smooth(tau2pole(glide_ms))`, gates a
fast `en.adsr` envelope, and shifts via `ef.transpose` (a crossfaded
delay-line shifter -- cheap, no pitch-detection lookahead, unlike the
autocorrelation-based SNAC engine already used elsewhere in this file).

**Why a second, separate engine instead of running N instances of the
existing SNAC engine (`effects/home/faust/pitch_ffi.h`)**: SNAC does real
pitch detection/resynthesis, real CPU cost per instance; running up to 6
of them concurrently was never seriously considered as a viable low-
latency option. `ef.transpose` is the same class of technique DT_Whammy
itself is built on and is cheap enough that this project's own established
"Faust has no runtime branching, always-on stages have a real fixed cost"
tradeoff (already accepted for `guitar_lofi_fx.dsp`'s 8 always-on stages)
applies the same way here. The existing SNAC engine (`fx/pitchbend`,
mod-wheel/CC52-driven) is UNTOUCHED -- it remains the mono "pedal ride"
lane; the new engine is strictly additive, summed with `pitchStage`'s
output in `effects_runtime.dsp` before the shared filter/delay/reverb
chain, so `pitchStage(dry) + harmonize(dry, ...)` reduces to exactly `dry`
when neither is engaged (`pitchStage` is a hard bypass when `ENGAGED=0`;
`harmonize` outputs silence when every voice's gate is 0), i.e. zero
behavior change to the existing mono pedal path.

**Faust `par()`-replicated UI duplication (this file's own long-documented
gotcha, "Faust `par()`-replicated UI controls silently duplicate per
instance") is why the 6 voices' `(semis, gate)` pairs are plain
`process()` signal inputs, never `hslider`/`button`.** `multitranspose.dsp`
is imported once (not `par()`-replicated) so this specific bug class can't
hit it directly, but threading the 12 values as signal inputs anyway
(matching `masterPhase`/`clearAll`/`effSpeed`'s existing discipline) means
nobody has to rediscover this if the stage is ever wrapped in a `par()`
later. This DOES mean `dsp/aloop.dsp`'s `process()` arity grew from 7 to
19 inputs (`s0,g0,...,s5,g5` appended), threaded through `mixAndFx` into
`effects_runtime.dsp`'s own `process()` (also grown, 1 to 13 inputs).

**Faust direct function-call syntax substitutes the WHOLE multi-wire
expression into a single formal parameter -- it does not splice a bus
across several formal parameters positionally.** Hit live while wiring
this feature: an initial `mixAndFx(loop(...), s0,g0,...)` (calling
`mixAndFx` as a function with `loop(...)`'s 2-output bus as the first
argument) failed to compile with `too much arguments : 2, instead of : 1`
inside `pitchStage`, because `dry` inside `mixAndFx` had been bound to the
ENTIRE 2-wire `loop(...)` output, not just its first wire -- Faust function
application is closer to textual substitution than to a wire-count-based
splice. The fix, and the correct idiom (matching how this file already
composed `loop(...) : mixAndFx` before this change): build the combined
bus with `,` (parallel composition) first, then pipe the whole thing with
`:` into a multi-formal-parameter function -- `:`-based composition DOES
wire positionally by count, unlike direct call syntax.
`(loop(...), s0,g0,...,s5,g5) : mixAndFx` and, inside `mixAndFx`,
`(dry, s0,g0,...,s5,g5) : fx` are the two sites this applied to.

**Voice allocation is a plain round-robin/oldest-steal allocator in
`ApcGrid`** (`allocateTransposeVoice`/`releaseTransposeVoice`,
`m_transposeVoiceNote[kTransposeVoices]`), the same class of technique
Faust's own `dsp_poly` voice manager uses -- a held note reuses its own
slot if replayed, an unheld slot is preferred, and once all 6 are held the
OLDEST-triggered voice is stolen (its Faust-side ADSR just re-attacks from
wherever its envelope is, same as a real synth voice-steal; not a special
case). `onKeybedNoteOff` releases by GATE only (`fx/xpose{v}/gate=0`),
never a hard cut -- the Faust-side `en.adsr` release phase (50ms, matching
DT_Whammy's own verified click-free release time) does the fade.
`onLiveEngageToggle` (the master "transpose on/off" button) and
`onClearAll` both release every held voice, matching this file's own
repeatedly-hit "momentary gates must be explicitly zeroed, never
fire-and-forget" discipline (see the `erase`/`finishreq`/CLEAR_ALL entries
above) -- applied here from the start, not discovered as a live bug this
time.

**Gain staging: fixed per-voice gain (0.6) plus a `ma.tanh` soft-clip on
the summed voice bus, not a dynamic active-voice-count normalization.** A
dynamic `1/sqrt(activeVoices)`-style renormalization was considered and
rejected: it would make the OVERALL harmony bus level jump every time a
chord note releases (a new pumping artifact directly contradicting the
"smooth" requirement), even though it would technically use full headroom
more efficiently. A fixed gain risks clipping when many voices are held at
once at a loud input level; `ma.tanh` after the sum is a static,
level-independent nonlinearity (no pumping) that caps the worst case
without touching the audible character of normal 1-3 voice playing.
Verified numerically via DawDreamer (`FaustProcessor`, real Faust JIT,
`test_multitranspose.py`/`test_performance.py`, not committed --
scratchpad-only harnesses matching this file's own established
`test/faust-flags/` verification style): 6 real voices held simultaneously
against a near-full-scale (0.95 peak) input stays at 0.995 max abs (no
clipping, no NaN/Inf); silence when no voice is gated; smooth,
click-free glide through note-on/retrigger/release sequences (max
sample-to-sample jump ~0.02 during a 3ms attack ramp, no discontinuities).

**Glide 8ms, ADSR (3ms attack / 30ms decay / sustain 1 / 50ms release),
window 10ms, crossfade 50%** -- deliberately faster than DT_Whammy's own
defaults (12ms glide, 15ms window) per this feature's explicit "even more
digitech, smooth AND fast" requirement, while keeping the exact
already-verified-click-free ADSR shape DT_Whammy's own commit history
arrived at (see `9e6426c` above). `auto_window` (DT_Whammy's later
pitch-synchronous window-sizing feature, `df93b37`) was deliberately NOT
ported: its own commit message states the zero-crossing pitch tracker
"assumes roughly monophonic/tonal input" and is unpredictable on
chord/broadband material -- exactly what this stage's shared input always
is once more than one voice is held, so it would be worse, not better,
for aloop's actual use case.

**None of this touches the audio-path block size/ALSA buffer chain** --
`ef.transpose`'s ~10ms window is an algorithmic pitch-shift latency
intrinsic to the effect itself (identical in kind to the existing SNAC
engine's own engaged-only latency), applied only to the wet harmony bus,
which is purely additive on top of the always-instant dry/loop signal.
Never a candidate for the "never add audio-path latency" rule, which is
about the fixed ~7ms system block chain, not a wet effect's own DSP
latency while engaged.

**Verification status, explicit split**: the Faust signal graph (all 3
touched `.dsp` files, `dsp/aloop.dsp`+`dsp/effects_runtime.dsp`+the new
`multitranspose.dsp`, wired together exactly as shipped) compiled and
rendered NaN/Inf/click-free via DawDreamer's real Faust frontend/JIT
(`pip install dawdreamer` in-session; `pitchStage`'s `ffunction` bridge to
`pitch_ffi.h` had to be bypassed for this specific harness only -- the JIT
backend flatly refuses to link `ffunction`-declared external symbols,
`calling foreign function 'dubfx_pitch_tick' is not allowed in this
compilation mode`, a known pre-existing JIT-vs-`-lang cpp` gap this file's
own `test/faust-flags/` harness already works around the same way by never
compiling `pitch.dsp`/`aloop.dsp` directly -- unrelated to this change,
`pitch_ffi.h` itself is untouched). `src/control/apc_grid.cpp`/`.h` and
`src/control/midi.cpp` (the new voice-allocator logic) compiled clean via
a real `g++ -std=c++17 -fsyntax-only`. `src/dsp/audio_thread.cpp`'s new
buffer-threading code could NOT be compiled in this session -- no Docker
daemon for this file's own `build-local.sh` cross-compile path, and the
sandbox's `apt` mirror (`security.ubuntu.com`) was 404ing on
`libasound2-dev`/`faust` package fetches, independent of anything in this
change. That file's new code was reviewed by hand against directly
adjacent, already-working patterns in the same function (`clearBuf`'s
`std::fill`, `sidechainSrcSlot`'s resolve-once-cache-by-slot pattern) but
is UNVERIFIED by any real compiler in this session -- the real
`build-binary.yml`/`build-lv2.yml` CI run on push is the first real
compile this code gets, and CPU/`core_busy`/xrun impact of 6 new always-on
`ef.transpose` voices, plus genuine audible click-free-ness under real
playing, both still need live Pi 4 verification per this file's own
"compiles clean proves nothing about runtime safety" discipline.

## `multitranspose.dsp` v2: fixed-interval harmonizer replaced with real pitch-LOCK (Infected Mushroom Manipulator style)

Direct user correction after testing the v1 feature above: the shipped
design (`harmonyShift = hz2midikey(freq) - root_note`, ported faithfully
from `lanmower/DawDreamer`'s `dt_whammy.dsp` `harmony_mode`) was never
actually a pitch LOCK -- `freq` there is the Faust-poly-bound MIDI note's
OWN pitch (`mtof(note)`), not a measurement of the live input signal, so
the shift is a fixed interval from whichever key is held. Playing a
different note on the instrument while holding the same key moves the
harmony note right along with it, instead of holding still on the pressed
key the way Digitech Whammy-style "poly harmony" pedals and Infected
Mushroom's Manipulator plugin do. `apc_grid.cpp`'s `onKeybedNoteOn` made
this concrete: `ps.setByName(semisName, (float)(note - 60))` -- a relative
semitone offset from middle C, not an absolute lock target. Confirmed via
a real DawDreamer render (dry input on a full-scale sine, single voice
gated) that this v1 design WAS reaching the output audibly (dry+harmony
both present in the spectrum, ~32% of total energy on the shifted band) --
"didn't hear it" was a semantic mismatch (harmonizer vs lock), not a
wiring/silence bug.

**Fix**: `multitranspose.dsp` now runs `an.pitchTracker` (Faust's own
zero-crossing-rate/adaptive-lowpass detector, the exact function
`dt_whammy.dsp`'s `auto_window` feature already used for window-sizing but
never for the shift itself) on the live input ONCE per sample, converts to
a MIDI note via `ba.hz2midikey`, and each voice's shift is
`(targetNote - detectedNote)` instead of a value handed in directly -- so
the output always lands on the exact held key regardless of what pitch is
actually being played. `apc_grid.cpp`/`.h` and `audio_thread.cpp` renamed
`semis`/`xposeSemisBuf`/`xposeSemisSlot`/`fx/xpose%d/semis` to
`note`/`xposeNoteBuf`/`xposeNoteSlot`/`fx/xpose%d/note` throughout (the
wire's meaning changed from a relative interval to an absolute MIDI note
target, so the old name would be actively misleading) and
`onKeybedNoteOn` now passes the raw MIDI note number, not `note - 60`.
`dsp/aloop.dsp`/`dsp/effects_runtime.dsp` needed NO changes -- their
`process()` signatures already just thread 13 generic `s0,g0,...,s5,g5`-
shaped signals through unchanged; only what those values MEAN changed,
entirely inside `multitranspose.dsp`.

**Fixed-window pitch inaccuracy found and fixed in the same pass**: a
first version of the lock kept the v1 file's fixed 10ms `ef.transpose`
window. DawDreamer verification (`detectedNote`/`shiftAmount` exposed as
extra outputs, isolated from the final `ma.tanh`) showed the pitch
DETECTION and the shift MATH were both correct to within 0.02-0.05
semitones at every tested input pitch -- but the actual `ef.transpose`
OUTPUT for a +22-semitone lock (110Hz input locked to G4) landed 366.67Hz
instead of 392.32Hz, ~114 cents flat, worsening as the shift ratio grew
(220Hz->G4: -0.68 semis; 440Hz->G4, barely more than unity ratio: only
+0.18 semis). This is `ef.transpose`'s own known limitation at large
shift ratios (a cheap crossfaded-delay-line shifter, not the sophisticated
period-locked-splice SNAC engine `pitch_ffi.h` uses) -- and it is not new
to this file: `dt_whammy.dsp`'s own preset modes reach +-24 semitones
through the identical `ef.transpose` primitive, so any design using it for
large jumps inherits this. Fixed by making the window pitch-synchronous
(sized from the detected period, `dt_whammy.dsp`'s own `auto_window` idea,
borrowed here as the ALWAYS-ON default rather than an optional toggle,
since it is strictly more accurate and the detected pitch is already being
computed for the lock target anyway) instead of a fixed 10ms value, capped
at 20ms (not `dt_whammy`'s 40ms ceiling, to keep this feature's own worst-
case wet-path latency tighter, matching the "even more digitech, smooth
AND fast" spec the original v1 feature was built to). Re-verified after
the fix: every one of the same test cases lands within 0.00-0.02 semitones
(effectively exact), including a fresh note-on-after-silence worst case
that previously mistracked by -4.69 semitones. `windowFor`'s
`si.smooth(...) : max(64)` double-clamp (smooth BEFORE truncating to int,
then re-floor after) is copied deliberately from `dt_whammy.dsp`'s own
documented fix for the same hazard: the smoother's ramp-up from a
zero-initialized register can pass through a near-zero window value, and
`ef.transpose`'s internal `fmod(_, w)` on a near-zero `w` poisons its
recursive delay state with NaN forever after.

**Verified via DawDreamer, real render + FFT + stability checks** (all
numbers reproducible, no hardware needed since this is pure Faust with no
`ffunction` dependency, unlike `pitch_ffi.h`): pitch-lock accuracy across
110-440Hz input for a fixed G4 target (0.00 semis error, every case,
post-fix); 3-voice chord lock (C4/E4/G4) from a single unrelated input
note, all three voices landing with balanced, correctly-placed spectral
energy; silence-input stability (zero output, no NaN/Inf -- the tracker's
own `max(minTrackHz)` floor keeps `ba.hz2midikey` away from `log2(0)`);
note-on-immediately-after-silence (the tracker's worst-case settling
scenario); real (non-sine) audio through the full effects_runtime.dsp
chain (`Music Delta - Disco/bass.wav`, 4-voice chord held throughout, no
NaN/Inf); 6-voice rapid retrigger stress test (every 150ms, deliberately
harsher than real playing) -- max sample-to-sample jump 0.23 with the new
pitch-synchronous window, versus 1.58 (a near-full-scale discontinuity)
for the OLD fixed-window v1 file under the IDENTICAL stress test, so this
fix is a click-safety improvement as well as an accuracy one, not a
tradeoff between them; extreme low/high lock targets (MIDI 24 and 108,
~3.5 octaves from the 220Hz test input in each direction) both stable;
voice-steal/reallocation stress (single slot cycling through 10 different
targets) stable. Full chain re-verified end-to-end through the real
`dsp/effects_runtime.dsp` (filters/delay/reverb/microrepeat at their
default-passthrough settings, `pitch.dsp` stubbed to a bare passthrough
for the harness only, matching this project's own established
JIT-vs-`ffunction` workaround) -- dry input and the pitch-locked harmony
voice both present in the output spectrum at the correct frequencies.

**Still needs live Pi 4 verification, same as the v1 entry above and for
the same underlying reason (no hardware this session)**: real playing
CPU/`core_busy` cost of `an.pitchTracker` running every sample (one
instance now, shared across all 6 voices -- computed once in `process`'s
own `with{}` block and threaded down as a parameter, not re-instantiated
per voice), and genuine audible lock feel/latency under a real
performance. The `src/control/apc_grid.cpp`/`.h` and
`src/dsp/audio_thread.cpp` renames were syntax-checked via a real local
`g++ -std=c++17 -fsyntax-only` (clean) but that check does not exercise
the `ALOOP_HAVE_FAUST_LOOP`/`ALOOP_HAVE_ALSA`-gated code paths the actual
renamed lines live inside (no Docker daemon, no `libasound2-dev`, no
`faust` CLI in this sandbox, same gap as the v1 entry) -- real coverage is
the next CI run on push, not this session.

## SHIFT engaging free (unlocked) transpose: `multitranspose.dsp` gets a `free`
signal input, gating the wet harmony bus rather than touching the pitch-lock math

User request: holding SHIFT (the same `fx/monitorfold` gesture as the
SHIFT-fold/latency-compensation entries above) should let the instrument be
played at its natural pitch -- the polyphonic pitch-lock engine
(`multitranspose.dsp`, see its own "v2: fixed-interval harmonizer replaced
with real pitch-LOCK" entry above) must stop pulling played notes onto the
locked target while SHIFT is held, without touching the lock behavior at all
once SHIFT releases. Implemented as a `free` signal input threaded through
`effects/home/faust/multitranspose.dsp` -> `dsp/effects_runtime.dsp` ->
`dsp/aloop.dsp`'s `mixAndFx`/top-level `process()`, following this file's own
established "momentary/held UI state is a plain signal input, not a
`hslider`/`button`" discipline (the `masterPhase`/`clearAll`/`effSpeed`
precedent) even though `multitranspose.dsp` itself is imported once, not
`par()`-replicated, so it isn't directly exposed to that bug class -- kept
for consistency with every other cross-block control in these files.
`audio_thread.cpp` fills the new `freeXposeBuf` (now `fins[20]`, was
`fins[19]`) every block from `g_params->get("fx/monitorfold")`, the exact
same ParamStore read `onShiftPress`/`onShiftRelease` already drive.

Implementation: `multitranspose.dsp`'s `process()` multiplies the summed
`harmonySum` (all 6 voices) by `freeGate = (1.0 - free) : si.smoo` BEFORE
the final `ma.tanh`, muting the wet pitch-shifted bus smoothly while SHIFT is
held and leaving the dry signal (passed through elsewhere in
`effects_runtime.dsp`'s `pitchStage(dry) + harmonize(...)` sum) untouched --
the pitch TRACKING/lock math itself keeps running unconditionally (cheap
relative to muting-and-restarting it, and avoids any settle-time cost when
SHIFT releases), only the audible wet output is gated.

Verified via DawDreamer (`pip install dawdreamer`, real Faust JIT,
`multitranspose.dsp` compiles standalone with no `ffunction` dependency so
this needed no `pitch.dsp` stub workaround): `free=0` reproduces the
existing pitch-lock behavior (wet content present, matching the file's own
prior verified behavior); `free=1` mutes the wet bus to below `1e-4` while
locked to the same target note/gate inputs; a mid-render `free` toggle from
0->1 produces no discontinuity beyond what the pre-existing ADSR attack
transient already produces elsewhere in the same render (isolated a 200-
sample window around the toggle instant vs. the rest of the render and
confirmed the toggle-local max sample-to-sample jump does not exceed the
render's own baseline max jump) -- `si.smoo`'s ramp is the same mechanism
`MONITORFOLD`/`GLITCHFOLD` already use for this exact gesture elsewhere in
`dsp/aloop.dsp`, so this carries no new click risk.
`src/dsp/audio_thread.cpp`'s new `fins[20]`/`freeXposeBuf` code is
`ALOOP_HAVE_FAUST_LOOP`-gated like the rest of the per-block worker loop, so
(same as every other native-side change in this file) it could not be
compiled in this sandbox (no Faust CLI, no generated `loop.cpp`) -- reviewed
by hand against the immediately adjacent `xposeNoteBuf`/`xposeGateBuf` fill
pattern it copies, real coverage is the next CI run.

## Continuous USB-drive ring recording: new `src/storage/usb_recorder.{h,cpp}`
subsystem, lock-free handoff from the RT thread to a control-thread poll, no
prior USB-storage detection existed anywhere in this tree

Researched first (per this session's own instruction to check before
building): grepped the whole tree for `mdev`/`hotplug`/`usb.*storage`/
`automount` -- zero hits. Nothing in `image/lib-boot-tree.sh` or `src/`
handled USB mass-storage detection or mounting at all before this session;
the only existing USB-related code was `src/usb/f_uac2-gadget.sh` (OTG
peripheral-mode gadget setup, a completely different USB role -- gadget
mode on the micro-USB/USB-C port vs. host mode on the 3 USB-A ports a flash
drive actually plugs into).

**Design, matching this project's established RT-safety discipline (the
same class of constraint documented throughout this file for
`AloopLoopDsp`/`Sampler`/the SHIFT-fold ramp)**: `UsbRecorder` owns a fixed,
heap-allocated `int16_t` ring buffer (5 seconds of capacity) that
`audio_thread.cpp`'s RT worker writes into every block
(`pushBlock(prevFiltOut.data(), N)`, called right next to `g_sampler->
captureBlock(...)` -- the SAME post-fx tap point the sampler/looper record
paths already use, per this file's own "Recording must tap a dedicated
post-fx Faust input"/"Sampler capture must tap the fully-effected post-fx
signal" entries). The producer side is a single atomic-counter SPSC ring
(`std::atomic<uint64_t>` write/read counters, not raw indices, so
full-vs-empty is unambiguous) that NEVER blocks or allocates: if the
consumer has fallen behind, `pushBlock` advances the read counter itself to
make room (dropping the oldest not-yet-written samples) and increments an
overrun counter instead of stalling the audio callback -- the same
"drop, never block" contract the rest of this file's RT-thread rules
describe for cases where the RT thread cannot wait.

The actual file I/O (mount detection, WAV chunk writing/rotation, deleting
nothing explicitly -- chunks are fixed-size and cyclically
`O_TRUNC`-reopened, so the ring bounds disk usage by construction rather
than needing an explicit eviction pass) all happens in `UsbRecorder::poll()`,
called from `main.cpp`'s existing 5 Hz control loop right alongside
`telem.publish()`/`remote.poll()` -- deliberately NOT a dedicated pthread.
This matches `Telemetry`/`RemoteControl`'s existing "own `poll()` called from
the control loop" shape rather than `Sampler`'s dedicated-worker-thread
shape, since the control loop's existing ~200ms cadence is already slow
enough that a blocking USB write (tens of ms, per this session's own
instruction) fits comfortably inside one iteration without needing its own
thread, and the 5-second ring absorbs any single slow iteration or a missed
mount-detection edge without dropping audio.

**Mount detection is a plain `stat()`-device-id comparison** (`isMounted()`:
the configured mount point's `st_dev` differs from its parent directory's
`st_dev` exactly when something is mounted there -- the same technique the
real `mountpoint` command uses), not `/proc/mounts` parsing -- avoids a
dependency on `/proc/mounts` line format and works identically regardless of
filesystem type. New `[storage]` section in `config/aloop.conf`
(`usb_record`, `usb_mount_point` default `/media/aloop-usb`,
`usb_chunk_minutes` default 10, `usb_chunk_count` default 6) follows the
existing `loadConfig()` sscanf pattern in `src/main.cpp` exactly.
`effectiveChunkCount()` shrinks the ring to fit smaller drives (`statvfs`
against the mount, capped by the configured chunk count) so a small flash
drive doesn't get asked to hold `usb_chunk_count * usb_chunk_minutes`
worth of audio it doesn't have room for.

**New `src/usb/usb-automount.sh` (mdev hotplug script) + `src/usb/
usb-automount-setup.sh` (local.d bootstrap)**, since nothing populated
`/media/aloop-usb` before this session. `usb-automount-setup.sh` appends
two rules to `/etc/mdev.conf` (`sd[a-z][0-9]* ... @/opt/aloop/
usb-automount.sh add` / `... $.../usb-automount.sh remove`) if not already
present -- APPENDED, never overwriting the file, since Alpine's stock
`mdev.conf` already drives the base system's own device-node population
(the `.default_boot_services` marker this file already documents elsewhere
depends on `mdev`'s normal hotplug behavior staying intact) and clobbering
it would be the same class of regression as the MONITORFOLD/dead-zone bugs
elsewhere in this file. Because `local.d` (boot runlevel) runs AFTER
`mdev -s`'s initial sysinit-runlevel coldplug scan, a drive already
inserted before boot would be missed by that first scan (our mdev.conf rule
doesn't exist yet when it runs) -- `usb-automount-setup.sh` compensates with
its own explicit coldplug pass over `/dev/sd[a-z][0-9]*` after installing
the rule, invoking the same `usb-automount.sh add` path directly. Only FAT32/
ext4/exFAT/NTFS are attempted (`mount` with no `-t` first for kernel
auto-detection, then explicit `-t vfat`/`ext4`/`exfat`/`ntfs` fallbacks) --
exFAT/NTFS userspace tools are almost certainly NOT in the minimal Alpine
RPi tarball's local apk repo (same "~100-package minimal set, no CDN
fallback" constraint this file's vendored-hostapd/dnsmasq entry already
documents), so only kernel-native FAT32/ext4 mounting is expected to
actually work without further vendoring -- UNVERIFIED on real hardware,
flagged here rather than assumed.

New files registered in BOTH `image/lib-boot-tree.sh`'s `_exec_paths` and
`image/build-netboot.sh`'s `_nb_exec_paths` (`./opt/aloop/
usb-automount.sh`, `./etc/local.d/25-usb-automount.start`), per this file's
own "Anything newly vendored into the apkovl needs adding to BOTH
`tar --mode='+x'` lists" entry -- `build-netboot.sh`'s `NBOVL` is extracted
directly from `boot_tree_apkovl`'s own output, so both lists needed the
identical addition, not just one.

**Verification actually performed this session** (no real Pi 4, no USB
hardware, no `faust`/Docker, matching every gap already documented
elsewhere in this file): `usb_recorder.cpp` compiles clean under
`g++ -std=c++17 -Wall -Wextra -fsyntax-only` (it has zero dependency on
Faust/ALSA/the `ALOOP_HAVE_*` gates, unlike every other native-side change
in this file's history) and was exercised with a real standalone harness
against a real `tmpfs` mount (root-mounted in this sandbox, so `isMounted()`
's `st_dev` comparison is exercised against a genuine distinct-filesystem
mountpoint, not a mock): recording auto-starts the instant the mountpoint
is detected as mounted, 400 pushed blocks at a deliberately tiny sample
rate produced exactly `usb_chunk_count` bounded chunk files with zero ring
overruns, chunk 0's WAV header parses back with the correct `RIFF`/`WAVE`/
`data` tags and a real non-zero patched `data` size (i.e. the seek-back-
and-repatch-on-rotate logic is genuinely exercised, not just written and
trusted), and recording survives the record directory being deleted out
from under it (the open fd keeps writing to the unlinked inode, matching
POSIX semantics) until a real unmount. `sh -n`/`dash -n` clean on both new
shell scripts and the two edited packaging scripts. The mdev.conf rule
syntax itself, real USB-drive enumeration on the Pi 4's host-mode USB-A
ports, and exFAT/NTFS driver/tool availability on the real device are all
UNVERIFIED and need live Pi 4 hardware to close -- same standing caveat
this file applies to every hardware-adjacent change with no device access.

## Follow-up to the SHIFT free-transpose fix, same session: the natural/dry
pitch must NOT play underneath the locked pitch while actively transposing

The free-transpose fix above only handled the SHIFT-held case; direct user
follow-up pointed out that even with SHIFT released, actively holding a
keybed lock target still let `pitchStage(dry)`'s own passthrough (SNAC
disengaged = a bare `dry` passthrough, see `pitch.dsp`'s `process = _,
scale, FORMANT, ENGAGED : pitchTick`, a no-op transform when `ENGAGED=0`)
sum alongside `harmonize`'s wet locked voices, so the original pitch was
always audible layered under the lock -- not a real "lock," a harmonizer.
Fixed in `dsp/effects_runtime.dsp` alone (no new signal input needed --
`dry` and `g0..g5` were already in scope there): `dryGate = (1.0 -
min(1.0,g0+g1+g2+g3+g4+g5)*(1.0-freeXpose)) : si.smoo` multiplies
`pitchStage(dry)`'s contribution, so it fades to ~0 whenever any voice is
gated AND `freeXpose` is 0 (actively locking), and stays at 1 (full
passthrough) whenever no voice is held OR `freeXpose` is 1 (SHIFT/free
held) -- the two features compose correctly by construction since `dryGate`
already factors in `freeXpose`.

Verified via the same DawDreamer/`pitch.dsp`-stub harness as the
free-transpose fix above: idle (no voice gated) keeps >0.99 correlation
with the original dry signal; actively locked (voice gated, `freeXpose=0`)
drops to ~0 correlation with the original dry pitch while the locked wet
content stays audible (`max_abs` > 0.02); SHIFT/free held with a voice
still gated restores full dry passthrough (>0.99 correlation again); a
mid-render gate-on transition's own max sample jump (0.025) stays below the
render's own pre-existing attack-transient jump size (0.058) -- no added
click from the new gate.

## `delay.dsp`'s minimum delay time was never actually fast: a spurious `+1.0`
per-sample term in the slew recursion put a hidden ~208ms floor under EVERY
TIME setting, independent of the TIME->ms mapping

User request: "the fastest delay must be real fast, to allow micro delay
effects." The TIME->ms mapping's floor was already 0.5ms (a prior session's
change from the original hardware-parity 1ms), which LOOKS fast on paper --
but `curStep(target, c) = c + (target - c)*SLEW + 1.0` (SLEW=0.0001) adds a
full sample of drift every single sample regardless of `target`, on top of
the intended exponential approach. Solving the recursion's own fixed point
(`c* = c*(1-SLEW) + target*SLEW + 1.0` => `c* = target + 1.0/SLEW`) shows the
delay-length state converges to `target + 10000` samples, not `target` --
i.e. every TIME setting carried a hidden, constant ~208ms (10000 samples @
48kHz) floor on top of whatever the TIME knob mapped to. WITNESSED via a
real DawDreamer render (`FaustProcessor`, real libfaust JIT): warming the
ring up for 90000 samples (so the SLEW recursion has time to reach its real
fixed point) then injecting a single impulse at TIME=0 (the new 0.02ms/
1-sample floor) showed the first echo landing at sample **9995**, not
sample 1 -- confirming the bug numerically, not just symbolically. This
explains the user's report directly: the "fastest" delay was never
meaningfully faster than any other setting near the bottom of the range,
since the ~208ms floor dominates the whole low end of the TIME mapping.

Root cause traced to a documented-but-wrong claim in the file's own header
comment ("C++: currentDelay = writePos - readPos == newDelay[n-1] + 1"): the
real C++ reference (`apcEffectsProcessor::processSends`, quoted verbatim at
the top of this file) has NO `+1` anywhere -- `newDelay = curLen +
(target-curLen)*0.0001` is a plain one-pole slew toward `target`, full stop.
The relationship the old comment describes (`currentDelay[n] ==
newDelay[n-1]+1`) is a TAUTOLOGY about how sample indices relate when
`readPos` tracks a constant-length gap behind `writePos` (which itself
advances by 1 every sample) -- true of the C++ model's own bookkeeping, but
never a reason to literally add 1.0 to a recursively-slewing state variable.
A prior session appears to have mistaken that tautology for a required
correction term. Fixed by removing the `+1.0` entirely: `curStep(target, c)
= c + (target - c)*SLEW` -- now numerically identical in form to
`newDelayFrom`, preserving the existing one-sample lag between the `letrec`
state and the actual read tap (`len[n] = newDelayFrom(target, cd[n])`, so
`cd[n+1] == len[n]` by construction) without the spurious drift.

**TIME->ms mapping also widened to genuinely reach the structural 1-sample
floor** (was 0.5ms min): `MIN_DELAY_MS = 1000.0/SR` (exactly 1 sample,
0.0208ms @ 48kHz) replaces the old hardcoded 0.5ms constant, so TIME=0 now
maps to precisely the `max(1.0, ...)` floor `targetSamples` already
enforces structurally -- the fastest the delay can possibly represent,
genuinely reachable now that the recursion bug no longer masks it. TIME=1
still maps to ~1000ms (the ceiling is unchanged).

**Verified via DawDreamer** (real libfaust JIT, `component("effects/home/
faust/delay.dsp")[DELAYAMT=...; TIME=...;]`, ring warmed up 90000 samples
before measuring so the SLEW recursion reflects true steady state, not
cold-start transient): TIME=0 now lands the echo at sample 1 (0.0208ms);
TIME=1.0 lands at sample 47980 (999.58ms, matching the intended ~1000ms
ceiling); a 6-point sweep (TIME=0/0.1/0.25/0.5/0.75/1.0) is linear and
monotonic across the full range (0.02ms/100.0ms/249.9ms/499.8ms/749.6ms/
999.6ms); a real-noise render at TIME=0 with feedback (DELAYAMT=0.6, a
stable fb=0.63<1 setting) produced no NaN/Inf. Not verified: real Pi 4
audible character at the new sub-millisecond floor (expected to sound like
a tight comb/flange rather than a discrete echo, which is the whole point
of "micro delay" -- this is a direct, provable consequence of the fixed
recursion's math, not something that needs live hardware to confirm, unlike
this file's numeric-approximation-flag entries elsewhere in this file).

## SHIFT-held polyphonic keyplay redirects the transpose engine onto the
loops instead of muting it: `multitranspose.dsp` gets a `loopSum` input,
crossfades its OWN tracked/shifted source and wet destination on `free`
rather than adding a second voice bank

User request, direct follow-up to the SHIFT free-transpose feature above:
"when shift is held the polyphonic keyplay of the transpose should affect
the loops." The existing SHIFT-held behavior (see this file's own "SHIFT
engaging free (unlocked) transpose" entry) simply MUTED the harmony engine
entirely while SHIFT was held (`freeGate = (1.0-free) : si.smoo` zeroed the
whole wet bus) so the live instrument could be played at its natural pitch
-- but nothing happened to the currently-playing loop content during that
hold. The user wants SHIFT-held polyphonic keyplay to become a live
"Whammy-on-the-loop" gesture instead: holding SHIFT and playing chords now
pitch-locks the LOOP PLAYBACK to the held keys, exactly like the existing
dry-input pitch-lock does when SHIFT is NOT held.

**Design: reuse the SAME 6-voice `ef.transpose` bank + single
`an.pitchTracker` instance, never add a second one.** This project's own
established cost discipline (`guitar_lofi_fx.dsp`'s 8 always-on stages,
this file's own "Faust has no runtime branching" entries) makes a naive
"add a second identical 6-voice bank for the loop" the wrong shape --
doubles this stage's CPU cost for a feature that's only ever audible while
SHIFT is held. Instead, `multitranspose.dsp`'s `process` now takes a second
audio input (`loopSum`, the raw loop-engine output, already available at
every call site since it was already an `effects_runtime.dsp`/`mixAndFx`
parameter) and crossfades which signal FEEDS the tracker/shifter, and which
output CARRIES the resulting wet signal, on the SAME smoothed `free` gate:
`sigIn = dry*(1-freeSmooth) + loopSum*freeSmooth`, `dryWet = wet*(1-
freeSmooth)`, `loopWet = wet*freeSmooth`. At free=0 this is numerically
identical to the pre-existing behavior (loopWet=0, dryWet=wet, sigIn=dry);
at free=1 the entire engine -- tracking AND shifting -- is redirected onto
`loopSum`, with dryWet=0. The crossfade sharing the same `freeSmooth` value
for both the source blend and the output split means a mid-transition
sample carries a proportionally correct mix of both, not a mismatched
source/destination pairing.

**Threading the second output through the call chain**: `effects_runtime.
dsp`'s `process` gained a `loopSum` input and now returns TWO outputs
(`mainOut`, unchanged shape/meaning; `loopHarmonyWet`, the new loop-routed
wet bus, deliberately NOT run through `microStage:filterStage:delayStage:
reverbStage` -- those are dry-signal-oriented stages, and `loopSum` itself
already bypasses them entirely on the direct-playback path, so the wet loop
transpose stays consistent with that existing architectural choice rather
than adding a second parallel fx-chain pass). `aloop.dsp`'s `mixAndFx` now
calls `fx` with `loopSum` as an extra argument (it already received
`loopSum` as its own parameter -- no new top-level `process()` input needed
anywhere, so **audio_thread.cpp needs zero changes** for this feature) and
splits the resulting 2-wire bus via `fxBus : _,! ` / `fxBus : !,_` (a
single shared computation, not a duplicated one -- Faust shares named
signal bindings referenced multiple times the same way this file's own
`freqDet`/`winSamples` bindings inside `multitranspose.dsp`'s `harmonySum`
already prove, this is NOT the `par()`-replication UI-duplication class of
bug documented elsewhere in this file).

**New `loopDirectGate` complementarily suppresses the raw, unprocessed
`loopSum` term exactly when SHIFT is held AND a voice is gated** (`1 -
anyVoiceGated*freeXpose`, the same multiplicative-complement shape as
`directFoldSuppress`/`monitorFold`/`glitchFold` elsewhere in this file, but
driven by `freeXpose` directly rather than the native one-block-lag fold
ramp, since the harmony bus here is computed same-block): `filtOut = fxOuts
+ loopSum*directFoldSuppress*loopDirectGate + loopHarmonyWet`. Mirrors the
existing dry-side `dryGate` exactly (dry fades OUT while actively locked,
replaced by the wet locked signal, never summed with the raw original) --
the loop is REPLACED by its pitch-locked version while SHIFT+voice is held,
not layered underneath it (avoiding a comb-filter/phasing mess from
playing raw and shifted copies of the same loop simultaneously). At
SHIFT-held-but-no-voice, `loopDirectGate` is 1 (raw loop plays normally,
unaffected) and `loopHarmonyWet` is 0 (silent, since the ADSR gate keeps
every voice's envelope at 0) -- zero behavior change from before this
feature when nothing is actually being played.

**Verified via DawDreamer** (real libfaust JIT): `multitranspose.dsp`
standalone -- free=0+voice-gated locks the `dry` input to the target note
(spectral energy at the target frequency >0.02, loopWet silent); free=1+
voice-gated locks `loopSum` instead (dryWet silent, loop carries the
target-note energy); free=1+no-voice leaves both wet outputs silent. The
real committed `effects_runtime.dsp` (compiled as its own file, not a
reimplementation, pitch.dsp stubbed to a bare passthrough for the harness
only -- the same established JIT-vs-`ffunction` workaround this file's
other DawDreamer entries already use) reproduces the identical 4-case
matrix end-to-end, including confirming the EXISTING dry-pitch-lock
behavior (free=0, voice gated) is numerically unchanged (correlation with
raw dry <0.5, i.e. genuinely locked not passthrough) and that plain
passthrough (free=0 or free=1, no voice) keeps >0.99 correlation with dry
in both cases. The real committed `aloop.dsp` (20 process() inputs, 3
outputs, matching the pre-existing signature exactly) compiles clean and
renders a 2-second real-signal smoke test with SHIFT held + a voice gated
with zero NaN/Inf and sane output levels; the `loopDirectGate`/
`loopHarmonyWet` arithmetic itself was hand-traced against all 4 gating
states (SHIFT x voice-gated) rather than re-verified with a second
DawDreamer pass, since `fxOuts`/`loopHarmonyWet` feeding into it are
already the DawDreamer-verified `effects_runtime.dsp` outputs and the
remaining new arithmetic in `mixAndFx` is a direct, mechanical extension of
the already-proven `directFoldSuppress` pattern. Not verified: real Pi 4
audible character/CPU cost -- expected to be ~zero additional CPU (no new
`ef.transpose` voices or `an.pitchTracker` instance, purely a crossfade of
existing computation), unlike the original multitranspose feature's own
real-hardware CPU caveat, but this has not been measured on target
hardware. `src/control/apc_grid.cpp`'s existing SHIFT/voice-allocation
logic needed no changes -- this feature is entirely a Faust-side reroute of
signals `apc_grid.cpp` already drives (`fx/monitorfold`/`freeXpose`,
`fx/xpose%d/note`+`/gate`).
