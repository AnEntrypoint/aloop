# effects/ — LV2 plugins (hot-swappable)

Effects are LV2 plugins, not compiled into the binary. That's the whole point of
the migration: **drop a `.lv2` bundle on flash and it loads — no recompile.**

- `home/` — the fixed home effects: the verified dubfx Faust chain
  (pitch → delay → reverb → beat-repeat → filters), packaged with `faust2lv2`.
  Runs on Core 1.
- `user/` — **your** effects. Drop any LV2 bundle here and it loads on the free
  Core 3. Write one in [Faust](https://faust.grame.fr) (`faust2lv2 mything.dsp`)
  or use any existing LV2 plugin.

## Adding your own effect
1. Build or obtain an LV2 bundle (`.lv2` directory with a `.so` + `.ttl`).
2. Copy it into `/effects/user/` on the SD card.
3. Reboot (or trigger a rescan). It loads on Core 3, in-process, zero added latency.

A crashing user plugin is caught by a watchdog and disabled — the home chain and
audio keep running (see ADR-002 / `docs/ARCHITECTURE.md`).
