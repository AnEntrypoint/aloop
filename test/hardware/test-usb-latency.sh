#!/bin/sh
# f_uac2 round-trip latency — run with the Pi 4 connected to a HOST as a USB
# audio device. Method: the host plays a click through the Pi gadget and records
# the return; the offset between sent and received click = round-trip latency.
# PASS: round-trip <= the bare-metal baseline (~1 block + USB, a few ms).
#
# This script runs the DEVICE side (confirm the gadget PCM is live + report the
# ALSA buffer/period the gadget negotiated). The click measurement is host-side
# (documented in docs/HARDWARE-TESTS.md), since it needs the host DAW/recorder.
set -eu
echo "[usb-latency] device-side check: is the f_uac2 gadget PCM live?"
aplay -l 2>/dev/null | grep -i uac2 || { echo "FAIL: no UAC2 gadget PCM (is f_uac2-gadget.sh run + host connected?)"; exit 1; }
PERIOD=$(cat /proc/asound/card*/pcm0p/sub0/hw_params 2>/dev/null | awk '/period_size/{print $2}')
echo "[usb-latency] gadget playback period = ${PERIOD:-unknown} frames"
echo "PASS(device-side): gadget PCM live. Run the host-side click measurement (see HARDWARE-TESTS.md)."
