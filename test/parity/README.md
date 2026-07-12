# Clone parity harness — aloop loop engine == looper loop engine

The dubfx A/B harness already proves the EFFECTS are sample-identical (the Faust
LV2 == looper's C++ effects, 10/10 presets). This harness extends that discipline
to the WHOLE loop engine, to witness the 100%-clone claim (docs/CLONE-PARITY.md).

## Method
Both looper and aloop expose a deterministic "dump" path: given the same input
audio + the same command sequence (record/play/overdub/quantize/…) at the same
block indices, they produce the same loop output. looper dumps via loopDump.cpp /
its WAV recorders; aloop dumps the same buffer at the same point.

`parity.sh`:
1. Drives a scripted command+audio sequence through the looper reference build
   (compiled from ../looper with a headless harness, like dubfx's refharness).
2. Drives the identical sequence through aloop (loopMachine compiled with
   -DALOOP_EFFECTS_VIA_LV2, effects via the home-FX LV2).
3. Compares the loop-engine output with the dubfx abcompare tool (maxAbs / rms /
   corr), per behavior.

## What it proves vs what needs hardware
- Loop-engine behavior (record/play/overdub/quantize/crossfade/varispeed/sampler/
  fold) — DETERMINISTIC, compared here, no hardware.
- The effect stages — already proven by the dubfx A/B (reused).
- The LIVE end-to-end device round-trip (real USB audio in/out) — the one part
  that needs a Pi 4 (hardware-test-execution).

The reference looper build reuses the dubfx refharness pattern (it #includes the
real ../looper source), so this is the same trusted oracle extended from effects
to the full engine.
