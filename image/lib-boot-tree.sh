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

  # SSH access — for remote debugging/updates without a serial console or a
  # power-cycle to inspect state (looper's dev-tooling equivalent: a way to
  # reach the running device directly, not just poll it over a bespoke UDP
  # protocol). `openssh-server` ships in the stock Alpine RPi tarball's own
  # apks/ repo (the SAME local set alpine_repo= already points at — no CDN
  # dependency), so Alpine's own diskless-boot mechanism installs it: the
  # initramfs/init reads etc/apk/world and runs `apk add` for anything listed
  # there against alpine_repo, exactly like the base packages already witnessed
  # installing (alpine-base, busybox, openssl, etc) — this is the FIRST real use
  # of that mechanism in this repo (the vendored libs above use direct file
  # copy instead, precisely because THEIR packages are NOT in the local repo).
  mkdir -p "$OVL/etc/apk"
  if ! grep -qx 'openssh-server' "$OVL/etc/apk/world" 2>/dev/null; then
    echo "openssh-server" >> "$OVL/etc/apk/world"
  fi
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
  ( cd "$OVL" && tar --mode='+x' -rf "$APKOVL_TAR" \
      ./opt/aloop/aloop ./opt/aloop/autoap.sh \
      ./etc/local.d/10-rt-tune.start ./etc/local.d/20-usb-gadget.start \
      ./etc/init.d/aloop ./etc/init.d/autoap \
      $(find opt/aloop/test -type f -name '*.sh' 2>/dev/null | sed 's|^|./|') )
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
  cat "$ROOT/image/config/usercfg.txt" >> "$_boot/usercfg.txt"
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
