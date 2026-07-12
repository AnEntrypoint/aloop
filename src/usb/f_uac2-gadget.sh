#!/bin/sh
# aloop USB audio gadget — configfs f_uac2 setup (ADR-008, docs/ARCHITECTURE.md).
#
# WHY this replaces looper's hand-rolled UAC2: the kernel's f_uac2 function lays
# out the isochronous USB microframes correctly by construction, deleting the
# entire class of buzz/crackle/-4608 corruption bugs the bare-metal looper had to
# find and fix by hand. The Pi presents itself to a host as a UAC2 soundcard.
#
# Runs at boot (from /etc/local.d) after libcomposite is loaded. Mono 48k s16 to
# match the looper's format exactly.

set -eu
G=/sys/kernel/config/usb_gadget/aloop
UDC=$(ls /sys/class/udc | head -n1)   # the dwc2 device controller

modprobe libcomposite || true
mkdir -p "$G"; cd "$G"

echo 0x1d6b > idVendor          # Linux Foundation
echo 0x0104 > idProduct         # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "aloop"        > strings/0x409/manufacturer
echo "aloop looper" > strings/0x409/product
echo "0001"         > strings/0x409/serialnumber

# --- the UAC2 audio function: mono, 48000, s16 ---
mkdir -p functions/uac2.0
echo 48000 > functions/uac2.0/c_srate    # capture (host -> Pi) sample rate
echo 48000 > functions/uac2.0/p_srate    # playback (Pi -> host) sample rate
# Stereo WIRE (0x3 = L+R), the same as the looper's UAC2. aloop's audio thread
# opens ALSA with this stereo wire, averages capture L/R -> mono for the Faust DSP,
# and duplicates the mono result onto both channels on playback (audio_thread.cpp
# wireCh handling). So the host sees a normal stereo soundcard; internally it is
# mono, matching the looper exactly.
echo 0x3   > functions/uac2.0/c_chmask   # capture channel mask (stereo wire)
echo 0x3   > functions/uac2.0/p_chmask   # playback channel mask (stereo wire)
echo 2     > functions/uac2.0/c_ssize    # sample size bytes (s16 = 2)
echo 2     > functions/uac2.0/p_ssize

mkdir -p configs/c.1/strings/0x409
echo "aloop UAC2" > configs/c.1/strings/0x409/configuration
ln -s functions/uac2.0 configs/c.1/

# Bind to the UDC — the gadget goes live.
echo "$UDC" > UDC
echo "[f_uac2] gadget bound to $UDC (mono/48k, presenting as a UAC2 soundcard)"
