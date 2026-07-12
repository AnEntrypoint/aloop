# DSP files ported from ../looper (Circle-free, unchanged)

These are the loop engine + effects that carry NO Circle dependency and are
allocation-free — they compile on Linux exactly as they ran on a bare-metal core
(docs/ARCHITECTURE.md, MIGRATION-MAP.md). The dubfx project already proved the
effect subset compiles standalone and produces sample-identical output; that A/B
harness is our regression oracle.

| looper file | role | notes |
|-------------|------|-------|
| `loopMachine.cpp` / `Looper.h` | the loop engine + `loopMachine::update()` audio callback | the audio heart; driven per 64-sample block |
| `patches/apcEffectsProcessor.h` | delay + reverb + HP/LP filters | pure DSP, header-only |
| `patches/microRepeat.h` | beat-repeat / stutter | pure DSP |
| `patches/soladSnacOctaver.h` + `grainFormant.h` | live pitch engine (SOLA + SNAC + grain formant) | the ONLY live-pitch engine (ADR-004) |
| `patches/RubberBandWrapper.h` | thin wrapper around the pitch engine | signalsmith/etc are dead members, dropped |
| `patches/sampler.h` | sampler voices | pure DSP |
| `patches/paramSnapshot.h` | the lock-free control→audio snapshot | the RT-safe boundary, ported unchanged |
| `apcKey25*.cpp` | MIDI CC/note → param mapping | logic ports; input source becomes ALSA rawmidi |

What does NOT port (replaced — see MIGRATION-MAP): the USB gadget, the WiFi/DHCP/
Link raw stack, the multicore SEV/WFE primitive, and all Circle HW classes.

## How the port is wired
The aloop build (`src/CMakeLists.txt`, added with the audio-callback row) pulls
these files from `../looper` (or a pinned vendored snapshot) and compiles them
against the Linux audio callback (`src/dsp/audio_thread.cpp`) instead of the
Circle `AudioSystem`. No edits to the DSP files themselves — the whole point is
that they are already portable.

## Full loop-engine port (the 100%-clone requirement)

Beyond the effects, the ENTIRE loop engine ports unchanged for the 100% clone
(docs/CLONE-PARITY.md, docs/COMMAND-SURFACE.md):

| looper file | role |
|-------------|------|
| `loopMachine.cpp` / `Looper.h` | the whole loop machine: record/play/overdub/quantize/crossfade/pause, tracks/layers/clips, masterPhase grid, varispeed Link sync, loop-fold/monitor, the final mix |
| `patches/sampler.h` | the sampler voices |
| `patches/paramSnapshot.h` | the lock-free control→audio snapshot |
| `apcKey25*.cpp` | the full command + control mapping (input → ALSA rawmidi) |
| `commonDefines.h`, `LooperVersion.h` | constants |

Compiled with `-DALOOP_EFFECTS_VIA_LV2`: the four inline effect call sites in
`loopMachine::update()` are UNCHANGED source but resolve to the `effects_bridge.h`
shims, which forward to the home-FX LV2. Loop engine = looper's exact code;
effects = the swappable Faust LV2. Result: byte-identical audio (dubfx A/B proof).
