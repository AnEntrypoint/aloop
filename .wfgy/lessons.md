# aloop — WFGY lessons

## 2026-07-12 — aloop is PURE LINUX; no ../looper (Circle) source belongs in it
Goal (G): get aloop to 100%, behaving like the looper but in Linux, effects = the dubfx LV2.
What drifted: I started vendoring ../looper source (a git submodule + CMake LOOPER_DIR + loopMachine.cpp + an effects_bridge shim mimicking looper's C++ types) to "100% clone" it. WRONG — looper is Circle bare-metal, a different platform; compiling its source into a Linux project is both infeasible (transitive Circle includes) and against the design (aloop is native Linux).
Fix / resolution: aloop is a behavior clone, not a source clone. Its loop engine is aloop-native (and can be Faust — see next lesson); its effects are the dubfx LV2 (the Linux-native reproduction, already A/B-verified). Stripped every looper reference from the build+docs.
Generalizes to: "100% clone" across platforms means reproduce the BEHAVIOR with native implementation, never compile the other platform's source. The equivalence proof is the A/B (dubfx already did it for effects), not shared source.

## 2026-07-12 — The loop engine can be pure Faust (rwtable), so aloop is a Faust program + thin Linux shell
Goal (G): decide whether the loop engine can be Faust for maintainability.
What drifted: nothing — this is a witnessed feasibility decision. Assumed a full multi-track looper's command control-flow might not fit Faust's pure-signal model.
Fix / resolution: witnessed a minimal Faust rwtable looper (dsp/loop_min.dsp) compiling to real record/play C++ (write input into a ring gated by rec, read the ring back, recursive phase counter). The rwtable + recursive read/write heads express record/play/loop directly — the same mechanism dubfx microrepeat used for stutter. The Faust/native line: DSP (record/play/mix/effects) = Faust; control-flow (command edge-detection, dynamic loop-length, MIDI, file I/O) = a thin native shim that feeds Faust as param inputs. So aloop = one Faust program (loop + home effects) + a swappable user LV2 + a minimal Linux RT shell (audio/Link/wifi/MIDI).
Generalizes to: buffer-based DSP that "feels" stateful/imperative (loopers, delays, granular) is expressible in Faust via rwtable + recursive heads; only the discrete control decisions need native code. Witness a minimal version compiling before committing the architecture.
