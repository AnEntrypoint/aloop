#!/bin/sh
# aloop Alpine SD image builder — produces a REAL flashable Pi 4 image.
#
# WHY diskless/RAM (ADR-001; docs/ARCHITECTURE.md "boot to appliance"): Alpine's
# Raspberry Pi image is *diskless* — it boots a kernel + initramfs off a single
# FAT32 partition into a tmpfs root, then restores an apkovl (a tar.gz "local
# backup") over that root. No writable disk root, no background daemons, near
# bare-metal determinism. That means the WHOLE device is one FAT32 partition:
#   firmware + kernel + DTBs + overlays + config.txt + cmdline.txt + our apkovl.
# No ext4 rootfs to build, no loopback/root needed — we assemble the FAT with
# mtools (unprivileged), so this runs identically in CI and on a dev host.
#
# Inputs (env or defaults):
#   OUT             output image path              (aloop-pi4.img)
#   ALPINE_VERSION  Alpine release                 (3.20.3)
#   ALPINE_BRANCH   Alpine branch for the tarball  (v3.20)
#   ALOOP_BIN       path to the aarch64/musl aloop binary  (required for a real device)
#   LV2_DIR         dir containing the home-FX *.lv2 bundle (required for a real device)
#   IMG_MB          image size in MiB              (256 — firmware+kernel+overlay fit easily)
# A build with no ALOOP_BIN/LV2_DIR still produces a valid, bootable image (for
# pipeline/layout validation); it just has no binary/effects yet and logs a warning.

set -eu

OUT="${OUT:-aloop-pi4.img}"
ALPINE_VERSION="${ALPINE_VERSION:-3.20.3}"
ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"
ARCH="aarch64"
IMG_MB="${IMG_MB:-256}"
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$HERE/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Shared boot-tree assembly (fetch + apkovl + config) — ONE source of truth for
# both the SD image and the TFTP netboot root. See image/lib-boot-tree.sh.
. "$HERE/lib-boot-tree.sh"

echo "[image] aloop Alpine $ALPINE_VERSION $ARCH -> $OUT (${IMG_MB} MiB, diskless FAT32)"

# --- 1-3. Assemble the diskless boot tree (firmware+kernel, apkovl, config) -----
# The tarball's contents ARE the FAT partition (firmware, kernel, DTBs, overlays);
# the apkovl is the device identity; config merges dwc2/serial/isolcpus. All three
# steps are shared with the netboot builder.
BOOT="$WORK/boot"
boot_tree_fetch  "$WORK" "$BOOT"
boot_tree_apkovl "$WORK" "$BOOT"
boot_tree_config "$BOOT"

# --- 4. Lay the boot dir onto a FAT32 partition inside a partitioned image ------
# One MBR partition, type 0x0c (FAT32 LBA), bootable, starting at 1 MiB. mtools
# writes the files unprivileged (no loopback/root).
IMG="$WORK/img.raw"
dd if=/dev/zero of="$IMG" bs=1M count="$IMG_MB" status=none
# MBR: one bootable FAT32-LBA partition from sector 2048 to the end.
printf 'label: dos\n2048,,0c,*\n' | sfdisk "$IMG" >/dev/null

PART_OFFSET=$((2048 * 512))
PART="$WORK/part.fat"
FAT_MB=$((IMG_MB - 1))
dd if=/dev/zero of="$PART" bs=1M count="$FAT_MB" status=none
mkfs.vfat -F 32 -n ALOOPBOOT "$PART" >/dev/null
# copy the whole boot tree into the FAT image with mtools
( cd "$BOOT" && for f in * .[!.]*; do
    [ -e "$f" ] || continue
    if [ -d "$f" ]; then mcopy -s -i "$PART" "$f" ::/ ; else mcopy -i "$PART" "$f" ::/ ; fi
  done )
# splice the FAT partition back into the image at the partition offset
dd if="$PART" of="$IMG" bs=512 seek=2048 conv=notrunc status=none

mv "$IMG" "$OUT"
echo "[image] wrote $OUT ($(du -h "$OUT" | cut -f1))"
echo "[image] flash with: dd if=$OUT of=/dev/sdX bs=4M conv=fsync   (or Raspberry Pi Imager)"
