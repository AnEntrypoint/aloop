# ci/ — build scripts invoked by GitHub Actions

Reproducible build steps (see `.github/workflows/`):
- build + validate the home-FX LV2 (`faust2lv2`),
- cross-compile the aloop binary for Pi 4 aarch64 under musl (Alpine container),
- assemble the Alpine SD image as a flashable artifact.

Everything the device runs is built here, so a fresh checkout reproduces the
image bit-for-bit.
