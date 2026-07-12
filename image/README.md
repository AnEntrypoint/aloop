# image/ — the Alpine device image

Builds the flashable Raspberry Pi 4 SD image: Alpine Linux booting diskless from
RAM (read-only root, near-bare-metal determinism), with the aloop binary, the
audio/WiFi services, the f_uac2 USB gadget setup, and the RT tuning applied at
boot. Assembled reproducibly in CI (see `ci/` and `.github/workflows/`).

See `docs/BOOT.md` for the runtime startup sequence.
