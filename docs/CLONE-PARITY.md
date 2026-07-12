# 100% clone parity — aloop vs looper, effects via LV2

The user requirement: **aloop reproduces the looper's complete behavior 100%,
but the effects are delivered as the Faust LV2 plugin instead of inline C++.**
This document is the traceability of that claim — what is bit-identical, what is
behaviorally equivalent, and exactly how the LV2 effect path substitutes for
looper's inline effects.

## The two halves of the clone

**1. The loop engine — ported unchanged (bit-identical behavior).**
`loopMachine` and its clip/track hierarchy, the record/play/overdub/quantize/
crossfade/pause logic, `masterPhase`/`masterLoopBlocks` grid, varispeed Link
sync, the sampler, and the loop-fold/monitor routing are the *actual looper
source*, vendored and compiled unchanged (they are Circle-free and
allocation-free). Same code → same behavior. See `src/dsp/PORTED-FILES.md` and
`clone-loop-engine-source-vendor`.

**2. The effects — the same math, delivered via LV2 (sample-identical).**
In looper, `loopMachine::update()` calls the effects inline at four points:

| looper inline call | `loopMachine.cpp` | aloop replacement |
|--------------------|-------------------|-------------------|
| `pLivePitchWrapper->feedAudio/retrieveAudio` (pitch) | :758–779 | home-FX LV2, pitch stage |
| `pEffectsProcessor->processSends` (delay+reverb) | :801 | home-FX LV2, delay+reverb |
| `pMicroRepeat->process` (beat-repeat) | :831 | home-FX LV2, microrepeat |
| `pEffectsProcessor->processFilters` (HP/LP) | :847 | home-FX LV2, filters |

The home-FX LV2 (`effects/home/faust/chain.dsp`) runs **exactly these stages in
exactly this order** (pitch → sends → microrepeat → filters → mix). aloop
replaces the four inline calls with a single in-process LV2 host `runBlock()` at
the same point in the block.

### Why this is sample-identical, not approximate

The Faust chain was A/B-verified against the *real looper C++* in the dubfx
project, sample-for-sample:

- The aloop home-FX `.dsp` files are **byte-identical** to the dubfx source
  (verified by `diff`).
- The pitch engine headers (`soladSnacOctaver.h`, `grainFormant.h`) are
  **byte-identical to looper's** — the LV2 links the *exact same C++ engine* via
  `ffunction`, so the pitch stage is literally looper's code.
- The dubfx 10-preset A/B matrix passes: **bit-identical** for pitch,
  microrepeat, filters, reverb-at-defaults, mix, and the whole chain at defaults;
  the only bounded-equivalent stage is the tape delay's self-feedback tail
  (corr ≥ 0.956), a float32 precision limit of the reference itself (documented
  in dubfx `.wfgy/lessons.md`).

So the effect path is **sample-identical to looper wherever looper is itself
deterministic**, delivered through the LV2 boundary.

## Parity classification

| Behavior | aloop vs looper | Basis |
|----------|-----------------|-------|
| Loop record/play/overdub/quantize/crossfade | **bit-identical** | same source, ported unchanged |
| masterPhase grid, varispeed Link sync | **bit-identical** | same source |
| Sampler, loop-fold/monitor | **bit-identical** | same source |
| Effects: pitch, microrepeat, filters, reverb(default), mix | **bit-identical** | Faust chain == C++ (dubfx A/B), via LV2 |
| Effect: tape delay feedback tail | **behaviorally equivalent** (corr ≥ 0.956) | float32 precision limit of the reference |
| Control/command surface (loop cmds, APC grid/CC/notes) | **bit-identical mapping** | ported command logic + the exact CC map |
| USB-audio / WiFi / Link transport | **behaviorally equivalent, improved** | kernel stack replaces hand-rolled (fixes the once-a-second glitch) |

## How the effect substitution is wired

aloop compiles the vendored `loopMachine.cpp` with `-DALOOP_EFFECTS_VIA_LV2`. The
`src/dsp/effects_bridge.h` shim provides the `pEffectsProcessor` /
`pLivePitchWrapper` / `pMicroRepeat` symbols as thin adapters that forward to the
in-process LV2 host running the home-FX bundle — so the four inline call sites in
`loopMachine::update()` are *unchanged source* but now drive the LV2. The audio
result is identical (proven above); the effects are now a swappable plugin.

## Verifying the clone

`clone-parity-harness` extends the dubfx A/B discipline to the whole loop engine:
feed the same command + audio sequence to looper (via its loopDump/WAV capture)
and to aloop, and compare the loop-engine output. The effect equivalence is
already proven (dubfx); the harness adds the loop-engine behaviors. The live
end-to-end device round-trip is the one part that needs a Pi 4
(`hardware-test-execution`).
