#!/bin/sh
set -eu
MOUNT_POINT=/media/aloop-usb
ACTION="$1"
DEV="/dev/$MDEV"

case "$MDEV" in
  sd[a-z][0-9]*) ;;
  *) exit 0 ;;
esac

mkdir -p "$MOUNT_POINT"

if [ "$ACTION" = "remove" ]; then
  if grep -q "^$DEV $MOUNT_POINT " /proc/mounts 2>/dev/null; then
    umount "$MOUNT_POINT" 2>/dev/null || umount -l "$MOUNT_POINT" 2>/dev/null || true
    echo "[usb-automount] unmounted $DEV from $MOUNT_POINT"
  fi
  exit 0
fi

if grep -q " $MOUNT_POINT " /proc/mounts 2>/dev/null; then
  echo "[usb-automount] $MOUNT_POINT already occupied -- ignoring $DEV"
  exit 0
fi

for FSTYPE in "" vfat ext4 exfat ntfs; do
  if [ -z "$FSTYPE" ]; then
    mount -o rw,noatime "$DEV" "$MOUNT_POINT" 2>/dev/null && break
  else
    mount -t "$FSTYPE" -o rw,noatime "$DEV" "$MOUNT_POINT" 2>/dev/null && break
  fi
done

if grep -q " $MOUNT_POINT " /proc/mounts 2>/dev/null; then
  echo "[usb-automount] mounted $DEV at $MOUNT_POINT"
else
  echo "[usb-automount] could not mount $DEV at $MOUNT_POINT (unsupported or unformatted filesystem)"
fi
