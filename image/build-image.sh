#!/bin/sh

set -eu

BOARD="${BOARD:-pi4}"
OUT="${OUT:-aloop-$BOARD.img}"
ALPINE_VERSION="${ALPINE_VERSION:-3.20.3}"
ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"
ARCH="aarch64"
IMG_MB="${IMG_MB:-256}"
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$HERE/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

. "$HERE/lib-boot-tree.sh"

echo "[image] aloop Alpine $ALPINE_VERSION $ARCH BOARD=$BOARD -> $OUT (${IMG_MB} MiB, diskless FAT32)"

BOOT="$WORK/boot"
boot_tree_fetch  "$WORK" "$BOOT"
boot_tree_apkovl "$WORK" "$BOOT"
boot_tree_config "$BOOT"

if [ "$BOARD" = "opi-prime" ]; then
  if ! command -v mkfs.ext4 >/dev/null 2>&1 || ! command -v losetup >/dev/null 2>&1; then
    echo "[image] ERROR: mkfs.ext4/losetup unavailable -- Orange Pi Prime image assembly needs a real Linux host with root (works in CI; not on this dev host)" >&2
    exit 1
  fi
  IMG="$WORK/img.raw"
  dd if=/dev/zero of="$IMG" bs=1M count="$IMG_MB" status=none
  printf 'label: dos\n2048,,83,*\n' | sfdisk "$IMG" >/dev/null

  PART_OFFSET=$((2048 * 512))
  PART="$WORK/part.ext4"
  ROOT_MB=$((IMG_MB - 1))
  dd if=/dev/zero of="$PART" bs=1M count="$ROOT_MB" status=none
  mkfs.ext4 -q -L aloopboot "$PART"

  MNT="$WORK/mnt"
  mkdir -p "$MNT"
  LOOP=$(sudo losetup --show -f "$PART")
  sudo mount "$LOOP" "$MNT"
  sudo mkdir -p "$MNT/boot"
  sudo cp -r "$BOOT/opi-boot/." "$MNT/boot/"
  sudo cp "$BOOT/aloop.apkovl.tar.gz" "$MNT/"
  sudo umount "$MNT"
  sudo losetup -d "$LOOP"

  dd if="$PART" of="$IMG" bs=512 seek=2048 conv=notrunc status=none

  UBOOT_BIN="$BOOT/opi-uboot/u-boot-sunxi-with-spl.bin"
  [ -f "$UBOOT_BIN" ] || { echo "[image] ERROR: $UBOOT_BIN missing -- boot_tree_fetch_opi did not produce a U-Boot blob" >&2; exit 1; }
  UBOOT_BYTES=$(wc -c < "$UBOOT_BIN")
  UBOOT_MAX_BYTES=$((PART_OFFSET - 8192))
  if [ "$UBOOT_BYTES" -gt "$UBOOT_MAX_BYTES" ]; then
    echo "[image] ERROR: extracted U-Boot blob is ${UBOOT_BYTES} bytes, larger than the ${UBOOT_MAX_BYTES}-byte pre-partition span available -- it would overlap the root partition" >&2
    exit 1
  fi
  dd if="$UBOOT_BIN" of="$IMG" bs=1024 seek=8 conv=notrunc status=none
  echo "[image] wrote U-Boot ($UBOOT_BYTES bytes) to raw sector offset 8KiB, after the partition splice"
  WRITTEN_MAGIC=$(dd if="$IMG" bs=1 skip=8196 count=8 2>/dev/null || true)
  if [ "$WRITTEN_MAGIC" != "eGON.BT0" ]; then
    echo "[image] ERROR: eGON.BT0 SPL magic not found at byte 8196 after the U-Boot write (got '$WRITTEN_MAGIC') -- the extracted U-Boot blob's own byte 0 must be the real SPL header start, not source-relative padding" >&2
    exit 1
  fi

  mv "$IMG" "$OUT"
  echo "[image] wrote $OUT ($(du -h "$OUT" | cut -f1))"
  echo "[image] flash with: dd if=$OUT of=/dev/sdX bs=4M conv=fsync"
else
  IMG="$WORK/img.raw"
  dd if=/dev/zero of="$IMG" bs=1M count="$IMG_MB" status=none
  printf 'label: dos\n2048,,0c,*\n' | sfdisk "$IMG" >/dev/null

  PART_OFFSET=$((2048 * 512))
  PART="$WORK/part.fat"
  FAT_MB=$((IMG_MB - 1))
  dd if=/dev/zero of="$PART" bs=1M count="$FAT_MB" status=none
  mkfs.vfat -F 32 -n ALOOPBOOT "$PART" >/dev/null
  ( cd "$BOOT" && for f in * .[!.]*; do
      [ -e "$f" ] || continue
      if [ -d "$f" ]; then mcopy -s -i "$PART" "$f" ::/ ; else mcopy -i "$PART" "$f" ::/ ; fi
    done )
  dd if="$PART" of="$IMG" bs=512 seek=2048 conv=notrunc status=none

  mv "$IMG" "$OUT"
  echo "[image] wrote $OUT ($(du -h "$OUT" | cut -f1))"
  echo "[image] flash with: dd if=$OUT of=/dev/sdX bs=4M conv=fsync   (or Raspberry Pi Imager)"
fi
