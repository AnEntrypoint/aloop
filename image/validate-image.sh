#!/bin/sh
# Validate a built aloop-pi4.img WITHOUT a Raspberry Pi.
#
# WHY: "it built" is not "it will boot". This proves the image is well-formed — a
# valid MBR with a FAT32 boot partition holding the firmware/kernel + our boot
# config + the apkovl, and the apkovl holding the binary + effects + start scripts.
# Everything here is checkable in CI with mtools/sfdisk (no root, no Pi). What it
# CANNOT prove — that the kernel actually boots and hits the latency target — is
# the on-hardware step (docs/HARDWARE-TESTS.md, ADR-009).
#
# Usage: image/validate-image.sh aloop-pi4.img
# Exits non-zero on any structural failure.

set -eu
IMG="${1:?usage: validate-image.sh <image>}"
FAIL=0
note()  { echo "[validate] $*"; }
ok()    { echo "  OK   $*"; }
bad()   { echo "  FAIL $*"; FAIL=1; }

command -v mdir   >/dev/null || { echo "mtools required"; exit 2; }
command -v sfdisk >/dev/null || { echo "fdisk/sfdisk required"; exit 2; }

note "image: $IMG ($(du -h "$IMG" | cut -f1))"

# --- MBR partition table: one bootable FAT32-LBA partition -------------------
PARTLINE=$(sfdisk -d "$IMG" 2>/dev/null | grep -E 'type=(c|0c)' || true)
if [ -n "$PARTLINE" ]; then ok "FAT32-LBA partition present: $PARTLINE"
else bad "no FAT32-LBA (type 0c) partition in the MBR"; fi
echo "$PARTLINE" | grep -q 'bootable' && ok "partition is bootable" || note "  (note: bootable flag not set — Pi firmware does not require it)"

# --- FAT partition contents (via an offset mtools view) ---------------------
# partition starts at sector 2048 → offset 1 MiB.
OFF=$((2048 * 512))
MT="$(mktemp)"; printf 'drive z: file="%s" offset=%s\n' "$IMG" "$OFF" > "$MT"
export MTOOLSRC="$MT"

LIST=$(mdir -/ -b z: 2>/dev/null || true)
[ -n "$LIST" ] && ok "FAT partition is readable" || bad "cannot read the FAT partition"

# Required boot files (Pi 4 firmware chain + kernel + our config + overlay).
for f in config.txt cmdline.txt aloop.apkovl.tar.gz; do
  if echo "$LIST" | grep -qi "/$f\$\|^z:/$f\$\|$f"; then ok "boot file: $f"
  else bad "missing boot file: $f"; fi
done
# Pi 4 firmware/kernel — at least one of these must be present.
if echo "$LIST" | grep -qiE 'start4?\.elf|bcm2711|kernel8?\.img|vmlinuz|boot/'; then
  ok "Pi firmware/kernel present"
else bad "no Pi firmware/kernel (start*.elf / kernel*.img / bcm2711 dtb)"; fi

# --- Boot config content: dwc2 gadget + serial + isolcpus cmdline ------------
CFG=$(mtype z:usercfg.txt 2>/dev/null || true)
echo "$CFG" | grep -q 'dwc2' && echo "$CFG" | grep -q 'peripheral' \
  && ok "usercfg.txt sets dwc2 peripheral (f_uac2 gadget mode)" \
  || bad "usercfg.txt missing dwc2 dr_mode=peripheral"
CMD=$(mtype z:cmdline.txt 2>/dev/null || true)
# The Pi firmware reads ONLY the first line of cmdline.txt — an embedded newline
# silently drops every later kernel param (isolcpus etc.). Enforce a single line.
# (mtools may translate the trailing EOL; count LF bytes and allow at most one.)
CMDNL=$(mtype z:cmdline.txt 2>/dev/null | tr -cd '\n' | wc -c | tr -d ' ')
if [ "${CMDNL:-0}" -le 1 ]; then
  ok "cmdline.txt is a single line (no embedded newline — firmware reads line 1 only)"
else
  bad "cmdline.txt has $CMDNL newlines — embedded newline truncates the kernel cmdline (isolcpus etc. dropped)"
fi
echo "$CMD" | grep -q 'isolcpus' && ok "cmdline.txt has isolcpus (RT core isolation)" \
  || bad "cmdline.txt missing isolcpus tuning"

# --- apkovl contents: the device payload ------------------------------------
OVLTMP="$(mktemp -d)"
mcopy z:aloop.apkovl.tar.gz "$OVLTMP/o.tar.gz" 2>/dev/null || true
if [ -f "$OVLTMP/o.tar.gz" ]; then
  INV=$(tar -tzf "$OVLTMP/o.tar.gz" 2>/dev/null || true)
  for p in etc/local.d/10-rt-tune.start etc/local.d/20-usb-gadget.start \
           etc/init.d/aloop etc/init.d/autoap etc/aloop.conf etc/aloop-controls.conf \
           etc/runlevels/default/aloop; do
    echo "$INV" | grep -q "$p" && ok "apkovl: $p" || bad "apkovl missing: $p"
  done
  # payload (binary + effects) — WARN not FAIL if absent (a layout-only build).
  if echo "$INV" | grep -q 'opt/aloop/aloop$'; then
    ok "apkovl: aloop binary present"
    # Verify it is an aarch64 ELF (the Pi 4 target). Extract the whole overlay
    # (tar member paths may be ./opt/... or opt/...) and file(1) the binary.
    ARCHTMP="$(mktemp -d)"
    tar -xzf "$OVLTMP/o.tar.gz" -C "$ARCHTMP" 2>/dev/null || true
    BINPATH=$(find "$ARCHTMP" -path '*opt/aloop/aloop' -type f 2>/dev/null | head -n1)
    if [ -n "$BINPATH" ] && command -v file >/dev/null; then
      ARCH_DESC=$(file -b "$BINPATH")
      case "$ARCH_DESC" in
        *aarch64*|*"ARM aarch64"*) ok "aloop binary is aarch64 ELF ($ARCH_DESC)";;
        *) bad "aloop binary is NOT aarch64: $ARCH_DESC";;
      esac
    elif [ -z "$BINPATH" ]; then
      note "  (arch check skipped: binary not extractable for file(1))"
    else
      note "  (arch check skipped: file(1) unavailable)"
    fi
    # Unlike the binary/LV2 (legitimately optional for a layout-only build), the
    # vendored runtime libs (usr/lib/*.so — alsa-lib + the lilv stack) are ALWAYS
    # required once the binary is bundled. WITNESSED live on a real Pi 4: aloop
    # fails to start without them (telemetry never came up) because the device's
    # only reachable apk repo has no alsa-lib/lilv packages. Hard FAIL, not WARN.
    for lib in usr/lib/libasound.so.2 usr/lib/liblilv-0.so.0 usr/lib/libserd-0.so.0 \
               usr/lib/libsord-0.so.0 usr/lib/libsratom-0.so.0 usr/lib/libzix-0.so.0 \
               usr/lib/libstdc++.so.6 usr/lib/libgcc_s.so.1; do
      [ -f "$ARCHTMP/$lib" ] && ok "apkovl: $lib" \
        || bad "apkovl missing $lib — aloop binary is present but WILL FAIL TO START (no alsa-lib/lilv/libstdc++ on device)"
    done
    rm -rf "$ARCHTMP"
  else
    echo "  WARN apkovl has NO aloop binary (layout-only build — set ALOOP_BIN)"
  fi
  echo "$INV" | grep -q 'effects/home/.*\.lv2' && ok "apkovl: home-FX LV2 present" \
    || echo "  WARN apkovl has NO home-FX LV2 (layout-only build — set LV2_DIR)"
  echo "$INV" | grep -q 'effects/user' && ok "apkovl: /effects/user dir present" \
    || bad "apkovl missing /effects/user (user LV2 drop dir)"

  # --- BOOT-LINT: every runtime path a shipped script/service references MUST
  # exist in the apkovl. This catches the recurring "path doesn't resolve on the
  # device" class (autoap CONF_DIR, LV2 dir, config locations) before a card-test.
  # NOTE: runtime-CREATED paths under tmpfs (e.g. /run/aloop, made by
  # Telemetry::start() via mkdir) are intentionally NOT checked here — they are not
  # shipped in the apkovl; the code that writes them is responsible for creating
  # them. Only paths the runtime EXPECTS to already exist are linted.
  echo "[validate] boot-lint: runtime path references -> apkovl contents"
  LINT="$(mktemp -d)"; tar -xzf "$OVLTMP/o.tar.gz" -C "$LINT" 2>/dev/null || true
  has() { [ -e "$LINT/$1" ] || [ -e "$LINT/./$1" ]; }
  # The canonical device paths the runtime depends on (service commands + the
  # scripts' own defaults). Each MUST be present in the overlay.
  for p in etc/aloop.conf etc/aloop-controls.conf \
           etc/aloop-net/hostapd.conf etc/aloop-net/wpa_supplicant.conf etc/aloop-net/dnsmasq.conf \
           opt/aloop/autoap.sh effects/user effects/home; do
    if has "$p"; then ok "boot-lint: /$p referenced and present"
    else bad "boot-lint: /$p is referenced by the runtime but MISSING from the apkovl"; fi
  done
  # autoap's CONF_DIR default must point at a dir the image actually ships.
  ACONF=$(grep -oE 'CONF_DIR:-[^}]*' "$LINT/opt/aloop/autoap.sh" 2>/dev/null | sed 's/CONF_DIR:-//' || true)
  if [ -n "$ACONF" ]; then
    REL="${ACONF#/}"
    if has "$REL"; then ok "boot-lint: autoap CONF_DIR default ($ACONF) exists in the apkovl"
    else bad "boot-lint: autoap CONF_DIR default ($ACONF) does NOT exist in the apkovl"; fi
  fi
  rm -rf "$LINT"
else
  bad "could not extract aloop.apkovl.tar.gz from the image"
fi

rm -f "$MT"; rm -rf "$OVLTMP"
echo ""
if [ "$FAIL" -eq 0 ]; then note "IMAGE VALID — structurally flashable (on-Pi boot = HARDWARE-TESTS.md)"; else note "IMAGE INVALID — see FAILs above"; fi
exit "$FAIL"
