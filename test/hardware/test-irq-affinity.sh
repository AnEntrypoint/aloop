#!/bin/sh
# IRQ affinity test — run ON A PI 4 after kernel/rt-tune.sh has applied.
# PASS: no WiFi/USB IRQ maps to the audio cores (1,3); audio-core mask excludes them.
set -eu
AUDIO_CORES_MASK=0xa      # cores 1 and 3 (bit1|bit3)
bad=0
for irq in /proc/irq/*/; do
    n=$(basename "$irq")
    if grep -qiE 'brcmfmac|dwc2|mmc|xhci|eth|wlan' "/proc/irq/$n/"* 2>/dev/null; then
        aff=$(cat "/proc/irq/$n/smp_affinity" 2>/dev/null || echo 0)
        # If the affinity mask intersects the audio-core mask, it is on an audio core.
        if [ $(( 0x$aff & 0xa )) -ne 0 ]; then
            echo "  IRQ $n (net/usb) is on an audio core: mask=$aff"; bad=1
        fi
    fi
done
if [ "$bad" -eq 0 ]; then echo "PASS: no net/USB IRQ on the audio cores"; exit 0
else echo "FAIL: some net/USB IRQ lands on an audio core — rt-tune.sh affinity did not take"; exit 1; fi
