#!/bin/sh
# image/build-netboot.sh — assemble a Pi 4 TFTP/DHCP netboot root.
#
# WHY (the pre-SD dry run): before burning an SD card, you can boot the exact same
# aloop diskless Alpine tree over the network. The Pi 4 bootloader (VideoCore
# EEPROM, boot order NETWORK) does DHCP, learns a TFTP next-server, and fetches
# start4.elf/fixup4.dat/config.txt/kernel/initramfs over TFTP — then Alpine boots
# diskless into RAM and restores the aloop apkovl, identically to the SD path. This
# lets you iterate the image and catch boot issues with ZERO reflashing.
#
# The netboot root is a PLAIN DIRECTORY (no FAT, no mtools/sfdisk) — it is served
# as-is by a TFTP server. That is also why this builder runs on any host with just
# curl+tar+sh (unlike the SD builder, which needs mtools to make a FAT partition).
#
# The boot tree itself (firmware chain, kernel, apkovl, dwc2/serial/isolcpus config)
# is assembled by the SHARED image/lib-boot-tree.sh — identical to build-image.sh —
# so the SD image and the netboot root can never drift. This script only adds the
# netboot-specific pieces: ip=dhcp on the cmdline (so the diskless initramfs brings
# the NIC up) and ensuring modloop-rpi + the apkovl are present in the served root.
#
# Inputs (env or defaults):
#   OUT       output netboot-root directory   (aloop-netboot/)
#   ALOOP_BIN / LV2_DIR / ALPINE_* — same as build-image.sh (passed to the lib).
# A build with no ALOOP_BIN/LV2_DIR still produces a valid, bootable netboot root
# (for pipeline/layout validation); it just has no binary/effects yet.
#
# Serve it (on a Linux/WSL host on the same wired LAN as the Pi) — see docs/NETBOOT.md.
# Build with NETBOOT_SERVER set to the serve host's IP so the cmdline HTTP URLs point
# back at it, then run the serve script (DHCP + TFTP + HTTP root, all in one):
#   NETBOOT_SERVER=192.168.137.1 OUT=/srv/tftp/aloop-netboot image/build-netboot.sh
#   sudo image/serve-netboot.sh --iface eth0 --server 192.168.137.1

set -eu

OUT="${OUT:-aloop-netboot}"
ALPINE_VERSION="${ALPINE_VERSION:-3.20.3}"
ARCH="${ARCH:-aarch64}"
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$HERE/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

. "$HERE/lib-boot-tree.sh"

echo "[netboot] aloop Alpine $ALPINE_VERSION $ARCH -> $OUT/ (TFTP netboot root)"

# --- 1-3. Assemble the diskless boot tree (SHARED with build-image.sh) ----------
# The netboot root IS this boot tree, laid into a directory instead of a FAT image.
BOOT="$WORK/boot"
boot_tree_fetch  "$WORK" "$BOOT"
boot_tree_apkovl "$WORK" "$BOOT"
boot_tree_config "$BOOT"

# --- 3b. Netboot overlay augmentation: keep eth0 up in real userspace -----------
# WITNESSED on the real Pi 4: after the initramfs (which brings eth0 up via
# ip=dhcp) pivots into the Alpine userspace, NOTHING re-configures eth0 — the aloop
# overlay only manages wlan0 (autoap). So the netbooted Pi silently drops off the
# wired LAN the moment userspace starts (ping/telemetry die right after the HTTP
# root fetches). A netbooted device MUST keep its wired link, so we inject an
# /etc/network/interfaces (eth0 dhcp) + enable the `networking` service into the
# apkovl for the netboot build only (the SD image boots from local media and does
# not need this). Repack the shared apkovl with the addition.
NBOVL="$WORK/nbovl"
mkdir -p "$NBOVL"
tar -xzf "$BOOT/aloop.apkovl.tar.gz" -C "$NBOVL"
mkdir -p "$NBOVL/etc/network" "$NBOVL/etc/runlevels/boot"
cat > "$NBOVL/etc/network/interfaces" <<'IFACE'
auto lo
iface lo inet loopback

# Netboot: keep the wired link up in userspace (the root came over it).
auto eth0
iface eth0 inet dhcp
IFACE
# Enable OpenRC networking at boot so eth0 comes up (symlink, marker fallback).
ln -sf /etc/init.d/networking "$NBOVL/etc/runlevels/boot/networking" 2>/dev/null \
  || : > "$NBOVL/etc/runlevels/boot/networking"
( cd "$NBOVL" && tar -czf "$BOOT/aloop.apkovl.tar.gz" . )
echo "[netboot] overlay: added eth0 dhcp + networking service (wired link stays up in userspace)"

# --- 4. Netboot-specific cmdline: HTTP-served Alpine root over the network -------
# WITNESSED on a real Pi 4 (docs/NETBOOT.md): the stock diskless cmdline expects a
# LOCAL boot medium. Over the network the Pi 4 firmware TFTP-loads config/kernel/
# initramfs, but the diskless initramfs then panics "unable to mount root fs,
# unknown-block(0,0)" because there is no block device holding the apks/modloop/
# apkovl. The Alpine RPi initramfs honors HTTP boot params — so we point it at an
# HTTP server (the serve host, image/serve-netboot.sh runs one on :8080):
#   ip=dhcp  alpine_repo=<url>/apks  modloop=<url>/boot/modloop-rpi  apkovl=<url>/...
# @NETBOOT_SERVER@ is substituted with the serve host's IP (env NETBOOT_SERVER,
# default 192.168.137.1 — the common WSL/ICS layout; override for your LAN).
NETBOOT_SERVER="${NETBOOT_SERVER:-192.168.137.1}"
NBCMD="$(sed "s/@NETBOOT_SERVER@/$NETBOOT_SERVER/g" "$ROOT/image/config/netboot-cmdline.txt" | tr '\n' ' ')"
if [ -f "$BOOT/cmdline.txt" ] && ! grep -q 'ip=dhcp' "$BOOT/cmdline.txt"; then
  # cmdline.txt must stay a SINGLE line (Pi firmware reads only the first line —
  # an embedded newline silently drops every later param, incl. ip=dhcp, so the
  # initramfs never DHCPs and drops to an emergency shell). boot_tree_config already
  # collapsed it to one line; rebuild it as one line again with the netboot params
  # appended, never `>>` after the trailing newline.
  _base="$(tr '\n' ' ' < "$BOOT/cmdline.txt")"
  printf '%s\n' "$(printf '%s %s' "$_base" "$NBCMD" | tr -s ' ' | sed 's/^ //;s/ $//')" \
    > "$BOOT/cmdline.txt"
  echo "[netboot] appended netboot cmdline as a single line (server=$NETBOOT_SERVER): $NBCMD"
fi

# --- 4b. Optional diagnostic cmdline (NETBOOT_DEBUG=1) ---------------------------
# The default cmdline is `quiet` — great for production, terrible for diagnosing a
# boot that stalls in the initramfs. With NETBOOT_DEBUG=1 we drop `quiet` and add
# `debug_init` so the Alpine init runs `set -x` and every step (Loading boot drivers
# -> Mounting boot media -> Obtaining IP via DHCP (eth0) -> apk fetch) prints to the
# serial console. Attach a serial console and power-cycle to see exactly where it
# stops. Still a SINGLE line (rebuilt, never `>>`).
if [ "${NETBOOT_DEBUG:-0}" = "1" ] && [ -f "$BOOT/cmdline.txt" ]; then
  _dbg="$(tr '\n' ' ' < "$BOOT/cmdline.txt" | sed 's/\bquiet\b//g')"
  case " $_dbg " in *" debug_init "*) : ;; *) _dbg="$_dbg debug_init" ;; esac
  printf '%s\n' "$(printf '%s' "$_dbg" | tr -s ' ' | sed 's/^ //;s/ $//')" > "$BOOT/cmdline.txt"
  echo "[netboot] NETBOOT_DEBUG=1: dropped 'quiet', added 'debug_init' (verbose serial init)"
fi

# --- 5. Sanity: the pieces the diskless initramfs needs must be in the root -----
# modloop-rpi (kernel modules squashfs) and the apkovl are mounted/restored by the
# initramfs; over TFTP they must live in the served tree, not on a local FS.
[ -f "$BOOT/boot/modloop-rpi" ]        || { echo "[netboot] ERROR: modloop-rpi missing from boot tree"; exit 1; }
[ -f "$BOOT/boot/initramfs-rpi" ]      || { echo "[netboot] ERROR: initramfs-rpi missing";            exit 1; }
[ -f "$BOOT/aloop.apkovl.tar.gz" ]     || { echo "[netboot] ERROR: apkovl missing from boot tree";    exit 1; }
[ -f "$BOOT/start4.elf" ]              || { echo "[netboot] ERROR: start4.elf (Pi4 firmware) missing"; exit 1; }
[ -f "$BOOT/fixup4.dat" ]              || { echo "[netboot] ERROR: fixup4.dat (Pi4 firmware) missing"; exit 1; }
[ -f "$BOOT/bcm2711-rpi-4-b.dtb" ]     || { echo "[netboot] ERROR: Pi4 DTB missing";                  exit 1; }

# --- 6. Publish the netboot root — ATOMICALLY -----------------------------------
# A plain directory; a TFTP server exports it as-is. (Per-serial subdirs are a
# runtime concern of the TFTP server config, documented in docs/NETBOOT.md.)
#
# WHY staged + renamed, not rm -rf + populate-in-place: a self-updating serve
# (image/serve-netboot-win.js) can rebuild $OUT while a Pi is ACTIVELY
# TFTP/HTTP-fetching from it (WITNESSED: a real Pi 4's boot-chain fetch was
# served concurrently with a live rebuild). An in-place rm -rf/cp -a leaves a
# window where $OUT is empty or half-copied — an in-flight read can get a
# spurious "not found" or a truncated file. `mv` between two directories ON THE
# SAME FILESYSTEM is atomic (a single rename(2) syscall, POSIX-guaranteed): a
# reader either sees the complete OLD tree or the complete NEW tree, never a
# partial one. The staging dir is a SIBLING of $OUT (same parent, so same
# filesystem) — NOT $WORK (mktemp -d typically lands on a different mount,
# e.g. /tmp vs a repo-relative $OUT, which would silently degrade `mv` to a
# non-atomic copy+delete across filesystems).
OUT_NEW="${OUT}.new.$$"
OUT_OLD="${OUT}.old.$$"
rm -rf "$OUT_NEW" "$OUT_OLD"
mkdir -p "$OUT_NEW"
# Copy the whole boot tree (including dotfiles like .alpine-release) into the
# staged netboot root. `cp -a "$BOOT/." "$OUT_NEW/"` copies the DIRECTORY
# CONTENTS (the trailing /. ), preserving perms, without the glob/dotfile-
# expansion pitfalls of a for-loop (an unmatched `.[!.]*` would pass a literal
# pattern to cp).
cp -a "$BOOT/." "$OUT_NEW/"

# WITNESSED on the real Pi 4: the Alpine tarball ships boot/initramfs-rpi as mode
# 600 (root-only), and cp -a PRESERVES that. A TFTP server runs unprivileged (dnsmasq
# drops to `nobody`), so it gets "Permission denied" on the initramfs and the Pi
# boots a kernel with NO initramfs -> panics "unable to mount root fs". Make the whole
# served tree world-readable (dirs +rx, files +r) so any TFTP/HTTP server can read it.
chmod -R a+rX "$OUT_NEW"

# Atomic swap: move the CURRENT $OUT aside (if it exists), rename the staged
# tree into place, then clean up the old one. The window between the two `mv`s
# is a bare rename of an already-fully-built directory — a reader either finds
# $OUT (old, still complete, momentarily) or $OUT (new, complete); it is never
# missing and never half-populated.
if [ -d "$OUT" ]; then mv "$OUT" "$OUT_OLD"; fi
mv "$OUT_NEW" "$OUT"
rm -rf "$OUT_OLD"

echo "[netboot] wrote netboot root -> $OUT/ ($(du -sh "$OUT" | cut -f1)) [atomic swap]"
echo "[netboot] serve it: see docs/NETBOOT.md (image/serve-netboot.sh — DHCP+TFTP+HTTP)"
