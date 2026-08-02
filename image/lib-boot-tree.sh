#!/bin/sh

ALPINE_VERSION="${ALPINE_VERSION:-3.20.3}"
ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"
ARCH="${ARCH:-aarch64}"
BOARD="${BOARD:-pi4}"

board_supports_usb_gadget() {
  case "$1" in
    pi4|pi-cm4|pi-zero2) return 0 ;;
    pi3|pi5) return 1 ;;
    *) return 1 ;;
  esac
}

board_wifi_irq_name() {
  case "$1" in
    pi3|pi4) echo "brcmfmac" ;;
    pi5) echo "brcmfmac" ;;
    *) echo "" ;;
  esac
}

boot_tree_fetch() {
  case "$BOARD" in
    opi-prime) boot_tree_fetch_opi "$1" "$2" ;;
    *)         boot_tree_fetch_rpi "$1" "$2" ;;
  esac
}

boot_tree_fetch_rpi() {
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

OPI_ARMBIAN_URL="${OPI_ARMBIAN_URL:-https://dl.armbian.com/orangepiprime/Trixie_current_minimal}"
boot_tree_fetch_opi() {
  _work="$1"; _boot="$2"
  _img_xz="$_work/armbian-opi-prime.img.xz"
  _img="$_work/armbian-opi-prime.img"
  if [ -n "${ARMBIAN_IMAGE:-}" ] && [ -f "${ARMBIAN_IMAGE}" ]; then
    echo "[boot-tree] using provided Armbian image ${ARMBIAN_IMAGE}"
    cp "${ARMBIAN_IMAGE}" "$_img_xz"
  else
    echo "[boot-tree] downloading $OPI_ARMBIAN_URL (stable redirect -- resolved asset version moves every Armbian trunk build)"
    curl -fsSL "$OPI_ARMBIAN_URL" -o "$_img_xz"
  fi
  echo "[boot-tree] decompressing Armbian image"
  xz -dk -f "$_img_xz" -c > "$_img"
  echo "[boot-tree] diagnostic: decompressed image size = $(wc -c < "$_img") bytes, xz source size = $(wc -c < "$_img_xz") bytes"
  echo "[boot-tree] diagnostic: MBR signature (should be 55aa) = $(dd if="$_img" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')"
  echo "[boot-tree] diagnostic: eGON.BT0 real byte offsets in the decompressed image = $(grep -abo 'eGON.BT0' "$_img" | head -5 | cut -d: -f1 | tr '\n' ',')"

  mkdir -p "$_boot/opi-boot" "$_boot/opi-uboot"

  _part1_start_sector=$(sfdisk -d "$_img" 2>/dev/null | awk '/img[0-9]* :/{print $4}' | head -n1 | tr -d ',')
  if [ -z "$_part1_start_sector" ]; then
    echo "[boot-tree] ERROR: could not read Armbian image's first partition start sector -- cannot locate the raw U-Boot region" >&2
    return 1
  fi
  _uboot_span_sectors=$((_part1_start_sector - 16))
  if [ "$_uboot_span_sectors" -le 0 ]; then
    echo "[boot-tree] ERROR: partition 1 starts at sector $_part1_start_sector, leaving no room after the sector-16 SPL offset for a U-Boot region" >&2
    return 1
  fi
  dd if="$_img" of="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" bs=512 skip=16 count="$_uboot_span_sectors" status=none
  _real_end_sector=$(cmp -l "$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" /dev/zero 2>/dev/null | tail -n1 | awk '{print int(($1-1)/512)+1}')
  [ -n "$_real_end_sector" ] || _real_end_sector="$_uboot_span_sectors"
  dd if="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" of="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin" bs=512 count="$_real_end_sector" status=none
  rm -f "$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw"
  echo "[boot-tree] extracted real U-Boot blob starting at source sector 16 ($_real_end_sector of $_uboot_span_sectors sectors were non-zero content)"

  _part_offset=$((_part1_start_sector * 512))
  _mnt="$_work/opi-root-mnt"
  mkdir -p "$_mnt"
  if ! command -v losetup >/dev/null 2>&1; then
    echo "[boot-tree] ERROR: losetup unavailable on this host -- Orange Pi Prime boot-tree extraction needs a real Linux loop-mount (works in CI; not on this dev host)" >&2
    return 1
  fi
  _loop=$(sudo losetup --show -fP -o "$_part_offset" "$_img")
  sudo mount -o ro "$_loop" "$_mnt"
  cp "$_mnt"/boot/vmlinuz-* "$_boot/opi-boot/" 2>/dev/null || cp "$_mnt"/boot/Image* "$_boot/opi-boot/" 2>/dev/null || true
  find "$_mnt/boot" -iname 'sun50i-h5-orangepi-prime.dtb' -exec cp {} "$_boot/opi-boot/" \; 2>/dev/null || true
  find "$_mnt/boot" -iname 'initrd.img-*' -exec cp {} "$_boot/opi-boot/" \; 2>/dev/null || true
  sudo umount "$_mnt"
  sudo losetup -d "$_loop"

  if [ -z "$(find "$_boot/opi-boot" -iname 'sun50i-h5-orangepi-prime.dtb' 2>/dev/null)" ]; then
    echo "[boot-tree] ERROR: sun50i-h5-orangepi-prime.dtb not found in the extracted Armbian /boot -- Armbian's dtb file naming may have changed, check $_mnt/boot manually" >&2
    return 1
  fi
  echo "[boot-tree] extracted Orange Pi Prime boot tree:"; ls "$_boot/opi-boot" "$_boot/opi-uboot"
}

boot_tree_apkovl() {
  _work="$1"; _boot="$2"
  OVL="$_work/ovl"
  mkdir -p "$OVL/etc/local.d" "$OVL/etc/runlevels/boot" "$OVL/etc/runlevels/default" \
           "$OVL/etc/init.d" "$OVL/opt/aloop" "$OVL/effects/home" "$OVL/effects/user"

  touch "$OVL/etc/.default_boot_services"

  cp "$ROOT/kernel/rt-tune.sh"        "$OVL/etc/local.d/10-rt-tune.start"
  cp "$ROOT/src/usb/f_uac2-gadget.sh" "$OVL/etc/local.d/20-usb-gadget.start"
  cp "$ROOT/src/net/autoap.sh"        "$OVL/opt/aloop/autoap.sh"
  cp -r "$ROOT/src/net/config"        "$OVL/etc/aloop-net"
  cp "$ROOT/config/aloop.conf"        "$OVL/etc/aloop.conf"
  cp "$ROOT/config/controls.conf"     "$OVL/etc/aloop-controls.conf"
  if [ -d "$ROOT/test/hardware" ]; then
    mkdir -p "$OVL/opt/aloop/test"
    cp -r "$ROOT/test/hardware" "$OVL/opt/aloop/test/hardware"
    chmod +x "$OVL/opt/aloop/test/hardware/"*.sh 2>/dev/null || true
  fi

  mkdir -p "$OVL/usr/lib"
  if [ -d "$ROOT/vendor/lib-aarch64" ]; then
    cp "$ROOT/vendor/lib-aarch64/"*.so* "$OVL/usr/lib/"
    echo "[boot-tree] laid in vendored runtime libs: $(ls "$OVL/usr/lib")"
  else
    echo "[boot-tree] WARNING: no vendor/lib-aarch64 — aloop will fail to start (missing libasound/liblilv)"
  fi

  mkdir -p "$OVL/usr/sbin"
  if [ -d "$ROOT/vendor/sbin-aarch64" ]; then
    cp "$ROOT/vendor/sbin-aarch64/"* "$OVL/usr/sbin/"
    chmod +x "$OVL/usr/sbin/"* 2>/dev/null || true
    echo "[boot-tree] laid in vendored mesh daemons: $(ls "$ROOT/vendor/sbin-aarch64" | tr '\n' ' ')"
  else
    echo "[boot-tree] WARNING: no vendor/sbin-aarch64 — autoap cannot host the ticker AP (missing hostapd/dnsmasq)"
  fi

  if [ -d "$ROOT/vendor/share-alsa" ]; then
    mkdir -p "$OVL/usr/share/alsa"
    cp -r "$ROOT/vendor/share-alsa/"* "$OVL/usr/share/alsa/"
    echo "[boot-tree] laid in vendored ALSA config data (usr/share/alsa/)"
  else
    echo "[boot-tree] WARNING: no vendor/share-alsa — aloop will SEGFAULT opening the default PCM (alsa-lib needs alsa.conf to resolve device names)"
  fi

  if [ -n "${ALOOP_BIN:-}" ] && [ -f "${ALOOP_BIN}" ]; then
    cp "${ALOOP_BIN}" "$OVL/opt/aloop/aloop"; chmod +x "$OVL/opt/aloop/aloop"
    echo "[boot-tree] laid in aloop binary ($(du -h "${ALOOP_BIN}" | cut -f1))"
  else
    echo "[boot-tree] WARNING: no ALOOP_BIN — boot tree has no aloop binary"
  fi
  if [ -n "${LV2_DIR:-}" ] && [ -d "${LV2_DIR}" ]; then
    find "${LV2_DIR}" -maxdepth 2 -name '*.lv2' ! -name 'aloop.lv2' -exec cp -r {} "$OVL/effects/home/" \;
    echo "[boot-tree] laid in home-FX LV2: $(ls "$OVL/effects/home")"
  else
    echo "[boot-tree] WARNING: no LV2_DIR — boot tree has no home-FX effects bundle"
  fi

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
rc_ulimit="-l unlimited -r 95"
depend() { after local autoap; need localmount; }
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

  rl_enable() {
    ln -sf "$1" "$2" 2>/dev/null || : > "$2"
  }
  rl_enable /etc/init.d/local  "$OVL/etc/runlevels/boot/local"
  rl_enable /etc/init.d/aloop  "$OVL/etc/runlevels/default/aloop"
  rl_enable /etc/init.d/autoap "$OVL/etc/runlevels/default/autoap"

  mkdir -p "$OVL/etc/apk"
  for _pkg in openssh-server wpa_supplicant iw; do
    if ! grep -qx "$_pkg" "$OVL/etc/apk/world" 2>/dev/null; then
      echo "$_pkg" >> "$OVL/etc/apk/world"
    fi
  done
  rl_enable /etc/init.d/sshd "$OVL/etc/runlevels/default/sshd"
  mkdir -p "$OVL/etc/ssh/sshd_config.d"
  cat > "$OVL/etc/ssh/sshd_config.d/aloop-debug.conf" <<'SSHCFG'
PermitRootLogin yes
PasswordAuthentication yes
SSHCFG
  ROOT_HASH='$6$aloopsalt$mLQd3y9csZMjCwucD8/e/WZn/HO/yj5.wWpZJqqKaURUBfeasNgYjt72eegiWQxLmoYOto41DXBCKiUzhbnLF0'
  mkdir -p "$OVL/etc"
  if [ -f "$OVL/etc/shadow" ]; then
    sed -i "s|^root:[^:]*:|root:${ROOT_HASH}:|" "$OVL/etc/shadow"
  else
    printf 'root:%s:19000:0:::::\n' "$ROOT_HASH" > "$OVL/etc/shadow"
  fi
  chmod 640 "$OVL/etc/shadow"
  echo "[boot-tree] SSH enabled: openssh-server via apk world + sshd service + root password 'aloop' set"

  echo "aloop" > "$OVL/etc/hostname"

  APKOVL="aloop.apkovl.tar.gz"
  APKOVL_TAR="$_work/aloop.apkovl.tar"
  ( cd "$OVL" && tar -cf "$APKOVL_TAR" . )
  _exec_paths="./opt/aloop/autoap.sh \
      ./etc/local.d/10-rt-tune.start ./etc/local.d/20-usb-gadget.start \
      ./etc/init.d/aloop ./etc/init.d/autoap \
      $(find usr/sbin -type f 2>/dev/null | sed 's|^|./|') \
      $(find opt/aloop/test -type f -name '*.sh' 2>/dev/null | sed 's|^|./|')"
  if [ -f "$OVL/opt/aloop/aloop" ]; then
    _exec_paths="./opt/aloop/aloop $_exec_paths"
  fi
  ( cd "$OVL" && tar --mode='+x' -rf "$APKOVL_TAR" $_exec_paths )
  gzip -f "$APKOVL_TAR"
  cp "$_work/$APKOVL" "$_boot/$APKOVL"
  APKOVL_LASTMODE=$(tar -tzvf "$_work/$APKOVL" 2>/dev/null | grep 'opt/aloop/aloop$' | tail -1 | cut -c1-10)
  if [ "$APKOVL_LASTMODE" = "-rwxr-xr-x" ]; then
    echo "[boot-tree] apkovl -> $_boot/$APKOVL ($(du -h "$_work/$APKOVL" | cut -f1)) [aloop binary confirmed +x in archive]"
  elif [ -z "$APKOVL_LASTMODE" ] && [ ! -f "$OVL/opt/aloop/aloop" ]; then
    echo "[boot-tree] apkovl -> $_boot/$APKOVL ($(du -h "$_work/$APKOVL" | cut -f1)) [no ALOOP_BIN this run, nothing to verify]"
  else
    echo "[boot-tree] ERROR: aloop binary is NOT executable in the built apkovl (last entry mode: $APKOVL_LASTMODE) — aloop service will crash-loop with 'Permission denied'"
  fi
}

boot_tree_config() {
  _boot="$1"
  if [ "$BOARD" = "opi-prime" ]; then
    boot_tree_config_opi "$_boot"
    return
  fi
  if board_supports_usb_gadget "$BOARD"; then
    cat "$ROOT/image/config/usercfg.txt" >> "$_boot/usercfg.txt"
  else
    grep -v 'dtoverlay=dwc2' "$ROOT/image/config/usercfg.txt" >> "$_boot/usercfg.txt"
    echo "[boot-tree] BOARD=$BOARD has no USB-OTG peripheral controller -- dwc2/f_uac2 gadget overlay omitted"
  fi
  if [ -f "$_boot/config.txt" ] && ! grep -q 'include usercfg.txt' "$_boot/config.txt"; then
    echo "include usercfg.txt" >> "$_boot/config.txt"
  fi
  _existing=""
  [ -f "$_boot/cmdline.txt" ] && _existing="$(tr '\n' ' ' < "$_boot/cmdline.txt")"
  _rt="$(tr '\n' ' ' < "$ROOT/kernel/cmdline.txt")"
  printf '%s\n' "$(printf '%s %s' "$_existing" "$_rt" | tr -s ' ' | sed 's/^ //;s/ $//')" \
    > "$_boot/cmdline.txt"
  echo "[boot-tree] boot config merged, cmdline.txt collapsed to a single line (dwc2 + serial + isolcpus)"
}

boot_tree_config_opi() {
  _boot="$1"
  _rt="$(tr '\n' ' ' < "$ROOT/kernel/cmdline.txt" | tr -s ' ' | sed 's/^ //;s/ $//')"
  _kernel=$(find "$_boot/opi-boot" -iname 'vmlinuz-*' -o -iname 'Image*' 2>/dev/null | head -n1)
  _dtb=$(find "$_boot/opi-boot" -iname 'sun50i-h5-orangepi-prime.dtb' 2>/dev/null | head -n1)
  _initrd=$(find "$_boot/opi-boot" -iname 'initrd.img-*' 2>/dev/null | head -n1)
  [ -n "$_kernel" ] || { echo "[boot-tree] ERROR: no kernel image found under $_boot/opi-boot" >&2; return 1; }
  [ -n "$_dtb" ]    || { echo "[boot-tree] ERROR: no sun50i-h5-orangepi-prime.dtb found under $_boot/opi-boot" >&2; return 1; }
  mkdir -p "$_boot/opi-boot/extlinux"
  {
    echo "LABEL aloop"
    echo "  KERNEL /boot/$(basename "$_kernel")"
    echo "  FDT /boot/$(basename "$_dtb")"
    [ -n "$_initrd" ] && echo "  INITRD /boot/$(basename "$_initrd")"
    echo "  APPEND root=LABEL=aloopboot rw console=ttyS0,115200 $_rt"
  } > "$_boot/opi-boot/extlinux/extlinux.conf"
  echo "[boot-tree] wrote extlinux.conf (kernel=$(basename "$_kernel") dtb=$(basename "$_dtb") isolcpus tuning included)"
}
