# Contributing to aloop

## Adding your own effect (the common case)
aloop's effects are LV2 plugins on flash — you don't touch the aloop source to
add one:
1. Write an effect in [Faust](https://faust.grame.fr) and build it:
   `faust2lv2 mything.dsp` → `mything.lv2/` (a `.so` + `.ttl`).
   (Or grab any existing LV2 plugin.)
2. Copy the `.lv2` bundle into `/effects/user/` on the SD card.
3. Reboot / rescan. It loads on the free core, in-process, zero added latency.

## Working on aloop itself
- **Read `docs/` first**, in the order `docs/README.md` gives. The *why* behind
  every design choice is in `docs/ARCHITECTURE.md` and `docs/DECISIONS.md`.
- **Every non-obvious decision appends an ADR to `docs/DECISIONS.md`** — with its
  rationale and the witnessed evidence. This is how we avoid losing insight; it
  is not optional.
- **The audio callback is sacred**: no allocation, no locks, no syscalls, no file
  I/O in the per-block path. RT-safety is a hard invariant, not a guideline.
- **Never introduce a JACK/PipeWire graph** into the effect path — it adds a full
  period of latency (ADR-002). Effects are hosted in-process.
- The build is reproducible via GitHub Actions (`.github/workflows/`); a change
  isn't done until CI is green.

## The plan
The whole migration is planned in `.gm/prd.yml` and drained in the order in
`docs/PLAN.md`. Hardware-dependent measurements (RT jitter, USB latency, on-air
Link) are parked as `blockedBy: external` until a real Pi 4 runs the documented
tests — never asserted from a dev host.
