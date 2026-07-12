#!/bin/sh
# image/serve-netboot.sh — serve a built netboot root to a Pi 4 over DHCP + TFTP + HTTP.
#
# WITNESSED working on a real Pi 4 (serial 7bec0617) over a WSL2/ICS link — see
# docs/NETBOOT.md. WHY a script: serving is done every test iteration; this wraps
# the exact witnessed invocation so it is one command, not hand-typed flags.
#
# What it runs, and why each piece is needed (all learned from a real boot):
#   1. dnsmasq DHCP  — hands the Pi an IP + the boot filename. Two modes:
#        --standalone (default): dnsmasq owns the DHCP lease. Use when the LAN has
#          NO other DHCP server actually leasing (e.g. a Windows ICS link that does
#          not lease to the Pi — the observed case). Pi got no address in proxy
#          mode, so standalone is the working default.
#        --proxy: only add boot options on top of another DHCP server's lease. Use
#          when a real DHCP server already leases to the Pi.
#   2. dnsmasq TFTP  — the Pi 4 firmware fetches bootcode/start4.elf/config.txt/
#      kernel/initramfs. The Pi requests under its board-serial subdir (<serial>/);
#      this script auto-creates <root>/<serial>/ symlinked to the root the first
#      time it sees the serial in the log (or serve flat — dnsmasq falls back).
#   3. HTTP server (:8080) — the Alpine diskless initramfs then fetches the ROOT
#      filesystem (apks repo, modloop, apkovl) over HTTP, per the cmdline
#      alpine_repo=/modloop=/apkovl= that build-netboot.sh baked in. WITHOUT this,
#      the initramfs panics "unable to mount root fs unknown-block(0,0)" — there is
#      no block device over the network. This HTTP server IS the root medium.
#
# Usage (root, on a Linux/WSL host on the same L2 as the Pi):
#   sudo image/serve-netboot.sh [--iface eth0] [--root /srv/tftp/aloop-netboot] \
#        [--server 192.168.137.1] [--net 192.168.137.0] [--proxy]
#
#   --iface    interface cabled to the Pi           (default eth0)
#   --root     the netboot root (build-netboot.sh)  (default /srv/tftp/aloop-netboot)
#   --server   this host's IP on that link          (default 192.168.137.1)
#   --net      the LAN network                       (default 192.168.137.0)
#   --proxy    proxy DHCP instead of standalone (another DHCP server leases)
#
# NOTE: the netboot root must have been built with a MATCHING NETBOOT_SERVER so the
# cmdline's HTTP URLs point back here:  NETBOOT_SERVER=<--server> image/build-netboot.sh
#
# Leave it running; watch the log for the Pi's DHCPACK -> TFTP GETs -> HTTP GETs of
# modloop/apkovl. Ctrl-C stops everything.

set -eu

IFACE="eth0"
ROOT="/srv/tftp/aloop-netboot"
SERVER="192.168.137.1"
NET="192.168.137.0"
MODE="standalone"

while [ $# -gt 0 ]; do
  case "$1" in
    --iface)  IFACE="$2"; shift 2 ;;
    --root)   ROOT="$2"; shift 2 ;;
    --server) SERVER="$2"; shift 2 ;;
    --net)    NET="$2"; shift 2 ;;
    --proxy)  MODE="proxy"; shift ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

command -v dnsmasq >/dev/null || { echo "dnsmasq not installed (apt-get install dnsmasq)"; exit 2; }
command -v python3 >/dev/null || { echo "python3 needed for the HTTP root server";        exit 2; }
[ -d "$ROOT" ] || { echo "netboot root not found: $ROOT (run image/build-netboot.sh first)"; exit 2; }
[ -f "$ROOT/start4.elf" ] || { echo "netboot root looks wrong: no start4.elf in $ROOT"; exit 2; }

if [ "$MODE" = "standalone" ]; then
  BASE="$(echo "$NET" | sed 's/\.0$//')"
  DHCP="--dhcp-range=${BASE}.50,${BASE}.150,255.255.255.0,1h --dhcp-boot=bootcode.bin"
  echo "[serve] STANDALONE DHCP on $IFACE (${BASE}.50-.150) — ensure NO other DHCP server leases here"
else
  DHCP="--dhcp-range=${NET},proxy"
  echo "[serve] PROXY DHCP on $IFACE for $NET (another DHCP server must lease the Pi)"
fi

cleanup() { kill "$HTTP_PID" 2>/dev/null || true; kill "$DNS_PID" 2>/dev/null || true; }
trap cleanup INT TERM EXIT

# 3. HTTP root server (the network root medium the initramfs mounts).
( cd "$ROOT" && exec python3 -m http.server 8080 --bind "$SERVER" ) >/tmp/aloop-http.log 2>&1 &
HTTP_PID=$!
echo "[serve] HTTP root  http://$SERVER:8080/  (apks / modloop-rpi / apkovl)"

# 1+2. dnsmasq DHCP + TFTP + Pi4 boot options.
# --dhcp-authoritative: WITNESSED necessary — when another DHCP server shares the
# link (e.g. Windows ICS on the same NIC), the Pi 4 would loop DISCOVER<->OFFER and
# never REQUEST our address; authoritative mode makes dnsmasq answer decisively so
# the handshake completes (DISCOVER->OFFER->REQUEST->ACK) and boot proceeds to TFTP.
# shellcheck disable=SC2086
dnsmasq --keep-in-foreground --log-dhcp \
  --interface="$IFACE" --bind-interfaces --except-interface=lo \
  --dhcp-authoritative \
  $DHCP \
  --dhcp-vendorclass=set:rpi,PXEClient \
  --dhcp-option-force=tag:rpi,43,"Raspberry Pi Boot" \
  --enable-tftp --tftp-root="$ROOT" --tftp-no-fail \
  --port=0 >/tmp/aloop-dnsmasq.log 2>&1 &
DNS_PID=$!
echo "[serve] TFTP root: $ROOT   (Pi requests under <serial>/ — auto-linked below)"
echo "[serve] watching for the Pi's DHCP + TFTP + HTTP — Ctrl-C to stop"
echo "[serve] ------------------------------------------------------------"

# dnsmasq TFTP/HTTP run unprivileged — the Alpine initramfs ships mode-600, which
# would fail to send. Ensure the served tree is world-readable (build-netboot.sh
# also does this; belt-and-suspenders for a hand-placed root).
chmod -R a+rX "$ROOT" 2>/dev/null || true

# Auto-create the Pi's per-serial TFTP subdir the moment it appears in the TFTP log
# (the Pi requests <serial>/start4.elf; symlink the serial dir to the root once).
SEEN=""
while kill -0 "$DNS_PID" 2>/dev/null; do
  SER="$(grep -oE 'file /[^ ]*/[0-9a-f]{8}/start4\.elf' /tmp/aloop-dnsmasq.log 2>/dev/null \
         | grep -oE '/[0-9a-f]{8}/' | tr -d / | head -n1 || true)"
  if [ -n "$SER" ] && [ "$SER" != "$SEEN" ] && [ ! -e "$ROOT/$SER" ]; then
    ( cd "$ROOT" && mkdir -p "$SER" && for f in *; do
        [ "$f" = "$SER" ] && continue; ln -sf "../$f" "$SER/$f"; done )
    echo "[serve] created per-serial TFTP dir $SER/ -> root (Pi will re-fetch)"
    SEEN="$SER"
  fi
  tail -n 3 /tmp/aloop-dnsmasq.log 2>/dev/null | grep -E 'DHCPACK|tftp:' || true
  sleep 3
done
