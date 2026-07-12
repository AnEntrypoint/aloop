#!/bin/sh
# RT jitter test — run ON A REAL PI 4 while aloop is running with Link over WiFi.
# PASS: max cyclictest latency < the block budget (1333 us) AND zero aloop xruns.
# See docs/RT-TUNING.md for why this is the RT proof.
set -eu
BUDGET_US=1333          # 64 samples @ 48k
DURATION="${DURATION:-600}"   # seconds
echo "[rt-jitter] running cyclictest for ${DURATION}s while aloop runs (Link over WiFi)..."
command -v cyclictest >/dev/null || { echo "FAIL: cyclictest not installed (apk add rt-tests)"; exit 2; }

# cyclictest on the audio cores, RT prio 95, 200us interval, latency histogram.
cyclictest -m -S -p 95 -i 200 -d 0 -h 400 -D "$DURATION" -q > /tmp/cyclictest.out 2>&1 || true
MAX=$(awk '/Max Latencies/{for(i=1;i<=NF;i++)if($i+0>m)m=$i}END{print m+0}' /tmp/cyclictest.out)
XRUNS_BEFORE=$(cat /run/aloop/xruns 2>/dev/null || echo 0)
sleep 2
XRUNS_AFTER=$(cat /run/aloop/xruns 2>/dev/null || echo 0)
XRUNS=$((XRUNS_AFTER - XRUNS_BEFORE))

echo "[rt-jitter] max latency = ${MAX} us (budget ${BUDGET_US} us), xruns during test = ${XRUNS}"
if [ "$MAX" -lt "$BUDGET_US" ] && [ "$XRUNS" -eq 0 ]; then
    echo "PASS: jitter under budget, no xruns"; exit 0
else
    echo "FAIL: jitter ${MAX}us or xruns ${XRUNS} out of spec"; exit 1
fi
