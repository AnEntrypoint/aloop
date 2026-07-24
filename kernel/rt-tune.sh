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
# WITNESSED live on a real Pi 4 (this session): this write was silently a no-op
# every boot -- `cat scaling_governor` on the running device showed `schedutil`
# still active despite `local` (this script's own service) reporting
# `started`. A [diag-gap] instrumentation elsewhere in this codebase showed a
# recurring ~37-41ms stall on the audio thread's per-block read, a magnitude
# consistent with schedutil's own periodic load-sampling/frequency-transition
# work running on the "isolated" cores (isolcpus only removes ordinary
# scheduler-domain tasks, never cpufreq's own kernel-internal governor logic).
# Manually re-running `echo performance > .../scaling_governor` on the booted
# device DID take effect immediately and held -- so the sysfs write mechanism
# itself works; only this boot-time attempt failed. Root cause suspected but
# unconfirmed: the cpufreq driver's sysfs nodes likely don't exist yet this
# early in boot (`local` runs in the `boot` runlevel, before any confirmed
# cpufreq-ready signal), so the glob matched nothing and the whole loop
# silently no-op'd via the `|| true` guard, exactly the kind of failure this
# script's own defensive style masks instead of catching. Fix: retry with a
# bounded wait for the sysfs path to exist, and log a REAL result (not just
# best-effort silence) so a future boot's failure is visible instead of
# invisible.
for CPU_N in 0 1 2 3; do
    GOV_PATH="/sys/devices/system/cpu/cpu$CPU_N/cpufreq/scaling_governor"
    tries=0
    while [ ! -w "$GOV_PATH" ] && [ "$tries" -lt 20 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
    if [ -w "$GOV_PATH" ]; then
        echo performance > "$GOV_PATH"
        applied=$(cat "$GOV_PATH" 2>/dev/null || echo "unreadable")
        if [ "$applied" = "performance" ]; then
            log "cpu$CPU_N governor -> performance (confirmed, ${tries}00ms wait)"
        else
            log "WARNING: cpu$CPU_N governor write succeeded but readback shows '$applied', not performance"
        fi
    else
        log "WARNING: cpu$CPU_N has no cpufreq/scaling_governor after 2s wait -- governor NOT pinned, schedutil's own periodic bookkeeping may still stall this core"
    fi
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
