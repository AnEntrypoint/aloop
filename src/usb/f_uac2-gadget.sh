#!/bin/sh
set -eu
G=/sys/kernel/config/usb_gadget/aloop

modprobe libcomposite || true
UDC=$(ls /sys/class/udc 2>/dev/null | head -n1)
if [ -z "$UDC" ]; then
  echo "[f_uac2] no USB device controller present (board has no OTG peripheral mode) -- skipping gadget setup"
  exit 0
fi
mkdir -p "$G"; cd "$G"

echo 0x1d6b > idVendor
echo 0x0104 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "aloop"        > strings/0x409/manufacturer
echo "aloop looper" > strings/0x409/product
echo "0001"         > strings/0x409/serialnumber

mkdir -p functions/uac2.0
echo 48000 > functions/uac2.0/c_srate
echo 48000 > functions/uac2.0/p_srate
echo 0x3   > functions/uac2.0/c_chmask
echo 0x3   > functions/uac2.0/p_chmask
echo 2     > functions/uac2.0/c_ssize
echo 2     > functions/uac2.0/p_ssize
if [ -e functions/uac2.0/req_number ]; then
  echo 4   > functions/uac2.0/req_number
fi

mkdir -p configs/c.1/strings/0x409
echo "aloop UAC2" > configs/c.1/strings/0x409/configuration
ln -s functions/uac2.0 configs/c.1/

echo "$UDC" > UDC
echo "[f_uac2] gadget bound to $UDC (mono/48k, presenting as a UAC2 soundcard)"
