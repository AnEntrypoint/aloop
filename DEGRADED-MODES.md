# Degraded modes — every failure path is explicit

An engineering invariant of aloop: **no silent catastrophic mode.** Every way the
system can be partially broken has a defined, non-crashing behavior. This is the
full-→degraded-→safe-fail-→explicit-error ladder applied to each subsystem.

| Subsystem | Failure | Behavior | Where |
|-----------|---------|----------|-------|
| **User effect** | LV2 bundle malformed / won't load | Skipped at load; logged; boot continues with home-FX only | `Lv2Host::loadDir` |
| **User effect** | LV2 segfaults or divides-by-zero in `run()` | SIGSEGV/SIGFPE handler → `disablePlugin()` → block continues with it bypassed; home chain unaffected | crash watchdog (ADR-002) |
| **User effect** | LV2 overruns its time budget repeatedly | Watchdog disables it after N overruns (same path); telemetry flags it | `runOne` + watchdog |
| **Home effect** | (should never fail — it's the verified chain) | If it faults, the host bypasses it to raw passthrough rather than dropping audio | crash watchdog |
| **Ableton Link** | No peers on the network | Free-run the internal tempo; publish it so a peer that joins later syncs to us | Link session default |
| **Ableton Link** | WiFi jitter / packet loss | Sync accuracy degrades gracefully (Link re-measures the offset); audio is never gated on Link | snapshot boundary (R1) |
| **WiFi** | No known network in range | Host the AP so peers can still Link | `autoap.sh` |
| **WiFi** | Radio init fails entirely | Run standalone (Link disabled); audio + effects still work locally | autoap error path |
| **USB audio** | Host disconnects | Output silence; loops keep their state and play position; resume cleanly on reconnect | ALSA xrun/disconnect handling |
| **USB audio** | Host changes sample rate | Reject rates ≠ 48k (the gadget advertises only 48k/mono); host must match | f_uac2 descriptor |
| **RT scheduling** | An xrun happens under extreme load | Count it in telemetry; recover on the next block (never spin or stall) | audio callback |
| **MIDI** | Controller unplugged | Params hold their last value; reconnect re-binds | ALSA rawmidi hotplug |
| **Config** | `aloop.conf` missing / malformed | Fall back to documented defaults; log which keys were defaulted | config loader |

The rule behind all of these: **fail loud in logs, degrade quietly in audio.** A
performer must never hear a crash — at worst they hear one effect drop out while
everything else keeps running.
