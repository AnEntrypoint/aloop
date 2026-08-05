#!/bin/sh

set -eu

BOARD="${BOARD:-pi4}"
OUT="${OUT:-aloop-netboot}"
ALPINE_VERSION="${ALPINE_VERSION:-3.20.3}"
ARCH="${ARCH:-aarch64}"
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$HERE/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

. "$HERE/lib-boot-tree.sh"

echo "[netboot] aloop Alpine $ALPINE_VERSION $ARCH -> $OUT/ (TFTP netboot root)"

BOOT="$WORK/boot"
boot_tree_fetch  "$WORK" "$BOOT"
boot_tree_apkovl "$WORK" "$BOOT"
boot_tree_config "$BOOT"

NBOVL="$WORK/nbovl"
mkdir -p "$NBOVL"
tar -xzf "$BOOT/aloop.apkovl.tar.gz" -C "$NBOVL"
mkdir -p "$NBOVL/etc/network" "$NBOVL/etc/runlevels/boot"
cat > "$NBOVL/etc/network/interfaces" <<'IFACE'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
IFACE
ln -sf /etc/init.d/networking "$NBOVL/etc/runlevels/boot/networking" 2>/dev/null \
  || : > "$NBOVL/etc/runlevels/boot/networking"
NBOVL_TAR="$WORK/nbovl.tar"
( cd "$NBOVL" && tar -cf "$NBOVL_TAR" . )
_nb_exec_paths="./opt/aloop/autoap.sh ./opt/aloop/usb-automount.sh \
    ./etc/local.d/10-rt-tune.start ./etc/local.d/20-usb-gadget.start ./etc/local.d/25-usb-automount.start \
    ./etc/init.d/aloop ./etc/init.d/autoap \
    $(cd "$NBOVL" && find usr/sbin -type f 2>/dev/null | sed 's|^|./|') \
    $(cd "$NBOVL" && find opt/aloop/test -type f -name '*.sh' 2>/dev/null | sed 's|^|./|')"
if [ -f "$NBOVL/opt/aloop/aloop" ]; then
  _nb_exec_paths="./opt/aloop/aloop $_nb_exec_paths"
fi
( cd "$NBOVL" && tar --mode='+x' -rf "$NBOVL_TAR" $_nb_exec_paths )
gzip -f "$NBOVL_TAR"
mv "$NBOVL_TAR.gz" "$BOOT/aloop.apkovl.tar.gz"

# Extraction on Windows loses the exec bit regardless of what the archive
# stores (NTFS has no exec bit), so modes are always read from the tar
# LISTING here, never from an extracted copy -- see AGENTS.md.
NB_LASTMODE=$(tar -tzvf "$BOOT/aloop.apkovl.tar.gz" 2>/dev/null | grep 'opt/aloop/aloop$' | tail -1 | cut -c1-10)
if [ "$NB_LASTMODE" = "-rwxr-xr-x" ]; then
  echo "[netboot] overlay: added eth0 dhcp + networking service [aloop binary confirmed +x after repack]"
elif [ -z "$NB_LASTMODE" ] && [ ! -f "$NBOVL/opt/aloop/aloop" ]; then
  echo "[netboot] overlay: added eth0 dhcp + networking service [no ALOOP_BIN this run, nothing to verify]"
else
  echo "[netboot] ERROR: aloop binary lost its +x bit during the netboot overlay repack (last entry mode: $NB_LASTMODE) — aloop service will crash-loop"
fi
for _x in usr/sbin/hostapd usr/sbin/dnsmasq; do
  _m=$(tar -tzvf "$BOOT/aloop.apkovl.tar.gz" 2>/dev/null | grep "$_x\$" | tail -1 | cut -c1-10)
  if [ -z "$_m" ]; then
    echo "[netboot] ERROR: $_x missing from the apkovl — the ticker AP cannot start"
  elif [ "$_m" = "-rwxr-xr-x" ]; then
    echo "[netboot] $_x confirmed +x in archive"
  else
    echo "[netboot] ERROR: $_x is NOT executable in the apkovl (mode: $_m) — autoap will fail to host the AP"
  fi
done

NETBOOT_SERVER="${NETBOOT_SERVER:-192.168.137.1}"
NBCMD="$(sed "s/@NETBOOT_SERVER@/$NETBOOT_SERVER/g" "$ROOT/image/config/netboot-cmdline.txt" | tr '\n' ' ')"
if [ -f "$BOOT/cmdline.txt" ] && ! grep -q 'ip=dhcp' "$BOOT/cmdline.txt"; then
  _base="$(tr '\n' ' ' < "$BOOT/cmdline.txt")"
  printf '%s\n' "$(printf '%s %s' "$_base" "$NBCMD" | tr -s ' ' | sed 's/^ //;s/ $//')" \
    > "$BOOT/cmdline.txt"
  echo "[netboot] appended netboot cmdline as a single line (server=$NETBOOT_SERVER): $NBCMD"
fi

if [ "${NETBOOT_DEBUG:-0}" = "1" ] && [ -f "$BOOT/cmdline.txt" ]; then
  _dbg="$(tr '\n' ' ' < "$BOOT/cmdline.txt" | sed 's/\bquiet\b//g')"
  case " $_dbg " in *" debug_init "*) : ;; *) _dbg="$_dbg debug_init" ;; esac
  printf '%s\n' "$(printf '%s' "$_dbg" | tr -s ' ' | sed 's/^ //;s/ $//')" > "$BOOT/cmdline.txt"
  echo "[netboot] NETBOOT_DEBUG=1: dropped 'quiet', added 'debug_init' (verbose serial init)"
fi

[ -f "$BOOT/boot/modloop-rpi" ]        || { echo "[netboot] ERROR: modloop-rpi missing from boot tree"; exit 1; }
[ -f "$BOOT/boot/initramfs-rpi" ]      || { echo "[netboot] ERROR: initramfs-rpi missing";            exit 1; }
[ -f "$BOOT/aloop.apkovl.tar.gz" ]     || { echo "[netboot] ERROR: apkovl missing from boot tree";    exit 1; }
[ -f "$BOOT/start4.elf" ]              || { echo "[netboot] ERROR: start4.elf (Pi4 firmware) missing"; exit 1; }
[ -f "$BOOT/fixup4.dat" ]              || { echo "[netboot] ERROR: fixup4.dat (Pi4 firmware) missing"; exit 1; }
[ -f "$BOOT/bcm2711-rpi-4-b.dtb" ]     || { echo "[netboot] ERROR: Pi4 DTB missing";                  exit 1; }

OUT_NEW="${OUT}.new.$$"
OUT_OLD="${OUT}.old.$$"
rm -rf "$OUT_NEW" "$OUT_OLD"
mkdir -p "$OUT_NEW"
cp -a "$BOOT/." "$OUT_NEW/"

chmod -R a+rX "$OUT_NEW"

if [ -d "$OUT" ]; then mv "$OUT" "$OUT_OLD"; fi
mv "$OUT_NEW" "$OUT"
rm -rf "$OUT_OLD"

echo "[netboot] wrote netboot root -> $OUT/ ($(du -sh "$OUT" | cut -f1)) [atomic swap]"
echo "[netboot] serve it: see docs/NETBOOT.md (image/serve-netboot.sh — DHCP+TFTP+HTTP)"
