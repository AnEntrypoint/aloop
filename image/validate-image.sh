#!/bin/sh

set -eu
IMG="${1:?usage: validate-image.sh <image>}"
BOARD="${BOARD:-pi4}"
FAIL=0
board_supports_usb_gadget() {
  case "$1" in
    pi4|pi-cm4|pi-zero2) return 0 ;;
    pi3|pi5) return 1 ;;
    *) return 1 ;;
  esac
}
note()  { echo "[validate] $*"; }
ok()    { echo "  OK   $*"; }
bad()   { echo "  FAIL $*"; FAIL=1; }

note "image: $IMG ($(du -h "$IMG" | cut -f1))"

validate_apkovl() {
  APKOVL_PATH="$1"
  [ -f "$APKOVL_PATH" ] || { bad "could not locate aloop.apkovl.tar.gz"; return; }
  INV=$(tar -tzf "$APKOVL_PATH" 2>/dev/null || true)
  for p in etc/local.d/10-rt-tune.start etc/local.d/20-usb-gadget.start \
           etc/init.d/aloop etc/init.d/autoap etc/aloop.conf etc/aloop-controls.conf \
           etc/runlevels/default/aloop; do
    echo "$INV" | grep -q "$p" && ok "apkovl: $p" || bad "apkovl missing: $p"
  done
  if echo "$INV" | grep -q 'opt/aloop/aloop$'; then
    ok "apkovl: aloop binary present"
    ARCHTMP="$(mktemp -d)"
    tar -xzf "$APKOVL_PATH" -C "$ARCHTMP" 2>/dev/null || true
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

  echo "[validate] boot-lint: runtime path references -> apkovl contents"
  LINT="$(mktemp -d)"; tar -xzf "$APKOVL_PATH" -C "$LINT" 2>/dev/null || true
  has() { [ -e "$LINT/$1" ] || [ -e "$LINT/./$1" ]; }
  for p in etc/aloop.conf etc/aloop-controls.conf \
           etc/aloop-net/hostapd.conf etc/aloop-net/wpa_supplicant.conf etc/aloop-net/dnsmasq.conf \
           opt/aloop/autoap.sh effects/user effects/home; do
    if has "$p"; then ok "boot-lint: /$p referenced and present"
    else bad "boot-lint: /$p is referenced by the runtime but MISSING from the apkovl"; fi
  done
  ACONF=$(grep -oE 'CONF_DIR:-[^}]*' "$LINT/opt/aloop/autoap.sh" 2>/dev/null | sed 's/CONF_DIR:-//' || true)
  if [ -n "$ACONF" ]; then
    REL="${ACONF#/}"
    if has "$REL"; then ok "boot-lint: autoap CONF_DIR default ($ACONF) exists in the apkovl"
    else bad "boot-lint: autoap CONF_DIR default ($ACONF) does NOT exist in the apkovl"; fi
  fi
  rm -rf "$LINT"
}

if [ "$BOARD" = "opi-prime" ]; then
  command -v losetup >/dev/null || { echo "losetup required"; exit 2; }
  command -v sfdisk  >/dev/null || { echo "fdisk/sfdisk required"; exit 2; }

  UBOOT_MAGIC=$(dd if="$IMG" bs=1 skip=8196 count=8 2>/dev/null || true)
  if [ "$UBOOT_MAGIC" = "eGON.BT0" ]; then
    ok "U-Boot SPL eGON magic present at the raw sector-8KiB offset"
  else
    bad "no eGON.BT0 SPL magic at offset 8KiB -- U-Boot region looks empty or misplaced"
  fi

  PARTLINE=$(sfdisk -d "$IMG" 2>/dev/null | grep -E 'type=83' || true)
  if [ -n "$PARTLINE" ]; then ok "ext4 (type 83) root partition present: $PARTLINE"
  else bad "no Linux (type 83) root partition in the MBR"; fi

  PART_START=$(sfdisk -d "$IMG" 2>/dev/null | awk '/img[0-9]* :/{print $4}' | head -n1 | tr -d ',')
  [ -n "$PART_START" ] || { bad "could not read the root partition's start sector"; PART_START=2048; }
  OFF=$((PART_START * 512))

  MNT="$(mktemp -d)"
  LOOP=$(sudo losetup --show -f -o "$OFF" "$IMG" 2>/dev/null || true)
  if [ -z "$LOOP" ]; then
    bad "could not attach a loop device to the root partition (needs real root -- run in CI)"
  else
    if sudo mount -o ro "$LOOP" "$MNT" 2>/dev/null; then
      ok "ext4 root partition mounts and is readable"
      [ -f "$MNT/boot/extlinux/extlinux.conf" ] && ok "boot file: extlinux.conf" || bad "missing boot file: extlinux.conf"
      if [ -f "$MNT/boot/extlinux/extlinux.conf" ]; then
        EXT=$(cat "$MNT/boot/extlinux/extlinux.conf")
        echo "$EXT" | grep -q 'KERNEL' && ok "extlinux.conf references a KERNEL" || bad "extlinux.conf missing KERNEL line"
        echo "$EXT" | grep -q 'FDT' && ok "extlinux.conf references an FDT (devicetree)" || bad "extlinux.conf missing FDT line"
        echo "$EXT" | grep -q 'isolcpus' && ok "extlinux.conf APPEND has isolcpus (RT core isolation)" \
          || bad "extlinux.conf APPEND missing isolcpus tuning"
      fi
      find "$MNT/boot" -iname 'sun50i-h5-orangepi-prime.dtb' 2>/dev/null | grep -q . \
        && ok "boot file: sun50i-h5-orangepi-prime.dtb" || bad "missing sun50i-h5-orangepi-prime.dtb"
      [ -f "$MNT/aloop.apkovl.tar.gz" ] && ok "boot file: aloop.apkovl.tar.gz" || bad "missing boot file: aloop.apkovl.tar.gz"
      [ -f "$MNT/aloop.apkovl.tar.gz" ] && validate_apkovl "$MNT/aloop.apkovl.tar.gz"
      sudo umount "$MNT"
    else
      bad "could not mount the ext4 root partition"
    fi
    sudo losetup -d "$LOOP"
  fi
  rm -rf "$MNT"
else
  command -v mdir   >/dev/null || { echo "mtools required"; exit 2; }
  command -v sfdisk >/dev/null || { echo "fdisk/sfdisk required"; exit 2; }

  PARTLINE=$(sfdisk -d "$IMG" 2>/dev/null | grep -E 'type=(c|0c)' || true)
  if [ -n "$PARTLINE" ]; then ok "FAT32-LBA partition present: $PARTLINE"
  else bad "no FAT32-LBA (type 0c) partition in the MBR"; fi
  echo "$PARTLINE" | grep -q 'bootable' && ok "partition is bootable" || note "  (note: bootable flag not set — Pi firmware does not require it)"

  OFF=$((2048 * 512))
  MT="$(mktemp)"; printf 'drive z: file="%s" offset=%s\n' "$IMG" "$OFF" > "$MT"
  export MTOOLSRC="$MT"

  LIST=$(mdir -/ -b z: 2>/dev/null || true)
  [ -n "$LIST" ] && ok "FAT partition is readable" || bad "cannot read the FAT partition"

  for f in config.txt cmdline.txt aloop.apkovl.tar.gz; do
    if echo "$LIST" | grep -qi "/$f\$\|^z:/$f\$\|$f"; then ok "boot file: $f"
    else bad "missing boot file: $f"; fi
  done
  if echo "$LIST" | grep -qiE 'start4?\.elf|bcm27[01].|kernel8?\.img|vmlinuz|boot/'; then
    ok "Pi firmware/kernel present"
  else bad "no Pi firmware/kernel (start*.elf / kernel*.img / bcm27xx dtb)"; fi

  CFG=$(mtype z:usercfg.txt 2>/dev/null || true)
  if board_supports_usb_gadget "$BOARD"; then
    echo "$CFG" | grep -q 'dwc2' && echo "$CFG" | grep -q 'peripheral' \
      && ok "usercfg.txt sets dwc2 peripheral (f_uac2 gadget mode)" \
      || bad "usercfg.txt missing dwc2 dr_mode=peripheral"
  else
    echo "$CFG" | grep -q 'dwc2' \
      && bad "BOARD=$BOARD has no USB-OTG peripheral controller but usercfg.txt still sets dwc2" \
      || ok "usercfg.txt correctly omits dwc2 (BOARD=$BOARD has no USB-OTG peripheral controller)"
  fi
  CMD=$(mtype z:cmdline.txt 2>/dev/null || true)
  CMDNL=$(mtype z:cmdline.txt 2>/dev/null | tr -cd '\n' | wc -c | tr -d ' ')
  if [ "${CMDNL:-0}" -le 1 ]; then
    ok "cmdline.txt is a single line (no embedded newline — firmware reads line 1 only)"
  else
    bad "cmdline.txt has $CMDNL newlines — embedded newline truncates the kernel cmdline (isolcpus etc. dropped)"
  fi
  echo "$CMD" | grep -q 'isolcpus' && ok "cmdline.txt has isolcpus (RT core isolation)" \
    || bad "cmdline.txt missing isolcpus tuning"

  OVLTMP="$(mktemp -d)"
  mcopy z:aloop.apkovl.tar.gz "$OVLTMP/o.tar.gz" 2>/dev/null || true
  validate_apkovl "$OVLTMP/o.tar.gz"
  rm -f "$MT"; rm -rf "$OVLTMP"
fi

echo ""
if [ "$FAIL" -eq 0 ]; then note "IMAGE VALID — structurally flashable (on-hardware boot = HARDWARE-TESTS.md)"; else note "IMAGE INVALID — see FAILs above"; fi
exit "$FAIL"
