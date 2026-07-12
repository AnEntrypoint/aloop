#!/bin/sh
# aloop Alpine SD image builder (ADR-001; docs/ARCHITECTURE.md "boot to appliance").
#
# WHY diskless/RAM: a read-only root booting into RAM means no disk writes, no
# background daemons, and near-bare-metal determinism — the reason to pick Alpine
# over a full distro. Config persists via an apkovl overlay, not a writable root.
#
# This produces a flashable Pi4 SD image. Run in CI (build-image.yml) or locally
# on an Alpine host. Requires: alpine-make-rpi / genimage, the built aloop binary,
# and the home-FX LV2 bundle.

set -eu
OUT="${OUT:-aloop-pi4.img}"
ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"
ARCH="aarch64"

echo "[image] building aloop Alpine $ALPINE_BRANCH $ARCH image -> $OUT"

# 1. Base: Alpine RPi tarball (kernel + firmware), extracted to a boot + root fs.
# 2. Overlay (apkovl): everything in ./overlay/ lands read-only into the root:
#      /etc/local.d/10-rt-tune.start      <- kernel/rt-tune.sh (RT tuning at boot)
#      /etc/local.d/20-usb-gadget.start   <- src/usb/f_uac2-gadget.sh
#      /etc/local.d/30-aloop.start        <- start the aloop process + autoap
#      /etc/aloop-net/                    <- hostapd/dnsmasq/wpa_supplicant configs
#      /etc/aloop.conf                    <- the tunables
#      /opt/aloop/aloop                   <- the binary
#      /effects/home, /effects/user       <- the LV2 bundles
# 3. Boot config: append kernel/cmdline.txt + image/config/usercfg.txt (dwc2).
# 4. Enable OpenRC services: local (runs /etc/local.d), the aloop service.
#
# The concrete tool (alpine-make-rpi-image or a genimage recipe) is wired in the
# CI job; this script documents the exact overlay layout the image must have so
# the device boots straight into aloop.

mkdir -p overlay/etc/local.d overlay/opt/aloop overlay/effects/home overlay/effects/user
cp ../kernel/rt-tune.sh           overlay/etc/local.d/10-rt-tune.start
cp ../src/usb/f_uac2-gadget.sh    overlay/etc/local.d/20-usb-gadget.start
cp ../src/net/autoap.sh           overlay/opt/aloop/autoap.sh
cp -r ../src/net/config           overlay/etc/aloop-net
cp ../config/aloop.conf           overlay/etc/aloop.conf
chmod +x overlay/etc/local.d/*.start overlay/opt/aloop/*.sh

cat > overlay/etc/local.d/30-aloop.start <<'START'
#!/bin/sh
/opt/aloop/aloop --config /etc/aloop.conf &
/opt/aloop/autoap.sh &
START
chmod +x overlay/etc/local.d/30-aloop.start

echo "[image] overlay ready. Layout:"
find overlay -maxdepth 3 -type f | sort
echo "[image] (CI wires the base-image + genimage step to emit $OUT)"
