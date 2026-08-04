#!/bin/sh
set -eu
CONF=/etc/mdev.conf
if ! grep -q usb-automount.sh "$CONF" 2>/dev/null; then
  cat >> "$CONF" <<'EOF'
sd[a-z][0-9]* root:disk 660 @/opt/aloop/usb-automount.sh add
sd[a-z][0-9]* root:disk 660 $/opt/aloop/usb-automount.sh remove
EOF
fi

for DEV in /dev/sd[a-z][0-9]*; do
  [ -b "$DEV" ] || continue
  MDEV=$(basename "$DEV") /opt/aloop/usb-automount.sh add
done
