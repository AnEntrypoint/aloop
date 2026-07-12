#!/bin/sh
# aloop PREEMPT_RT kernel build (docs/RT-TUNING.md; the biggest single task).
#
# WHY: PREEMPT_RT bounds worst-case scheduling latency to tens of microseconds,
# which is what lets 64-sample (1.333 ms) audio blocks survive without xruns.
# Stock Alpine ships a non-RT kernel, so we build an RT one for the Pi 4.

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
