#!/bin/sh

set -eu
DIR="${1:?usage: validate-netboot.sh <netboot-root-dir>}"
BOARD="${BOARD:-pi4}"
FAIL=0
board_supports_usb_gadget() {
  case "$1" in
    pi4|pi-cm4|pi-zero2) return 0 ;;
    pi3|pi5) return 1 ;;
    *) return 1 ;;
  esac
}
note() { echo "[validate-netboot] $*"; }
ok()   { echo "  OK   $*"; }
bad()  { echo "  FAIL $*"; FAIL=1; }

[ -d "$DIR" ] || { echo "not a directory: $DIR"; exit 2; }
if [ "$BOARD" = "opi-prime" ]; then
  echo "[validate-netboot] BOARD=opi-prime has no netboot path (opi-netboot-feasibility: Allwinner's BootROM has no Pi-style native TFTP/HTTP netboot firmware) -- nothing to validate here"
  exit 0
fi
note "netboot root: $DIR ($(du -sh "$DIR" | cut -f1))"

for f in bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb; do
  [ -f "$DIR/$f" ] && ok "firmware: $f" || bad "missing Pi firmware file: $f"
done

for f in boot/vmlinuz-rpi boot/initramfs-rpi boot/modloop-rpi; do
  [ -f "$DIR/$f" ] && ok "kernel payload: $f" || bad "missing kernel payload: $f"
done

if [ -f "$DIR/config.txt" ]; then
  ok "config.txt present"
  grep -q 'include usercfg.txt' "$DIR/config.txt" && ok "config.txt includes usercfg.txt" \
    || bad "config.txt does not include usercfg.txt"
else bad "missing config.txt"; fi

if [ -f "$DIR/usercfg.txt" ]; then
  CFG=$(cat "$DIR/usercfg.txt")
  if board_supports_usb_gadget "$BOARD"; then
    echo "$CFG" | grep -q 'dwc2' && echo "$CFG" | grep -q 'peripheral' \
      && ok "usercfg.txt sets dwc2 peripheral (f_uac2 gadget)" \
      || bad "usercfg.txt missing dwc2 dr_mode=peripheral"
  else
    echo "$CFG" | grep -q 'dwc2' \
      && bad "BOARD=$BOARD has no USB-OTG peripheral controller but usercfg.txt still sets dwc2" \
      || ok "usercfg.txt correctly omits dwc2 (BOARD=$BOARD has no USB-OTG peripheral controller)"
  fi
  echo "$CFG" | grep -q 'enable_uart=1' && ok "usercfg.txt keeps serial console (enable_uart=1)" \
    || bad "usercfg.txt missing enable_uart=1 (serial debug parity)"
else bad "missing usercfg.txt"; fi

if [ -f "$DIR/cmdline.txt" ]; then
  CMD=$(cat "$DIR/cmdline.txt")
  NL=$(tr -cd '\n' < "$DIR/cmdline.txt" | wc -c | tr -d ' ')
  if [ "$NL" -le 1 ]; then
    ok "cmdline.txt is a single line (no embedded newline — Pi firmware reads line 1 only)"
  else
    bad "cmdline.txt has $NL newlines — embedded newline truncates the kernel cmdline (ip=dhcp etc. dropped)"
  fi
  echo "$CMD" | grep -q 'isolcpus' && ok "cmdline.txt has isolcpus (RT core isolation)" \
    || bad "cmdline.txt missing isolcpus tuning"
  echo "$CMD" | grep -q 'ip=dhcp' && ok "cmdline.txt has ip=dhcp (initramfs NIC bring-up for netboot)" \
    || bad "cmdline.txt missing ip=dhcp — diskless initramfs will not reach the network"
else bad "missing cmdline.txt"; fi

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
  if echo "$INV" | grep -q 'opt/aloop/aloop$'; then
    for lib in usr/lib/libasound.so.2 usr/lib/liblilv-0.so.0 usr/lib/libserd-0.so.0 \
               usr/lib/libsord-0.so.0 usr/lib/libsratom-0.so.0 usr/lib/libzix-0.so.0 \
               usr/lib/libstdc++.so.6 usr/lib/libgcc_s.so.1; do
      echo "$INV" | grep -q "$lib" && ok "apkovl: $lib" \
        || bad "apkovl missing $lib — aloop binary is present but WILL FAIL TO START (no alsa-lib/lilv/libstdc++ on device)"
    done
  fi
else
  bad "aloop.apkovl.tar.gz missing from the netboot root — the device would boot with no identity"
fi

echo ""
if [ "$FAIL" -eq 0 ]; then note "NETBOOT ROOT VALID — structurally TFTP-serviceable (on-Pi boot = docs/NETBOOT.md)"; else note "NETBOOT ROOT INVALID — see FAILs above"; fi
exit "$FAIL"
