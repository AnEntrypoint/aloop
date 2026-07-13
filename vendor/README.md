# vendor/lib-aarch64 — bundled runtime shared libraries

These are the exact `.so` files aloop needs at runtime that the device's own
apk repo cannot provide (see `image/lib-boot-tree.sh`'s `boot_tree_apkovl` for
why: the netboot/SD `alpine_repo=` points at the ~100-package minimal set
bundled in the stock Alpine RPi tarball, with no CDN fallback — none of
`alsa-lib`, the `lilv` stack, `libstdc++`, or `libgcc` are in it, so `apk add`
at boot can never install them).

WITNESSED (two separate real failures, found via SSH into a live-booted Pi 4
after wiring in remote debug access — see `image/lib-boot-tree.sh`'s SSH
section): (1) without `alsa-lib`/`lilv`+deps, telemetry never answers after an
otherwise fully successful boot; (2) once those were vendored, `aloop` still
crash-looped with `Error loading shared library libstdc++.so.6`/`libgcc_s.so.1`
— aloop is C++, and the musl/Alpine minimal base doesn't ship the GCC C++
runtime either. Both classes of missing dependency are now vendored here.

## Provenance

Fetched from the official Alpine 3.20 CDN (`dl-cdn.alpinelinux.org`), the same
Alpine version the `alpine:3.20` aarch64 container in
`.github/workflows/build-binary.yml` builds `aloop` against, so the ABI
matches exactly:

| File | Source package | Version |
|---|---|---|
| `libasound.so.2` | `alsa-lib` | 1.2.11-r0 (main) |
| `liblilv-0.so.0` | `lilv-libs` | 0.24.24-r1 (community) |
| `libserd-0.so.0` | `serd-libs` | 0.32.2-r0 (community) |
| `libsord-0.so.0` | `sord-libs` | 0.16.16-r0 (community) |
| `libsratom-0.so.0` | `sratom` | 0.6.16-r0 (community) |
| `libzix-0.so.0` | `zix-libs` | 0.4.2-r0 (community) |
| `libstdc++.so.6` | `libstdc++` | 13.2.1_git20240309-r1 (main) |
| `libgcc_s.so.1` | `libgcc` | 13.2.1_git20240309-r1 (main) |

The lilv stack is the minimal true runtime closure for `liblilv-0.so.0` (traced
via each package's `APKINDEX` `D:` dependency line) — deliberately NOT the
full `lilv`/`lv2` CLI-tools packages, which pull in GTK/Cairo/GLib for tools
aloop never uses. `libstdc++`/`libgcc` are self-contained (only depend on
`libc.musl` and each other per their own `D:` lines).

## Re-vendoring (new Alpine version, or a package update)

```sh
V=3.20   # match ALPINE_VERSION in image/lib-boot-tree.sh
for spec in "main:alsa-lib:1.2.11-r0" \
            "community:lilv-libs:0.24.24-r1" \
            "community:serd-libs:0.32.2-r0" \
            "community:sord-libs:0.16.16-r0" \
            "community:sratom:0.6.16-r0" \
            "community:zix-libs:0.4.2-r0" \
            "main:libstdc++:13.2.1_git20240309-r1" \
            "main:libgcc:13.2.1_git20240309-r1"; do
  repo="${spec%%:*}"; rest="${spec#*:}"; pkg="${rest%%:*}"; ver="${rest#*:}"
  curl -fsSL "https://dl-cdn.alpinelinux.org/alpine/v$V/$repo/aarch64/$pkg-$ver.apk" -o "/tmp/$pkg.apk"
  mkdir -p "/tmp/x-$pkg"; tar -xzf "/tmp/$pkg.apk" -C "/tmp/x-$pkg"
done
# then copy the real (non-symlink) usr/lib/*.so.N.N.N files here, renamed to
# their SONAME (e.g. libserd-0.so.0.32.2 -> libserd-0.so.0) — the symlink
# entries in the .apk tarball extract out of order and can be ignored/skipped.
```

Verify with `file vendor/lib-aarch64/*.so*` — every entry must say
`ELF 64-bit LSB shared object, ARM aarch64`.

## If aloop still fails to start after this

Check `/var/log/aloop.log` on the device (SSH in — see the SSH section of
`image/lib-boot-tree.sh`) for further `Error loading shared library X` /
`Error relocating ... symbol not found` lines: each one names the missing
`.so`, which you trace to its Alpine package the same way (APKINDEX `D:` line)
and add here. `ldd` isn't available in this minimal environment to preflight
the full closure locally — the device's own dynamic linker error IS the
authoritative list, one layer at a time.
