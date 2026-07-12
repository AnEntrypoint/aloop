#!/bin/sh
# image/lib-boot-tree.sh — the ONE source of truth for the aloop Pi 4 boot tree.
#
# WHY this exists: the device boots the SAME diskless Alpine tree whether it comes
# off an SD FAT32 partition (image/build-image.sh) or over TFTP/DHCP network boot
# (image/build-netboot.sh). Both paths need the identical steps: fetch the Alpine
# RPi tarball, assemble the apkovl overlay (the device identity), and merge our
# boot config (dwc2 gadget + serial + isolcpus cmdline). Duplicating those steps in
# two scripts would drift the moment one is edited — so they live here once and are
# sourced by both builders. The builders differ ONLY in the final packaging (FAT
# image vs plain TFTP directory).
#
# Contract for callers: set ROOT (repo root) and a WORK dir, then call:
#   boot_tree_fetch   "$WORK" "$BOOT"   -> extract firmware/kernel/DTBs into $BOOT
#   boot_tree_apkovl  "$WORK" "$BOOT"   -> build+drop aloop.apkovl.tar.gz into $BOOT
#   boot_tree_config  "$BOOT"           -> merge usercfg.txt + isolcpus cmdline
# Env respected (same defaults as build-image.sh): ALPINE_VERSION, ALPINE_BRANCH,
# ARCH, ALPINE_TARBALL, ALOOP_BIN, LV2_DIR.

ALPINE_VERSION="${ALPINE_VERSION:-3.20.3}"
ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"
ARCH="${ARCH:-aarch64}"

# --- 1. Fetch + extract the Alpine RPi tarball (firmware + kernel + DTBs) -------
# The tarball's contents ARE the boot tree: start4.elf/fixup4.dat/bootcode.bin
# (Pi 4 firmware chain — the SAME files the TFTP bootloader fetches), the bcm2711
# DTBs, boot/vmlinuz-rpi + boot/initramfs-rpi + boot/modloop-rpi, config.txt,
# cmdline.txt, overlays/, and apks/. Extracting it is identical for SD and netboot.
boot_tree_fetch() {
  _work="$1"; _boot="$2"
  _tarball="alpine-rpi-${ALPINE_VERSION}-${ARCH}.tar.gz"
  _url="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/${ARCH}/${_tarball}"
  if [ -n "${ALPINE_TARBALL:-}" ] && [ -f "${ALPINE_TARBALL}" ]; then
    echo "[boot-tree] using provided tarball ${ALPINE_TARBALL}"
    cp "${ALPINE_TARBALL}" "$_work/$_tarball"
  else
    echo "[boot-tree] downloading $_url"
    curl -fsSL "$_url" -o "$_work/$_tarball"
  fi
  mkdir -p "$_boot"
  tar -xzf "$_work/$_tarball" -C "$_boot"
  echo "[boot-tree] extracted boot files:"; ls "$_boot" | head
}

# --- 2. Assemble the apkovl overlay (the device's identity) --------------------
# One source of truth for the device layout: boot scripts, the supervised OpenRC
# services, the aloop binary + home-FX LV2, the on-device test suite. Everything
# here lands read-only into the tmpfs root at boot, restored by Alpine's diskless
# lbu mechanism. Drops aloop.apkovl.tar.gz into $BOOT next to config.txt — the SD
# FAT and the TFTP root deliver it the same way (see docs/NETBOOT.md for how the
# diskless initramfs finds it over the network).
boot_tree_apkovl() {
  _work="$1"; _boot="$2"
  OVL="$_work/ovl"
  mkdir -p "$OVL/etc/local.d" "$OVL/etc/runlevels/boot" "$OVL/etc/runlevels/default" \
           "$OVL/etc/init.d" "$OVL/opt/aloop" "$OVL/effects/home" "$OVL/effects/user"

  # Boot-time scripts (run by the `local` service in order).
  cp "$ROOT/kernel/rt-tune.sh"        "$OVL/etc/local.d/10-rt-tune.start"
  cp "$ROOT/src/usb/f_uac2-gadget.sh" "$OVL/etc/local.d/20-usb-gadget.start"
  cp "$ROOT/src/net/autoap.sh"        "$OVL/opt/aloop/autoap.sh"
  cp -r "$ROOT/src/net/config"        "$OVL/etc/aloop-net"
  cp "$ROOT/config/aloop.conf"        "$OVL/etc/aloop.conf"
  cp "$ROOT/config/controls.conf"     "$OVL/etc/aloop-controls.conf"
  # The on-hardware test suite ships on the device so it can be run from the card
  # (docs/FLASHING.md step 6 / HARDWARE-TESTS.md).
  if [ -d "$ROOT/test/hardware" ]; then
    mkdir -p "$OVL/opt/aloop/test"
    cp -r "$ROOT/test/hardware" "$OVL/opt/aloop/test/hardware"
    chmod +x "$OVL/opt/aloop/test/hardware/"*.sh 2>/dev/null || true
  fi

  # The aloop binary + home-FX LV2 (the real device payload).
  if [ -n "${ALOOP_BIN:-}" ] && [ -f "${ALOOP_BIN}" ]; then
    cp "${ALOOP_BIN}" "$OVL/opt/aloop/aloop"; chmod +x "$OVL/opt/aloop/aloop"
    echo "[boot-tree] laid in aloop binary ($(du -h "${ALOOP_BIN}" | cut -f1))"
  else
    echo "[boot-tree] WARNING: no ALOOP_BIN — boot tree has no aloop binary"
  fi
  if [ -n "${LV2_DIR:-}" ] && [ -d "${LV2_DIR}" ]; then
    find "${LV2_DIR}" -maxdepth 2 -name '*.lv2' -exec cp -r {} "$OVL/effects/home/" \;
    echo "[boot-tree] laid in home-FX LV2: $(ls "$OVL/effects/home")"
  else
    echo "[boot-tree] WARNING: no LV2_DIR — boot tree has no home-FX effects bundle"
  fi

  # aloop as a SUPERVISED OpenRC service (respawn on crash + logging) — an appliance,
  # not a fire-and-forget script. autoap is its own service too.
  cat > "$OVL/etc/init.d/aloop" <<'SVC'
#!/sbin/openrc-run
name="aloop"
description="aloop RT audio looper + effects"
command="/opt/aloop/aloop"
command_args="--config /etc/aloop.conf"
command_background=true
pidfile="/run/aloop.pid"
output_log="/var/log/aloop.log"
error_log="/var/log/aloop.log"
respawn_delay=2
respawn_max=0
depend() { after local; need localmount; }
SVC
  cat > "$OVL/etc/init.d/autoap" <<'SVC'
#!/sbin/openrc-run
name="autoap"
description="aloop WiFi: join known net, else host an AP (for Ableton Link)"
command="/opt/aloop/autoap.sh"
command_background=true
pidfile="/run/autoap.pid"
respawn_delay=2
respawn_max=0
depend() { after local; }
SVC
  chmod +x "$OVL/etc/local.d/"*.start "$OVL/opt/aloop/"*.sh "$OVL/etc/init.d/aloop" "$OVL/etc/init.d/autoap"

  # Enable the services in the right runlevels (OpenRC = symlinks under runlevels/).
  # OpenRC only needs these symlinks to exist inside the tarball on the Linux
  # target; a build host that cannot create symlinks (e.g. Git-Bash on Windows for
  # a dry-run) falls back to a plain marker file so the layout is still assembled.
  # In CI (Linux) the real symlinks are created. Keep all three consistent.
  rl_enable() { # <target> <link>
    ln -sf "$1" "$2" 2>/dev/null || : > "$2"
  }
  rl_enable /etc/init.d/local  "$OVL/etc/runlevels/boot/local"
  rl_enable /etc/init.d/aloop  "$OVL/etc/runlevels/default/aloop"
  rl_enable /etc/init.d/autoap "$OVL/etc/runlevels/default/autoap"

  # hostname + the .apkovl name Alpine's diskless boot auto-restores.
  echo "aloop" > "$OVL/etc/hostname"

  # Package the overlay as Alpine's local-backup tarball. Diskless RPi restores the
  # file named <hostname>.apkovl.tar.gz found on the boot medium.
  APKOVL="aloop.apkovl.tar.gz"
  ( cd "$OVL" && tar -czf "$_work/$APKOVL" . )
  cp "$_work/$APKOVL" "$_boot/$APKOVL"
  echo "[boot-tree] apkovl -> $_boot/$APKOVL ($(du -h "$_work/$APKOVL" | cut -f1))"
}

# --- 3. Boot partition config (config.txt / cmdline.txt / usercfg.txt) ---------
# Merge our boot additions: dwc2 peripheral (f_uac2), serial console, and append
# the RT cmdline (isolcpus etc.). Alpine RPi reads usercfg.txt from config.txt.
# Identical for SD and netboot — the firmware reads these the same way whether the
# boot medium is a FAT partition or a TFTP directory.
boot_tree_config() {
  _boot="$1"
  cat "$ROOT/image/config/usercfg.txt" >> "$_boot/usercfg.txt"
  if [ -f "$_boot/config.txt" ] && ! grep -q 'include usercfg.txt' "$_boot/config.txt"; then
    echo "include usercfg.txt" >> "$_boot/config.txt"
  fi
  if [ -f "$_boot/cmdline.txt" ]; then
    printf ' %s' "$(cat "$ROOT/kernel/cmdline.txt")" >> "$_boot/cmdline.txt"
  else
    cat "$ROOT/kernel/cmdline.txt" > "$_boot/cmdline.txt"
  fi
  echo "[boot-tree] boot config merged (dwc2 + serial + isolcpus cmdline)"
}
