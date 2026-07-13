# Remote control — reboot + log-tail over the network

aloop ports two of looper's dev-tooling conveniences — remote reboot and
live log streaming — adapted for aloop's Linux/Alpine architecture. See
`src/control/remote_control.h`/`.cpp` for the device side and
`image/aloop-reboot.js`/`image/aloop-logtail.js` for the host side.

## Why not looper's protocol unchanged

looper's UDP `REBOOT` (port 4444) and syslog capture (port 514) are:
- **Unauthenticated** — a bare 6-byte `"REBOOT"` string reboots the Pi from
  anyone on the LAN. aloop requires a shared-secret token instead.
- **Bare-metal-specific** — looper's firmware has no OS, so a UDP packet sets
  a flag another core polls to perform the reboot, and syslog is a hand-rolled
  UDP protocol because there's no real filesystem to log to. aloop **is**
  Linux: `reboot(2)` is a real syscall, and aloop's log is already a real file
  (`/var/log/aloop.log`, via OpenRC's `output_log`/`error_log`).

## Enabling it

Unset (no `token=` line) = the listener is **disabled** — the safe default.
To enable, add to `/etc/aloop.conf`:

```
[remote]
token = <a real secret, not the example below>
```

Anyone who knows this token can reboot the device over the network. Treat it
like a password.

## Protocol (udp/4446)

Two verbs, both `<VERB>:<token>` as the UDP payload:

- **`REBOOT:<token>`** — `sync()`s the filesystem, then calls `reboot(2)`
  (`RB_AUTOBOOT`). No reply (the device reboots). Needs `CAP_SYS_BOOT` — the
  diskless boot runs `aloop` as root, so this is satisfied by default.
- **`LOGTAIL:<token>`** — replies with whatever bytes have been appended to
  `/var/log/aloop.log` since the *previous* `LOGTAIL` request from any
  client (one shared read offset per process lifetime, not per-client — a
  deliberate simplification matching looper's single-dev-host assumption).
  The **first** `LOGTAIL` call after boot establishes a baseline at
  end-of-file and returns nothing — a freshly-connecting client sees only
  new lines going forward, like `tail -f`, not the whole boot history like
  `cat`. Capped at 4096 bytes per reply (poll more often for a busy log).

Telemetry's existing `udp/4445` (`src/control/telemetry.cpp`) is deliberately
**not** reused for this — its "any packet gets a status reply" semantics must
never be confused with a destructive verb like reboot.

## Host-side scripts

```sh
node image/aloop-reboot.js  --host 192.168.137.100 --token <secret>
node image/aloop-logtail.js --host 192.168.137.100 --token <secret>
```

Or via env vars: `PI_HOST`, `PI_PORT`, `PI_TOKEN`, `PI_LOGTAIL_INTERVAL`
(logtail poll interval, default 1000ms).

`aloop-logtail.js` highlights lines matching `panic|crash|kernel panic|fatal`
(case-insensitive) with a `!!!` prefix, the same live-visibility looper's
`syslog-listener.js`/`tftp-server.js` gave during dev.

## What was already there — no new mechanism needed

- **Crash-loop recovery** (looper's `dev-server.js` restarts a crashed child
  2s after exit, forever): aloop's `etc/init.d/aloop` OpenRC service already
  has `respawn_delay=2 respawn_max=0` (unlimited respawns) — the same
  guarantee, at the OS/service-supervisor level instead of a Node wrapper.
- **Status query** (looper's `pi-debug.js` sends `STATUS` to udp/4445):
  `src/control/telemetry.cpp`'s `Telemetry::publish()` already answers *any*
  UDP packet on `udp/4445` with the full status JSON, and also writes
  `/run/aloop/status.json` for local `curl`/shell inspection.
- **Serial console capture** (looper's `otg-monitor.js`, Windows-COM-port
  specific): not ported as a new script — `docs/NETBOOT.md`'s debug-boot
  section already documents the serial-console procedure for aloop's specific
  hardware (`dwc2`/`enable_uart=1`), and `otg-monitor.js`'s hardcoded `COM11`
  + `pnputil` phantom-device-clearing logic is machine-specific enough that a
  generic port would need per-machine configuration anyway.
