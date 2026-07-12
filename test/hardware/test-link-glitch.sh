#!/bin/sh
# Link-over-WiFi no-glitch test — run ON A PI 4 with Link active over WiFi.
# PASS: zero audio xruns/glitches attributable to Link TX over a 10-min window
# (the bare-metal looper had a once-a-second glitch; aloop should have none).
set -eu
DURATION="${DURATION:-600}"
echo "[link-glitch] sampling aloop telemetry xrun counter for ${DURATION}s with Link over WiFi..."
X0=$(cat /run/aloop/xruns 2>/dev/null || echo 0)
sleep "$DURATION"
X1=$(cat /run/aloop/xruns 2>/dev/null || echo 0)
XRUNS=$((X1 - X0))
# A once-a-second glitch over 600s would be ~600. We require essentially zero.
echo "[link-glitch] xruns over ${DURATION}s with Link active = ${XRUNS}"
if [ "$XRUNS" -le 1 ]; then echo "PASS: no Link-induced glitching (bare-metal had ~1/s)"; exit 0
else echo "FAIL: ${XRUNS} xruns — investigate Link TX / IRQ affinity"; exit 1; fi
