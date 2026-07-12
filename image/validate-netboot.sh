#!/bin/sh
# Validate a built netboot root (image/build-netboot.sh output) WITHOUT a Pi.
#
# WHY: "it assembled" is not "the Pi will TFTP-boot it". This proves the netboot
# root is structurally serviceable — the Pi 4 firmware chain the bootloader fetches
# over TFTP, the kernel + initramfs + modloop, our boot config (dwc2/serial/isolcpus
# + ip=dhcp), and the apkovl with the device payload inside. Everything here is
# checkable on any host (plain files, no FAT/mtools, no root). What it CANNOT prove
# — that the Pi firmware actually DHCPs, TFTP-fetches, and boots into RAM — is the
# on-hardware step (docs/NETBOOT.md, ADR-009-style honest hardware gate).
#
# Usage: image/validate-netboot.sh aloop-netboot/
# Exits non-zero on any structural failure.

set -eu
DIR="${1:?usage: validate-netboot.sh <netboot-root-dir>}"
FAIL=0
note() { echo "[validate-netboot] $*"; }
ok()   { echo "  OK   $*"; }
bad()  { echo "  FAIL $*"; FAIL=1; }

[ -d "$DIR" ] || { echo "not a directory: $DIR"; exit 2; }
note "netboot root: $DIR ($(du -sh "$DIR" | cut -f1))"

# --- Pi 4 TFTP firmware chain (what the bootloader fetches first) --------------
for f in bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb; do
  [ -f "$DIR/$f" ] && ok "firmware: $f" || bad "missing Pi4 firmware file: $f"
done

# --- kernel + initramfs + modloop (mounted by the diskless initramfs) ----------
for f in boot/vmlinuz-rpi boot/initramfs-rpi boot/modloop-rpi; do
  [ -f "$DIR/$f" ] && ok "kernel payload: $f" || bad "missing kernel payload: $f"
done

# --- boot config: config.txt references kernel/initramfs + includes usercfg -----
if [ -f "$DIR/config.txt" ]; then
  ok "config.txt present"
  grep -q 'include usercfg.txt' "$DIR/config.txt" && ok "config.txt includes usercfg.txt" \
    || bad "config.txt does not include usercfg.txt"
else bad "missing config.txt"; fi

# usercfg.txt: dwc2 gadget + serial console parity with the SD path.
if [ -f "$DIR/usercfg.txt" ]; then
  CFG=$(cat "$DIR/usercfg.txt")
  echo "$CFG" | grep -q 'dwc2' && echo "$CFG" | grep -q 'peripheral' \
    && ok "usercfg.txt sets dwc2 peripheral (f_uac2 gadget)" \
    || bad "usercfg.txt missing dwc2 dr_mode=peripheral"
  echo "$CFG" | grep -q 'enable_uart=1' && ok "usercfg.txt keeps serial console (enable_uart=1)" \
    || bad "usercfg.txt missing enable_uart=1 (serial debug parity)"
else bad "missing usercfg.txt"; fi

# cmdline.txt: isolcpus tuning AND ip=dhcp (the netboot-specific NIC bring-up).
if [ -f "$DIR/cmdline.txt" ]; then
  CMD=$(cat "$DIR/cmdline.txt")
  echo "$CMD" | grep -q 'isolcpus' && ok "cmdline.txt has isolcpus (RT core isolation)" \
    || bad "cmdline.txt missing isolcpus tuning"
  echo "$CMD" | grep -q 'ip=dhcp' && ok "cmdline.txt has ip=dhcp (initramfs NIC bring-up for netboot)" \
    || bad "cmdline.txt missing ip=dhcp — diskless initramfs will not reach the network"
else bad "missing cmdline.txt"; fi

# --- apkovl: present in the served root + carries the device payload -----------
if [ -f "$DIR/aloop.apkovl.tar.gz" ]; then
  ok "apkovl present in netboot root (fetchable over TFTP)"
  INV=$(tar -tzf "$DIR/aloop.apkovl.tar.gz" 2>/dev/null || true)
  for p in etc/local.d/10-rt-tune.start etc/local.d/20-usb-gadget.start \
           etc/init.d/aloop etc/init.d/autoap etc/aloop.conf \
           etc/aloop-net/dnsmasq.conf opt/aloop/autoap.sh \
           effects/home effects/user etc/runlevels/default/aloop; do
    echo "$INV" | grep -q "$p" && ok "apkovl: $p" || bad "apkovl missing: $p"
  done
  echo "$INV" | grep -q 'opt/aloop/aloop$' \
    && ok "apkovl: aloop binary present" \
    || echo "  WARN apkovl has NO aloop binary (layout-only build — set ALOOP_BIN)"
  echo "$INV" | grep -q 'effects/home/.*\.lv2' \
    && ok "apkovl: home-FX LV2 present" \
    || echo "  WARN apkovl has NO home-FX LV2 (layout-only build — set LV2_DIR)"
else
  bad "aloop.apkovl.tar.gz missing from the netboot root — the device would boot with no identity"
fi

echo ""
if [ "$FAIL" -eq 0 ]; then note "NETBOOT ROOT VALID — structurally TFTP-serviceable (on-Pi boot = docs/NETBOOT.md)"; else note "NETBOOT ROOT INVALID — see FAILs above"; fi
exit "$FAIL"
