#!/bin/sh
# aloop autoAP — WiFi mode-switching (ADR-007, docs/ARCHITECTURE.md).
#
# WHY this exists: the Pi should join a known network (station mode) so it can
# reach the wider Ableton Link session, but when NO known network is available it
# must host its OWN access point so nearby devices can still Link to it. That is
# "AP when external unavailable" — mode SWITCHING, not simultaneous AP+STA (which
# is flaky on a single Pi radio). The bare-metal looper hand-rolled exactly this
# logic; here it's a small shell state machine over the standard tools.
#
# State machine:
#   try STA  ── join a known net + get DHCP ─► STAY STA (recheck periodically)
#      │ fail
#      ▼
#   host AP  ── run hostapd + dnsmasq at 192.168.4.1 ─► serve Link peers
#      │ periodically re-scan for a known net
#      ▼
#   known net reappears (and no AP clients attached) ─► switch back to STA
#
# Hysteresis: don't flap. Require N consecutive failed STA attempts before going
# AP, and don't leave AP while clients are attached.

set -eu
IFACE="${IFACE:-wlan0}"
# The net configs (hostapd/wpa_supplicant/dnsmasq) are installed by the image at
# /etc/aloop-net (image/build-image.sh: src/net/config -> /etc/aloop-net). Default
# there; env-overridable for a dev checkout (CONF_DIR=src/net/config ./autoap.sh).
CONF_DIR="${CONF_DIR:-/etc/aloop-net}"
AP_IP="192.168.4.1/24"
SCAN_INTERVAL="${SCAN_INTERVAL:-15}"     # seconds between state checks
STA_FAIL_LIMIT="${STA_FAIL_LIMIT:-3}"    # consecutive STA fails before AP (hysteresis)

log() { echo "[autoap] $*"; }

known_net_available() {
    # A known net is available if wpa_supplicant can associate + we get an IP.
    wpa_supplicant -B -i "$IFACE" -c "$CONF_DIR/wpa_supplicant.conf" 2>/dev/null || true
    sleep 6
    if udhcpc -i "$IFACE" -n -q >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

start_ap() {
    log "no known network — hosting AP 'aloop' at $AP_IP"
    pkill wpa_supplicant 2>/dev/null || true
    ip addr flush dev "$IFACE" || true
    ip addr add "$AP_IP" dev "$IFACE"
    ip link set "$IFACE" up
    hostapd -B "$CONF_DIR/hostapd.conf"
    dnsmasq -C "$CONF_DIR/dnsmasq.conf"
}

stop_ap() {
    pkill dnsmasq 2>/dev/null || true
    pkill hostapd 2>/dev/null || true
    ip addr flush dev "$IFACE" || true
}

ap_has_clients() {
    # Any station associated with our AP? (iw returns stations while AP is up.)
    [ -n "$(iw dev "$IFACE" station dump 2>/dev/null)" ]
}

state="STA"
fails=0
while true; do
    case "$state" in
        STA)
            if known_net_available; then
                log "joined a known network (STA)"
                fails=0
            else
                fails=$((fails + 1))
                log "STA attempt failed ($fails/$STA_FAIL_LIMIT)"
                if [ "$fails" -ge "$STA_FAIL_LIMIT" ]; then
                    start_ap
                    state="AP"
                fi
            fi
            ;;
        AP)
            # Only leave the AP if a known network is reachable AND no client is
            # currently attached (don't drop peers mid-session). We probe for a
            # known net by a passive scan so we don't tear down the AP to test.
            if ! ap_has_clients && iw dev "$IFACE" scan 2>/dev/null | grep -qFf <(grep -oE 'ssid="[^"]*"' "$CONF_DIR/wpa_supplicant.conf" | sed 's/ssid="//;s/"//'); then
                log "a configured network is in range and no AP clients — switching back to STA"
                stop_ap
                state="STA"
                fails=0
            fi
            ;;
    esac
    sleep "$SCAN_INTERVAL"
done
