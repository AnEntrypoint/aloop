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
BOARD="${BOARD:-pi4}"

# --- Board capability matrix ----------------------------------------------------
# USB-audio-gadget (f_uac2) needs a USB-OTG controller running in PERIPHERAL mode.
# Pi 4/CM4/Zero2 route their USB-C/OTG port to dwc2 and support dr_mode=peripheral.
# Pi 3 has no OTG-capable controller (its USB is host-only via an onboard hub) and
# Pi 5's RP1 southbridge USB is host-only — neither can run f_uac2 gadget mode, so
# the dwc2 overlay must never be written into their boot config (a dtoverlay for a
# controller the board doesn't have is silently ignored by the Pi firmware, but
# shipping it as if it worked would be a real, wrong claim about device behavior).
board_supports_usb_gadget() {
  case "$1" in
    pi4|pi-cm4|pi-zero2) return 0 ;;
    pi3|pi5) return 1 ;;
    *) return 1 ;;
  esac
}

# WiFi IRQ name to steer off the audio cores (kernel/rt-tune.sh greps this by name).
# Empty means "no board-specific WiFi chip to steer" (board has none, or it's a
# different chip rt-tune.sh's generic wlan/eth pattern already covers).
board_wifi_irq_name() {
  case "$1" in
    pi3|pi4) echo "brcmfmac" ;;
    pi5) echo "brcmfmac" ;;
    *) echo "" ;;
  esac
}

# --- 1. Fetch + extract the board's firmware/kernel/DTB boot tree ---------------
# Dispatches per BOARD: the Pi family shares one Alpine-published tarball (identical
# fetch, identical tree shape) since Alpine bundles every Pi model's firmware+DTBs
# together; Orange Pi Prime has no such tarball (see boot_tree_fetch_opi) and needs
# its own fetch shape entirely.
boot_tree_fetch() {
  case "$BOARD" in
    opi-prime) boot_tree_fetch_opi "$1" "$2" ;;
    *)         boot_tree_fetch_rpi "$1" "$2" ;;
  esac
}

# The tarball's contents ARE the boot tree: start4.elf/fixup4.dat/bootcode.bin
# (Pi 4 firmware chain — the SAME files the TFTP bootloader fetches), the bcm2711
# DTBs, boot/vmlinuz-rpi + boot/initramfs-rpi + boot/modloop-rpi, config.txt,
# cmdline.txt, overlays/, and apks/. Extracting it is identical for SD and netboot,
# and identical for pi3/pi4/pi5 -- one tarball bundles every Pi model's DTB, and the
# real Pi firmware auto-selects the correct one for its own hardware ID at boot.
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

# --- 1b. Orange Pi Prime (Allwinner H5, sun50i-h5-orangepi-prime) boot tree -----
# There is no Alpine-published sunxi tarball (unlike alpine-rpi-*.tar.gz), and
# Allwinner's boot chain is structurally different from the Pi's FAT-partition
# firmware model: BootROM reads a raw SPL/U-Boot image at a FIXED RAW SD SECTOR
# OFFSET (dd seek=8, 1K blocks), before any filesystem exists -- there is no
# config.txt-style firmware file to drop into a boot tree. Real, verified source
# (researched this session, not guessed): Armbian's official Orange Pi Prime
# "Trixie minimal" build is the most turnkey donor for a working U-Boot + kernel +
# sun50i-h5-orangepi-prime.dtb triple. dl.armbian.com/orangepiprime/Trixie_current
# _minimal is a STABLE redirect URL Armbian maintains across their rolling trunk
# builds -- the resolved github.com/armbian/community/releases/.../*.img.xz asset
# URL it 302s to is NOT stable (its version string moves every trunk build), so
# only the dl.armbian.com redirect path is fetched here, never a pinned asset URL.
# Extracted pieces: the raw image's first-8KiB-to-first-partition span (U-Boot,
# already correctly positioned by Armbian's own partition layout) is copied
# byte-for-byte onto the target media at the same offset; /boot inside the root
# partition (ext4) holds the kernel + dtb, mounted read-only via a loop device to
# pull just those two files out. The Alpine diskless philosophy is kept for the
# USERSPACE (see boot_tree_apkovl, unchanged/shared) -- only the boot chain itself
# is Armbian-sourced, since Alpine has no sunxi boot tarball to offer instead
# (opi-alpine-image-source-decision).
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

  # U-Boot: raw sectors before the first partition. Read the first partition's
  # start offset from the image's own partition table (never assume a fixed
  # value -- Armbian's layout is authoritative, not a guess) and copy everything
  # from sector 0 up to that offset as one raw U-Boot blob.
  _part1_start_sector=$(sfdisk -d "$_img" 2>/dev/null | awk '/img[0-9]* :/{print $4}' | head -n1 | tr -d ',')
  if [ -z "$_part1_start_sector" ]; then
    echo "[boot-tree] ERROR: could not read Armbian image's first partition start sector -- cannot locate the raw U-Boot region" >&2
    return 1
  fi
  echo "[boot-tree] diagnostic: sfdisk -d full output ='$(sfdisk -d "$_img" 2>/dev/null)'"
  echo "[boot-tree] diagnostic: _part1_start_sector value = '[${_part1_start_sector}]' (length $(printf '%s' "$_part1_start_sector" | wc -c))"
  # The pre-partition-1 span on Armbian's own image ($_part1_start_sector sectors,
  # often 4MiB+) is mostly empty padding reserved for a later U-Boot environment,
  # NOT the real SPL+u-boot.bin size (which is typically a few hundred KB). Reading
  # the whole span as "the U-Boot blob" and writing it at our own image's sector-8
  # offset would extend past our OWN partition start (sector 2048 = byte 1048576)
  # and get overwritten by the later ext4 partition splice, corrupting/dropping the
  # real SPL. Trim to the last non-zero 512-byte sector within the span instead, so
  # the copied blob is only as large as its actual real content.
  dd if="$_img" of="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" bs=512 count="$_part1_start_sector" status=none
  echo "[boot-tree] diagnostic: extracted .raw file size = $(wc -c < "$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw") bytes (expect $((_part1_start_sector * 512)))"
  echo "[boot-tree] diagnostic: raw pre-trim bytes at offset 4-12 = '$(dd if="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" bs=1 skip=4 count=8 2>/dev/null)'"
  echo "[boot-tree] diagnostic: eGON offsets found inside the extracted .raw file = $(grep -abo 'eGON.BT0' "$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" | head -3 | cut -d: -f1 | tr '\n' ',')"
  _real_end_sector=$(cmp -l "$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" /dev/zero 2>/dev/null | tail -n1 | awk '{print int(($1-1)/512)+1}')
  [ -n "$_real_end_sector" ] || _real_end_sector="$_part1_start_sector"
  dd if="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw" of="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin" bs=512 count="$_real_end_sector" status=none
  rm -f "$_boot/opi-uboot/u-boot-sunxi-with-spl.bin.raw"
  echo "[boot-tree] diagnostic: post-trim bytes at offset 4-12 = '$(dd if="$_boot/opi-uboot/u-boot-sunxi-with-spl.bin" bs=1 skip=4 count=8 2>/dev/null)'"
  echo "[boot-tree] extracted real U-Boot blob ($_real_end_sector of $_part1_start_sector sectors were non-zero content)"

  # Kernel + DTB: inside the first (and on a minimal Armbian image, only) real
  # partition, an ext4 filesystem with /boot at its root (no separate /boot
  # partition on Armbian's minimal layout). mtools cannot read ext4, so this
  # needs a real loop mount -- CI (Linux) has one; a non-Linux dev host does not,
  # and is expected to fail loudly here rather than silently ship an empty tree.
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

  # WITNESSED live on a real Pi 4 (via SSH into a booted device — see the SSH
  # section below): /lib/modules was completely EMPTY, /proc/asound did not
  # exist, and /sys/kernel/config/usb_gadget/ could not be created — the whole
  # kernel-modules/configfs/ALSA-subsystem layer never came up, even though
  # modloop-rpi WAS fetched over HTTP during boot (confirmed 200 in the serve
  # log). Root cause, found by extracting the Alpine RPi initramfs's own /init
  # script: `rc_add modloop sysinit` (which also enables devfs/dmesg/mdev/
  # hwdrivers — the whole hardware-bring-up layer) is gated behind
  # `[ -f "$sysroot/etc/.default_boot_services" -o ! -f "$ovl" ]` — i.e. it ONLY
  # runs for a fresh/no-apkovl boot, UNLESS the apkovl itself carries a
  # `.default_boot_services` marker file asking init to still enable them (a
  # documented, one-shot Alpine mechanism — init removes the marker after
  # reading it). Shipping our own apkovl with runlevels already populated
  # silently opted OUT of ALL of these services, with no error anywhere.
  touch "$OVL/etc/.default_boot_services"

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

  # The aloop binary's runtime shared-library dependencies (alsa-lib + the lilv
  # stack: lilv-libs/serd-libs/sord-libs/sratom/zix-libs — pkg_check_modules(ALSA
  # alsa)/pkg_check_modules(LILV lilv-0) in src/CMakeLists.txt). WITNESSED live on
  # a real Pi 4: the device's only reachable apk repo is the ~100-package minimal
  # set bundled in the Alpine RPi tarball (image/build-netboot.sh's alpine_repo=
  # points at that local tree, no CDN fallback) — none of these six packages are
  # in it, so `apk add` at boot could never install them and the aloop binary
  # would fail to dynamically link (confirmed: telemetry never came up after a
  # full successful boot). Bundling the real musl-aarch64 .so files directly
  # (vendor/lib-aarch64/, fetched from the exact Alpine 3.20 CDN versions CI
  # builds against) sidesteps the repo-availability question entirely — same
  # pattern as the aloop binary/LV2 bundle below, just files copied straight in.
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

  # alsa-lib's OWN config data (not just its .so): WITNESSED live on a real Pi 4
  # — with libasound.so.2 vendored but NO /usr/share/alsa/alsa.conf, calling
  # snd_pcm_open("default", ...) segfaults deep inside alsa-lib's config parser
  # (the "default" PCM name is an ALIAS defined in alsa.conf; without it there
  # is nothing to resolve "default" against). ALSA lib's own stderr confirms
  # this exactly: "Cannot access file /usr/share/alsa/alsa.conf". Vendor the
  # whole data tree (~340K, small enough not to prune) rather than guess which
  # of alsa.conf's @hooks/includes are load-bearing.
  if [ -d "$ROOT/vendor/share-alsa" ]; then
    mkdir -p "$OVL/usr/share/alsa"
    cp -r "$ROOT/vendor/share-alsa/"* "$OVL/usr/share/alsa/"
    echo "[boot-tree] laid in vendored ALSA config data (usr/share/alsa/)"
  else
    echo "[boot-tree] WARNING: no vendor/share-alsa — aloop will SEGFAULT opening the default PCM (alsa-lib needs alsa.conf to resolve device names)"
  fi

  # The aloop binary + home-FX LV2 (the real device payload).
  if [ -n "${ALOOP_BIN:-}" ] && [ -f "${ALOOP_BIN}" ]; then
    cp "${ALOOP_BIN}" "$OVL/opt/aloop/aloop"; chmod +x "$OVL/opt/aloop/aloop"
    echo "[boot-tree] laid in aloop binary ($(du -h "${ALOOP_BIN}" | cut -f1))"
  else
    echo "[boot-tree] WARNING: no ALOOP_BIN — boot tree has no aloop binary"
  fi
  if [ -n "${LV2_DIR:-}" ] && [ -d "${LV2_DIR}" ]; then
    # EXCLUDE aloop.lv2: build-lv2.yml's home-fx-lv2 job compiles dsp/aloop.dsp
    # (the SAME source audio_thread.cpp's faustHome already compiles NATIVELY
    # and runs every block) into a standalone LV2 bundle purely as a CI
    # reproducibility/packaging check (ADR-003) -- it was never meant to be
    # loaded as a SECOND, real-time-competing copy of the home loop engine
    # alongside the native one. WITNESSED live on a real Pi 4: once a separate
    # lilv bundle-path-matching bug (that had silently made every LV2 plugin
    # crash+get-disabled on its first block, every boot) was fixed, aloop.lv2
    # started actually RUNNING for the first time -- and running the entire
    # home Faust stack a second time, in full, every block, on top of the
    # native compute() already running it, doubled real per-block DSP cost
    # (core_busy jumped from ~22% to ~63%, xruns climbing continuously).
    # guitar_lofi_fx.lv2 (a genuinely standalone, additive effect with no
    # dependency on aloop.dsp/loop.dsp) is unaffected and still copied in.
    find "${LV2_DIR}" -maxdepth 2 -name '*.lv2' ! -name 'aloop.lv2' -exec cp -r {} "$OVL/effects/home/" \;
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
# WITNESSED live on a real Pi 4: kernel/rt-tune.sh sets `ulimit -l unlimited`
# (memlock, needed for mlockall(MCL_FUTURE)) but that runs inside a
# `local.d/*.start` script, which OpenRC's `local` service `eval`s in a
# transient subshell that exits once local's start() returns — the ulimit
# change NEVER reaches aloop, a separate process started later by a separate
# service (confirmed: `ulimit -l` on the booted device showed the 8192 KB
# default, not unlimited). rc_ulimit is OpenRC's own per-service ulimit
# mechanism (read by openrc-run.sh itself before exec'ing command) — the
# correct place for a limit the SERVICE'S OWN process needs. This is a real,
# independent correctness fix for mlockall regardless of a separate,
# still-unresolved SIGSEGV investigated the same session (see PRD row
# reopen-audio-thread-segfault-investigation) — manually setting `ulimit -l
# unlimited` before running aloop interactively did NOT fix that crash, so
# this fix is not claimed to resolve it, only to make memlock actually
# unlimited for the service as rt-tune.sh always intended.
rc_ulimit="-l unlimited -r 95"
# `after autoap`: aloop constructs ableton::Link (and with it Link's UDP
# multicast socket) during startup. Both services previously declared only
# `after local` and neither referenced the other, so OpenRC was free to start
# them in either order or in parallel -- meaning Link could open its socket
# before autoap had brought wlan0 up or given it an address. ../esp-idf-link
# treats this exact race as real and defends against it twice (a 500ms
# interface settle before constructing Link in main.cpp, plus re-asserting
# IGMP membership for ~10s after every connection in wifi_config.cpp, whose
# own comment says a single join at GOT_IP can race netif readiness so the
# membership does not stick). Ordering aloop after autoap is the cheap half of
# the same defence; src/main.cpp additionally waits for the interface to carry
# an address before starting Link.
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

  mkdir -p "$OVL/etc/apk"
  for _pkg in openssh-server wpa_supplicant iw; do
    if ! grep -qx "$_pkg" "$OVL/etc/apk/world" 2>/dev/null; then
      echo "$_pkg" >> "$OVL/etc/apk/world"
    fi
  done
  rl_enable /etc/init.d/sshd "$OVL/etc/runlevels/default/sshd"
  # PermitRootLogin + password auth: a DEV/DEBUG convenience for a closed dev
  # LAN (docs/NETBOOT.md's WSL/ICS setup) — never how an internet-reachable
  # device should be configured. Root password is `aloop` (SHA-512 crypt via
  # `openssl passwd -6`, verified reproducible with this exact salt/hash pair
  # before landing here — never hand-write a hash without checking it).
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

  # hostname + the .apkovl name Alpine's diskless boot auto-restores.
  echo "aloop" > "$OVL/etc/hostname"

  # Package the overlay as Alpine's local-backup tarball. Diskless RPi restores the
  # file named <hostname>.apkovl.tar.gz found on the boot medium.
  #
  # WHY two tar passes instead of one: WITNESSED live on a real Pi 4 booted from
  # a netboot root built on this Windows/Git-Bash host — `chmod +x` on an NTFS
  # filesystem (MSYS2/Git-Bash has no real POSIX exec bit to set) is a SILENT
  # NO-OP. Every earlier `chmod +x` call in this function ran without error but
  # never actually set the bit, so the apkovl shipped /opt/aloop/aloop as
  # `-rw-r--r--`, and OpenRC's `start-stop-daemon` failed with "Permission
  # denied" — the aloop service crash-looped forever with NO other symptom
  # (network/boot/apk-install all succeeded; only exec permission was wrong).
  # `tar --mode=CHANGES` FORCES the mode on entries in that specific tar
  # invocation regardless of what chmod actually did to the file on disk, so
  # appending the executable-needing paths in a second pass with --mode='+x'
  # guarantees the bit lands correctly in the archive on ANY host (Linux CI
  # included — --mode is a no-op there since chmod already worked, so this is
  # safe everywhere, not just a Windows workaround).
  APKOVL="aloop.apkovl.tar.gz"
  APKOVL_TAR="$_work/aloop.apkovl.tar"
  ( cd "$OVL" && tar -cf "$APKOVL_TAR" . )
  # The append pass's paths MUST match the first pass's entry names exactly
  # (both written with `./` since the first pass tars `.`) — GNU tar's -r does
  # NOT overwrite an existing entry by a differently-spelled equivalent path,
  # it APPENDS a second entry, and extraction order then decides which wins
  # (fragile, host-dependent). Prefixing `./` here is what makes -r correctly
  # update the same entry instead of duplicating it.
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
  # $APKOVL_TAR.gz IS $_work/$APKOVL already (APKOVL_TAR = $_work/aloop.apkovl.tar,
  # APKOVL = aloop.apkovl.tar.gz) — no mv needed; a self-mv errors "same file" on
  # real Linux (WITNESSED: this exact line failed CI after landing, having
  # silently no-op'd on this dev host's Git-Bash mv instead of erroring there).
  cp "$_work/$APKOVL" "$_boot/$APKOVL"
  # Verify the fix actually landed — never trust chmod silently; check the archive.
  # (Use `tar -tzv --occurrence=-1` semantics implicitly: grep the LAST match,
  # since a genuinely duplicated entry would otherwise let an earlier -rw-r--r--
  # line pass this check even though extraction might pick the wrong one.)
  APKOVL_LASTMODE=$(tar -tzvf "$_work/$APKOVL" 2>/dev/null | grep 'opt/aloop/aloop$' | tail -1 | cut -c1-10)
  if [ "$APKOVL_LASTMODE" = "-rwxr-xr-x" ]; then
    echo "[boot-tree] apkovl -> $_boot/$APKOVL ($(du -h "$_work/$APKOVL" | cut -f1)) [aloop binary confirmed +x in archive]"
  elif [ -z "$APKOVL_LASTMODE" ] && [ ! -f "$OVL/opt/aloop/aloop" ]; then
    echo "[boot-tree] apkovl -> $_boot/$APKOVL ($(du -h "$_work/$APKOVL" | cut -f1)) [no ALOOP_BIN this run, nothing to verify]"
  else
    echo "[boot-tree] ERROR: aloop binary is NOT executable in the built apkovl (last entry mode: $APKOVL_LASTMODE) — aloop service will crash-loop with 'Permission denied'"
  fi
}

# --- 3. Boot partition config (config.txt / cmdline.txt / usercfg.txt) ---------
# Merge our boot additions: dwc2 peripheral (f_uac2), serial console, and append
# the RT cmdline (isolcpus etc.). Alpine RPi reads usercfg.txt from config.txt.
# Identical for SD and netboot — the firmware reads these the same way whether the
# boot medium is a FAT partition or a TFTP directory.
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
  # cmdline.txt MUST be a SINGLE line: the Pi firmware reads only the first line as
  # the kernel command line, so ANY embedded newline silently truncates every param
  # after it. The stock Alpine cmdline.txt ends with a trailing '\n', and the RT
  # fragment (kernel/cmdline.txt) may too — appending raw left an embedded newline
  # between them, dropping isolcpus + (for netboot) ip=dhcp/alpine_repo/modloop/apkovl
  # entirely (WITNESSED: Pi never ran the initramfs DHCP -> dropped to emergency
  # shell). Strip ALL newlines from both parts and re-emit one line + one trailing \n.
  _existing=""
  [ -f "$_boot/cmdline.txt" ] && _existing="$(tr '\n' ' ' < "$_boot/cmdline.txt")"
  _rt="$(tr '\n' ' ' < "$ROOT/kernel/cmdline.txt")"
  # Collapse runs of whitespace, trim, join with a single space, one trailing newline.
  printf '%s\n' "$(printf '%s %s' "$_existing" "$_rt" | tr -s ' ' | sed 's/^ //;s/ $//')" \
    > "$_boot/cmdline.txt"
  echo "[boot-tree] boot config merged, cmdline.txt collapsed to a single line (dwc2 + serial + isolcpus)"
}

# --- 3b. Orange Pi Prime boot config: U-Boot extlinux, not config.txt/cmdline.txt -
# U-Boot's generic distro-boot mechanism reads /boot/extlinux/extlinux.conf (the
# same file syslinux/grub2-style bootloaders use) to find the kernel/dtb/initrd and
# the kernel command line -- there is no config.txt/cmdline.txt on this boot chain
# at all (those are Pi-firmware-specific files, meaningless to U-Boot). isolcpus/RT
# tuning still applies (a real kernel parameter, board-independent) but travels via
# extlinux.conf's APPEND line instead.
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
