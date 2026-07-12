#!/bin/sh
# aloop RT tuning — applied at boot before the audio process starts.
# Every knob here has a WHY; see docs/RT-TUNING.md for the full rationale. The
# goal: worst-case scheduling jitter in the tens of microseconds so 64-sample
# (1.333 ms) audio blocks never miss their deadline (no xruns).

set -eu
log() { echo "[rt-tune] $*"; }

# --- CPU cores ---
# Cores 1 and 3 are the audio cores (home-FX and user-FX). They are isolated
# from the general scheduler via isolcpus=1,3 in cmdline.txt, so only our pinned
# SCHED_FIFO threads run on them. Core 0 = USB I/O, Core 2 = control/network.
AUDIO_CORES="1 3"
CONTROL_CORE="2"

# --- Force max clock, disable power-save that causes latency spikes ---
# WHY: frequency scaling and deep C-states park the CPU and take microseconds to
# wake — enough to blow a 1.333 ms block under load. Pin to performance.
for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$c" ] && echo performance > "$c" || true
done
# Disable deep idle states on the audio cores if the platform exposes them.
for core in $AUDIO_CORES; do
    for st in /sys/devices/system/cpu/cpu$core/cpuidle/state*/disable; do
        [ -w "$st" ] && echo 1 > "$st" || true
    done
done
log "governor=performance, deep C-states disabled on cores: $AUDIO_CORES"

# --- Steer network / WiFi / USB IRQs OFF the audio cores onto the control core ---
# WHY: this is the mechanism that lets Ableton Link over WiFi NOT glitch audio
# (ADR-006, feasibility R1). On Pi 4 these IRQs are steerable (Pi 5's RP1 is not).
CONTROL_MASK=$(printf '%x' $((1 << CONTROL_CORE)))    # bitmask for core 2
for irq in /proc/irq/*/; do
    n=$(basename "$irq")
    # Match brcmfmac (WiFi), dwc2 (USB), and generic net IRQs by their /proc name.
    name=$(cat "$irq/../$n/spurious" 2>/dev/null || true)
    if grep -qiE 'brcmfmac|mmc|dwc2|eth|wlan|xhci' "/proc/irq/$n/"* 2>/dev/null; then
        echo "$CONTROL_MASK" > "/proc/irq/$n/smp_affinity" 2>/dev/null || true
    fi
done
# Fallback: many Pi net IRQs are named in /proc/interrupts; steer any that match.
awk '/brcmfmac|dwc2|mmc|xhci|eth/{gsub(":","",$1); print $1}' /proc/interrupts 2>/dev/null | while read -r n; do
    echo "$CONTROL_MASK" > "/proc/irq/$n/smp_affinity" 2>/dev/null || true
done
log "network/USB IRQs steered to control core $CONTROL_CORE (mask $CONTROL_MASK)"

# --- rtprio + memlock limits so the audio process can go SCHED_FIFO + mlockall ---
# (Also set in /etc/security/limits or the service unit; belt-and-suspenders.)
ulimit -r 95 2>/dev/null || true      # max realtime priority
ulimit -l unlimited 2>/dev/null || true   # unlimited locked memory (mlockall)

log "RT tuning applied. aloop audio threads will run SCHED_FIFO pinned to cores: $AUDIO_CORES"
