#!/bin/sh
# aloop PREEMPT_RT kernel build — an OPTIONAL on-hardware optimization, NOT a
# prerequisite for flashing/testing (ADR-011).
#
# WHY optional-not-blocking: the flashable image ships with Alpine's STOCK
# linux-rpi kernel plus the full userspace tuning (isolcpus/nohz_full/rcu_nocbs/
# threadirqs in cmdline.txt + SCHED_FIFO + pinned affinity + mlockall in the audio
# thread). That boots and is testable from a card TODAY, and running it is how we
# MEASURE whether the stock kernel already meets the 64-sample (1.333 ms) no-xrun
# target. PREEMPT_RT bounds worst-case scheduling latency to tens of microseconds;
# build it (this script's fragment) ONLY if the on-hardware measurement shows the
# stock kernel misses the target. This is the honest order: ship + measure first,
# then optimize — see docs/RT-TUNING.md and docs/HARDWARE-TESTS.md.
#
# This script emits the RT config fragment + documents the cross-build; when the
# measurement calls for it, the CI kernel job (heavy: full cross-compile) runs it.

set -eu
KVER="${KVER:-6.12}"     # a kernel version with a mainline/near-mainline RT option
ARCH=arm64
CROSS="${CROSS:-aarch64-linux-gnu-}"

echo "[rt-kernel] building PREEMPT_RT kernel $KVER for Pi4 ($ARCH)"

# 1. Fetch the Raspberry Pi kernel source (or mainline + the RT patch for $KVER).
#    Recent kernels have PREEMPT_RT largely mainlined; enable it via config.
# 2. Start from bcm2711_defconfig (Pi4), then enable RT + the aloop knobs:
cat > rt.fragment <<'FRAG'
CONFIG_PREEMPT_RT=y
CONFIG_HIGH_RES_TIMERS=y
CONFIG_NO_HZ_FULL=y
CONFIG_RCU_NOCB_CPU=y
CONFIG_CPU_ISOLATION=y
CONFIG_HZ_1000=y
# USB gadget for f_uac2:
CONFIG_USB_CONFIGFS=y
CONFIG_USB_CONFIGFS_F_UAC2=y
CONFIG_USB_DWC2=y
CONFIG_USB_DWC2_PERIPHERAL=y
# Sound + gadget audio:
CONFIG_SND_USB_AUDIO=y
FRAG

# 3. make bcm2711_defconfig; merge rt.fragment; make -j Image modules dtbs.
#    (The CI job runs these in a cross-compile container and emits the kernel +
#     modules + the bcm2711-rpi-4-b.dtb as artifacts for the image build.)
echo "[rt-kernel] config fragment written (rt.fragment):"
cat rt.fragment
echo "[rt-kernel] (CI runs: defconfig + merge fragment + make Image modules dtbs)"
