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

echo "[image] aloop Alpine $ALPINE_VERSION $ARCH -> $OUT (${IMG_MB} MiB, diskless FAT32)"

# --- 1. Fetch the Alpine RPi tarball (kernel + firmware + boot files) ----------
TARBALL="alpine-rpi-${ALPINE_VERSION}-${ARCH}.tar.gz"
URL="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/${ARCH}/${TARBALL}"
if [ -n "${ALPINE_TARBALL:-}" ] && [ -f "${ALPINE_TARBALL}" ]; then
  echo "[image] using provided tarball ${ALPINE_TARBALL}"
  cp "${ALPINE_TARBALL}" "$WORK/$TARBALL"
else
  echo "[image] downloading $URL"
  curl -fsSL "$URL" -o "$WORK/$TARBALL"
fi

BOOT="$WORK/boot"
mkdir -p "$BOOT"
# The tarball's contents ARE the FAT partition (firmware, kernel, DTBs, overlays).
tar -xzf "$WORK/$TARBALL" -C "$BOOT"
echo "[image] extracted boot files:"; ls "$BOOT" | head

# --- 2. Assemble the apkovl overlay (the device's identity) --------------------
# One source of truth for the device layout. Everything here lands read-only into
# the tmpfs root at boot, restored by Alpine's diskless lbu mechanism.
OVL="$WORK/ovl"
mkdir -p "$OVL/etc/local.d" "$OVL/etc/runlevels/boot" "$OVL/etc/runlevels/default" \
         "$OVL/etc/init.d" "$OVL/opt/aloop" "$OVL/effects/home" "$OVL/effects/user"

# Boot-time scripts (run by the `local` service in order).
cp "$ROOT/kernel/rt-tune.sh"        "$OVL/etc/local.d/10-rt-tune.start"
cp "$ROOT/src/usb/f_uac2-gadget.sh" "$OVL/etc/local.d/20-usb-gadget.start"
cp "$ROOT/src/net/autoap.sh"        "$OVL/opt/aloop/autoap.sh"
cp -r "$ROOT/src/net/config"        "$OVL/etc/aloop-net"
cp "$ROOT/config/aloop.conf"        "$OVL/etc/aloop.conf"
cp "$ROOT/config/controls.conf"     "$OVL/etc/aloop-controls.conf"
# The on-hardware test suite ships on the device so it can be run from the card
# (docs/FLASHING.md step 6 / HARDWARE-TESTS.md).
if [ -d "$ROOT/test/hardware" ]; then
  mkdir -p "$OVL/opt/aloop/test"
  cp -r "$ROOT/test/hardware" "$OVL/opt/aloop/test/hardware"
  chmod +x "$OVL/opt/aloop/test/hardware/"*.sh 2>/dev/null || true
fi

# The aloop binary + home-FX LV2 (the real device payload).
if [ -n "${ALOOP_BIN:-}" ] && [ -f "${ALOOP_BIN}" ]; then
  cp "${ALOOP_BIN}" "$OVL/opt/aloop/aloop"; chmod +x "$OVL/opt/aloop/aloop"
  echo "[image] laid in aloop binary ($(du -h "${ALOOP_BIN}" | cut -f1))"
else
  echo "[image] WARNING: no ALOOP_BIN — image will boot but has no aloop binary"
fi
if [ -n "${LV2_DIR:-}" ] && [ -d "${LV2_DIR}" ]; then
  find "${LV2_DIR}" -maxdepth 2 -name '*.lv2' -exec cp -r {} "$OVL/effects/home/" \;
  echo "[image] laid in home-FX LV2: $(ls "$OVL/effects/home")"
else
  echo "[image] WARNING: no LV2_DIR — image has no home-FX effects bundle"
fi

# aloop as a SUPERVISED OpenRC service (respawn on crash + logging) — an appliance,
# not a fire-and-forget script. autoap is its own service too.
cat > "$OVL/etc/init.d/aloop" <<'SVC'
#!/sbin/openrc-run
name="aloop"
description="aloop RT audio looper + effects"
command="/opt/aloop/aloop"
command_args="--config /etc/aloop.conf"
command_background=true
pidfile="/run/aloop.pid"
output_log="/var/log/aloop.log"
error_log="/var/log/aloop.log"
respawn_delay=2
respawn_max=0
depend() { after local; need localmount; }
SVC
cat > "$OVL/etc/init.d/autoap" <<'SVC'
#!/sbin/openrc-run
name="autoap"
description="aloop WiFi: join known net, else host an AP (for Ableton Link)"
command="/opt/aloop/autoap.sh"
command_background=true
pidfile="/run/autoap.pid"
respawn_delay=2
respawn_max=0
depend() { after local; }
SVC
chmod +x "$OVL/etc/local.d/"*.start "$OVL/opt/aloop/"*.sh "$OVL/etc/init.d/aloop" "$OVL/etc/init.d/autoap"

# Enable the services in the right runlevels (OpenRC = symlinks under runlevels/).
#   local  (boot)    runs /etc/local.d/*.start
#   aloop  (default) the supervised audio process
#   autoap (default) the wifi manager
ln -sf /etc/init.d/local  "$OVL/etc/runlevels/boot/local"    2>/dev/null || true
ln -sf /etc/init.d/aloop  "$OVL/etc/runlevels/default/aloop"
ln -sf /etc/init.d/autoap "$OVL/etc/runlevels/default/autoap"

# hostname + the .apkovl name Alpine's diskless boot auto-restores.
echo "aloop" > "$OVL/etc/hostname"

# Package the overlay as Alpine's local-backup tarball. Diskless RPi restores the
# file named <hostname>.apkovl.tar.gz found on the boot FAT partition.
APKOVL="aloop.apkovl.tar.gz"
( cd "$OVL" && tar -czf "$WORK/$APKOVL" . )
cp "$WORK/$APKOVL" "$BOOT/$APKOVL"
echo "[image] apkovl -> $BOOT/$APKOVL ($(du -h "$WORK/$APKOVL" | cut -f1))"

# --- 3. Boot partition config (config.txt / cmdline.txt / usercfg.txt) ---------
# Merge our boot additions: dwc2 peripheral (f_uac2), serial console, and append
# the RT cmdline (isolcpus etc.). Alpine RPi reads usercfg.txt from config.txt.
cat "$ROOT/image/config/usercfg.txt" >> "$BOOT/usercfg.txt"
# ensure config.txt includes usercfg.txt (Alpine's default does; add if absent).
if [ -f "$BOOT/config.txt" ] && ! grep -q 'include usercfg.txt' "$BOOT/config.txt"; then
  echo "include usercfg.txt" >> "$BOOT/config.txt"
fi
# Append the RT tuning to the kernel cmdline (single line, space-joined).
if [ -f "$BOOT/cmdline.txt" ]; then
  printf ' %s' "$(cat "$ROOT/kernel/cmdline.txt")" >> "$BOOT/cmdline.txt"
else
  cat "$ROOT/kernel/cmdline.txt" > "$BOOT/cmdline.txt"
fi
echo "[image] boot config merged (dwc2 + serial + isolcpus cmdline)"

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
