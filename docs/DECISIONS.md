# Decision log (ADRs)

An append-only record of every non-obvious decision, with the rationale and the
**witnessed evidence** behind it — so insight is never lost as the build
progresses. New decisions append to the bottom. Each entry:

```
## ADR-NNN — <title>   (<date>, <phase/context>)
Decision: <what was decided>
Why: <the reasoning>
Evidence: <the witnessed fact — a file:line, a measurement, a doc reference>
Alternatives rejected: <what else was considered and why not>
Revisit if: <the condition that would reopen this>
```

The first block of ADRs is seeded from the feasibility study
([`FEASIBILITY.md`](FEASIBILITY.md)); everything after is decided during the build.

---

## ADR-001 — Target Alpine Linux + PREEMPT_RT on Raspberry Pi 4
Decision: Migrate the bare-metal Circle looper to Alpine Linux with a PREEMPT_RT kernel, on Pi 4.
Why: Linux gives a filesystem + dynamic linker (needed for hot-swappable LV2 effects) and a tested WiFi/USB stack; PREEMPT_RT keeps worst-case scheduling jitter in the tens of microseconds so 64-sample blocks survive; Alpine's diskless/RAM appliance mode gives near-bare-metal determinism.
Evidence: `FEASIBILITY.md` — the whole study; the four goals are impossible on bare metal (no filesystem/linker) but feasible on Pi 4 Linux.
Alternatives rejected: staying bare-metal (blocks hot-swap + carries the hand-rolled-driver maintenance); a full desktop distro (more background jitter than Alpine).
Revisit if: the project moves to Pi 5 (see ADR-006).

## ADR-002 — Effects are LV2 plugins hosted IN-PROCESS, never a JACK/PipeWire graph
Decision: `dlopen` the LV2 bundles and call `run()` inside aloop's own audio callback; do not use a graph host.
Why: A graph host schedules clients across period boundaries, adding one full audio period (~1.333 ms) of latency per hop — violating the no-added-latency goal. In-process calling adds zero latency.
Evidence: witnessed latency arithmetic (`FEASIBILITY.md`, Requirement 3): serial-in-process = 0 ms added; fork-join parallel = 0 ms; JACK graph = +1 period. Combined chain ~0.9 ms fits one 1.333 ms block.
Alternatives rejected: JACK2/PipeWire multi-client graph (adds a period); LV2 as just a static-linked struct with no swap (loses the moddability point).
Revisit if: the chain becomes CPU-bound beyond what fork-join across Core 1+3 can cover.

## ADR-003 — Reuse the dubfx verified Faust chain as the home-FX LV2
Decision: Package the dubfx Faust chain (pitch/delay/reverb/microrepeat/filters) with `faust2lv2` as the fixed home effects bundle; do not re-derive the effects.
Why: dubfx already reproduced looper's effects and A/B-verified them sample-for-sample against the real C++. That work is done and witnessed; reusing it is correct and saves rebuilding.
Evidence: dubfx `README.md` (10-preset A/B matrix, all pass); `faust2lv2` codegen witnessed producing a 69 KB LV2 architecture with LV2_Descriptor refs.
Alternatives rejected: rewriting the effects natively (wasteful; loses the A/B guarantee).
Revisit if: never — this is a strict reuse.

## ADR-004 — Live pitch = EngineSoladSnac (soladSnac), no signalsmith/RubberBand runtime dep
Decision: The live pitch stage links `EngineSoladSnac` (`soladSnacOctaver.h` + `grainFormant.h`) exactly as dubfx did via `ffunction`.
Why: looper's live pitch path uses ONLY EngineSoladSnac; the signalsmith/yinPsola/sincFormant engines are dead code removed from the wrapper. Matching looper means using soladSnac.
Evidence: `../looper/patches/RubberBandWrapper.h:10-17` — "ONLY EngineSoladSnac is in the live pitch path. The former embedded signalsmith / yinPsola / sincFormant engines were dead members."
Alternatives rejected: using signalsmith/RubberBand (not what looper ships; would change the sound).
Revisit if: looper's live pitch engine changes.

## ADR-005 — Ableton Link via the official library, over UDP
Decision: Delete the hand-rolled raw-Ethernet Link clone; use github.com/Ableton/link over ordinary UDP multicast 224.76.78.75:20808.
Why: The bare-metal clone existed only because there was no IP stack. Linux has one, so the maintained library is correct, RT-safe, and interoperable. It maps onto the same lock-free snapshot handoff the looper already used.
Evidence: `FEASIBILITY.md` R1; looper's clone (`abletonLink.cpp`, 938 lines) is pure overhead on Linux; Ableton Link's `captureAudioSessionState` is RT-safe.
Alternatives rejected: porting the raw clone (maintenance burden, no benefit).
Revisit if: never (the library is the standard).

## ADR-006 — Pi 4 only; Pi 5 reopens the no-glitch guarantee
Decision: Target Pi 4. Treat Pi 5 as out of the current guarantee.
Why: The no-glitch guarantee depends on steering WiFi/network IRQs off the audio core. Pi 4 allows this; Pi 5's RP1 I/O controller has `PF_NO_SETAFFINITY` on force-threaded IRQ handlers, so those IRQs cannot always be moved.
Evidence: `FEASIBILITY.md` Pi-4-vs-Pi-5 caveat; raspberrypi/linux#7301.
Alternatives rejected: targeting Pi 5 now (risks R1's no-glitch claim without a mitigation).
Revisit if: the project must run on Pi 5 — then investigate RP1 IRQ handling / a mitigation before promising no-glitch.

## ADR-007 — WiFi = autoAP mode-switching (STA-or-AP), not simultaneous AP+STA
Decision: Try station mode; host an AP only when no known network is available; switch back when one returns.
Why: The requirement is "AP when external unavailable" = mode-switching, a solved pattern. Simultaneous single-radio AP+STA is flaky on the Pi (`nl80211: Could not configure driver mode`).
Evidence: `FEASIBILITY.md` R4; looper already implements join-first-then-fallback-to-AP (`kernel.cpp:85-169`); the autoAP project.
Alternatives rejected: simultaneous AP+STA on one radio (flaky). If ever truly needed → USB WiFi dongle for a second radio (documented, not built).
Revisit if: a use case genuinely needs the Pi on home WiFi AND hosting a Link AP at the same time.

## ADR-008 — USB audio via kernel f_uac2 configfs gadget
Decision: Replace looper's hand-rolled UAC2 gadget (`dwusbgadget.cpp`) with the Linux `f_uac2` configfs gadget on dwc2.
Why: The kernel lays out isochronous USB packets correctly by construction, deleting the entire class of microframe-corruption bugs looper had to fix by hand. It is the hardest port but removes the most maintenance.
Evidence: `FEASIBILITY.md` USB section; looper's buzz/crackle history all traced to the hand-rolled decode.
Alternatives rejected: porting the hand-rolled gadget (carries the bugs forward).
Revisit if: f_uac2 cannot hit the latency target on-hardware (then measure and tune, don't revert).

## ADR-009 — Hardware-dependent verification is honestly marked, never faked
Decision: Rows that need a real Pi 4 to *measure* (RT jitter via cyclictest, f_uac2 round-trip latency, on-air Link no-glitch, AP multicast) are marked `blockedBy: [external, no-Pi4-in-session]` with the exact on-hardware test documented — never resolved as done from a dev host.
Why: A measurement claim without the measurement is false completion. The build (code/config/CI/docs) is fully reachable now; only on-device numbers are external.
Evidence: gm false-completion rule; these rows cannot be witnessed without hardware.
Alternatives rejected: asserting the numbers from analysis (dishonest).
Revisit if: a Pi 4 becomes available in-session — then run the documented tests and resolve with real output.

## ADR-010 — Loop engine is a Faust feedback-delay ring, not a buffer+playhead (rwtable)
Decision: Each of the 20 loopers is a cycle-free Faust feedback-delay ring (`de.fdelay` with `step ~ _`): record replaces the loop, play recirculates it, no overdub. A buffer+playhead (`rwtable`) engine with an addressable read head was attempted and rejected.
Why: Record/play/stop/erase/clear/half-double-speed and Link-driven varispeed loop length all map cleanly onto the delay ring, and it compiles. The buffer+playhead was pursued ONLY to gain an addressable read position for the hardware's mark-point commands (SET/CLEAR_LOOP_START 0x09/0x0A, LOOP_IMMEDIATE 0x08). But a preserve-on-hold playhead looper must read the buffer and write the read-back to the SAME buffer so the loop survives while not recording — a read-modify-write Faust's pure-signal evaluator forbids.
Evidence: 4 witnessed CI faust-codegen failures on the rwtable engine: `loop.dsp:75 syntax error` (invalid `\(prev).(…) ~ _` lambda), then `stack overflow in eval` (rp↔mark mutual recursion, twice, even with both cross-refs delayed), then `endless evaluation cycle of 8 steps` (the write-back RMW, even feeding back `stored'`). The delay ring was green before and after. See `.wfgy/lessons.md` (2026-07-12 Faust-recursion lesson).
Consequence: mark-point / immediate-retrigger (0x08–0x0A) is the ONE loop command family not reproduced — a deliberate, documented model difference (COMMAND-SURFACE.md), not a silent drop. Every other command maps 1:1.
Alternatives rejected: (a) rwtable playhead — infeasible in pure Faust (the RMW cycle above); (b) implementing the loop buffer in native C++ instead of Faust — reintroduces hand-written DSP maintenance, defeating the single-Faust-program design (ADR-001); (c) claiming mark-point works — false completion.
Revisit if: a future Faust release supports a write-enable / RMW-safe table primitive, OR the mark-point behavior becomes a hard requirement (then the loop buffer moves to native C++ as a scoped, deliberate exception with its own ADR).

## ADR-011 — Ship the flashable image with the stock Alpine kernel; PREEMPT_RT is an on-hardware optimization
Decision: The `aloop-pi4.img` ships Alpine's stock `linux-rpi` kernel (from the Alpine RPi tarball) plus the full userspace RT tuning (isolcpus/nohz_full/rcu_nocbs/threadirqs in cmdline.txt + SCHED_FIFO + pinned affinity + mlockall). Building a PREEMPT_RT kernel (`kernel/build-rt-kernel.sh`) is an OPTIONAL step taken only if on-hardware measurement shows the stock kernel misses the 64-sample no-xrun target.
Why: The user's goal is to test from a card. A bootable image with the stock kernel + full userspace tuning boots and is testable TODAY, and running it is precisely how we measure whether the stock kernel already suffices. Gating card-testing on a ~30-min CI cross-compile of an RT kernel — which itself can't be validated without the hardware we're trying to reach — inverts the honest order. Ship + measure first, optimize second.
Evidence: `kernel/build-rt-kernel.sh` writes the RT config fragment (PREEMPT_RT, NO_HZ_FULL, f_uac2/dwc2, HZ_1000) but does not build a kernel; the stock kernel ships in the tarball the image builder extracts; RT-TUNING.md documents the userspace knobs that apply regardless of kernel.
Consequence: first-boot latency is whatever the stock kernel + tuning delivers — measured by `test/hardware/test-rt-jitter.sh` (shipped on the device). If it misses, build the RT kernel and re-flash; the fragment + cross-build recipe are ready.
Alternatives rejected: (a) block the flashable image on a full RT-kernel CI build (inverts ship-then-measure; unverifiable without hardware); (b) claim RT latency without measuring (false completion, ADR-009).
Revisit if: on-hardware RT-jitter measurement misses the target → run the RT-kernel build and fold it into the image.

## ADR-013 — The Faust home DSP struct is heap-allocated in the audio worker thread, never stack-local
Decision: `worker()` (src/dsp/audio_thread.cpp) allocates `AloopLoopDsp` via `std::make_unique<AloopLoopDsp>()` at thread startup, binding a reference alias for the rest of the function. It is never declared as a plain stack-local (`AloopLoopDsp faustHome;`).
Why: `sizeof(AloopLoopDsp)` is 336,326,896 bytes (~320 MiB) — the 20 loopers' `MAXLEN = 48000*60` (60s) delay-line buffers dominate. As a stack-local inside a pthread's entry function, no thread stack size is large enough to hold it; a real, reproducible SIGSEGV was witnessed on a real Pi 4 at `setRealtimeSelf`'s very first prologue instruction. The heap allocation happens once at thread startup, never in the per-block RT hot path, so it carries none of the "no allocation in the audio callback" real-time risk the rest of this file is written to avoid.
Evidence: `gdb` (debug build, `-g -O0`, unstripped, built via a temporary CI job) against a real core dump: `print sizeof(AloopLoopDsp)` → `336326896`. Two prior hypotheses were tested and disproven first via live core-dump analysis: an explicit 8 MiB pthread stack (still crashed, identical offset) and a memlock/`ulimit -l` fix (crashed identically even with `ulimit -l unlimited` applied interactively). Fix verified live: `rc-status` shows `aloop [started]` (not crashed) staying up, `/var/log/aloop.log` shows a full clean startup, and a live UDP telemetry query returns real engine state.
Consequence: any future Faust-generated struct added to the home stack must be checked for size before being declared as a local; if it is more than a few KB, heap-allocate it the same way.
Revisit if: the loopers' buffer sizing changes enough that stack allocation becomes plausible again (it will not — 20 loopers × 60s × 48kHz float buffers is inherent to the design, not incidental).

## ADR-014 — ALSA PCMs are opened with explicit hw_params/sw_params against a stable device name, never "default" + convenience latency API
Decision: `audio_thread.cpp` opens its wire audio devices by stable name (never ALSA's `"default"` PCM). hw_params are set explicitly (`period_size` = `block_size` exactly on the instrument device) instead of via `snd_pcm_set_params()`'s convenience latency argument. sw_params explicitly sets `start_threshold` to one period on both capture and playback. The f_uac2 gadget's own `req_number` (isochronous USB request queue depth) is raised from the kernel default of 2 to 4 in `src/usb/f_uac2-gadget.sh`.
Why: `"default"` is ambiguous without a shipped `/etc/asound.conf` (this device has multiple real ALSA cards — an instrument audio interface, a MIDI controller, and the OTG gadget — plus dmix/dsnoop's own large fixed period on top), and `snd_pcm_set_params(..., latency_us)` picks whatever period/buffer satisfies a requested LATENCY NUMBER, not the configured `block_size` — the as-shipped code requested 20ms, ~15x the intended 1.33ms. Separately, the hw_params-default `start_threshold` for a from-scratch-configured playback stream is the full buffer size, but a loop that only writes one period per `snd_pcm_writei()` call before blocking on the next capture read never crosses that default threshold, so the stream stays permanently `PREPARED` (never actually running) while capture (whose default threshold is 1 frame) runs fine — the two streams silently desync.
Evidence: all fixes were witnessed live on a real Pi 4 via SSH — `/proc/asound/.../hw_params` showing `period_size:64` (matching `block_size`) after the device+hw_params fix (was an unpinned ~20ms request before); `/proc/asound/.../status` showing `state: PREPARED` (stuck) for playback before the sw_params fix, `RUNNING` after; `/sys/kernel/config/usb_gadget/aloop/functions/uac2.0/req_number` reading `4` after the gadget-script fix (was kernel default `2`). A live diagnostic build (temporary `fprintf` logging on `snd_pcm_readi`/`writei` error codes, since removed) confirmed a residual xrun source was `-EIO` with a frozen `hw_ptr` on the gadget's PCM specifically — traced to no host application actively streaming audio through the gadget device at verification time (confirmed with the user directly), i.e. expected idle-USB-audio-class behavior, not a code defect. This diagnosis was BEFORE ADR-015 corrected which physical device is actually the primary DSP I/O — see ADR-015 for why the gadget was never the right primary target to begin with.
Consequence: any future change to `block_size` in `aloop.conf` automatically re-derives the ALSA period without further tuning (the hw_params call always targets `N = block_size` frames exactly). If a different or newer UAC2-class device is ever substituted, re-verify `req_number` and the sw_params thresholds against it — these are values tuned against THIS gadget driver's specific behavior, not universal constants.
Revisit if: a future USB audio gadget driver changes its own internal buffering defaults, or block_size is lowered enough that the gadget mirror's buffering stops being sufficient against real-world USB scheduling jitter (re-measure xruns with an actual host audio session running, not an idle default-device check).

## ADR-015 — Two distinct ALSA devices: the real instrument USB audio interface (primary, tight-latency) and the OTG gadget (mirror, best-effort)
Decision: `audio_thread.cpp` opens TWO separate ALSA devices, matching `../looper`'s architecture exactly: `instrumentDevice` (`aloop.conf [audio] instrument_device=`, default `hw:0,0` — the real USB audio interface an instrument/mic plugs into, e.g. the M-Audio AIR 192|4) is the primary capture+playback path the Faust engine reads from and writes to every block, blocking, tight-latency (matching `block_size` exactly, ADR-014). `audioDevice` (`aloop.conf [audio] audio_device=`, default `hw:UAC2Gadget,0` — the f_uac2 OTG gadget) is opened `SND_PCM_NONBLOCK` and written to as a best-effort MIRROR of the same processed output after the instrument-device write, with a looser period/buffer (4x block_size) and silent absorption of `-EAGAIN`/errors — it can never stall, desync, or degrade the instrument device's real-time path.
Why: an earlier version of this code (before this ADR) opened ONLY the OTG gadget as the sole audio device — meaning the real instrument USB audio interface was NEVER read from or written to at all, and the user's reported "no audible sound" / "no passthrough from input to output" was because the actual instrument input was never being captured. Deep research into `../looper`'s reference implementation (`patches/input_usb.cpp`, `output_usb.cpp`, `output_otg.cpp`) confirmed the real architecture: `AudioInputUSB`/`AudioOutputUSB` (the USB-HOST audio class driver talking to the real external interface) are the graph's ONLY real `AudioConnection` input/output nodes; `AudioOutputOTG`/`AudioInputOTG` are NOT audio-graph nodes at all — they are passive tap/inject shims that read/write the SAME ring buffers via independent cursors with deliberately looser latency targets (looper: `OTG_LAG_TARGET=384` samples vs the real device path's `OUT_RD_TARGET=96`, a 4x headroom difference), because OTG's job is only to mirror audio to/from whatever host computer is on the cable, not to be the real-time-critical path.
Evidence: `/proc/asound/card0/stream0` (the AIR 192 4) showed `Status: Stop` on both playback and capture before this fix, confirming it was never opened. `../looper` source citations: `input_usb.cpp:206` (`AudioInputUSB::inHandler`, the real capture path), `output_usb.cpp:188` (`AudioOutputUSB::outHandler`, the real playback path), `output_otg.cpp:6-16` + `output_usb.cpp:63` (`AudioOutputUSB_tapOTG`, the passive OTG mirror reading the same ring with an independent, longer-lag cursor), `audio.cpp:147-148` (only `input`/`output` — the real USB-audio-interface objects — get `AudioConnection`s in the graph; the OTG objects do not).
Consequence: `aloop.conf`'s `instrument_device=` must be pinned correctly for the physical hardware in use (default `hw:0,0` assumes the instrument interface enumerates before the gadget, true when the gadget is `UAC2Gadget` and not a numbered `hw:N`). If the instrument interface fails to open, audio stays down entirely (matching the pre-fix retry/backoff behavior) since it is now the primary path — the gadget mirror failing to open is silent-degrade-only and never blocks startup.
Revisit if: a setup exists with no instrument USB audio interface at all (headless/OTG-only operation) — then `instrument_device=` would need an explicit "none" mode that falls back to the gadget as primary, which does not exist yet because no such deployment has been described.

## ADR-012 — Drum-record-mode (per-key keybed sampler) is not ported
Decision: looper's drum-record-mode (`apcKey25.cpp:112,159-165,203,219`: holding note 66 arms `m_drumRecordMode`, then each keybed key records into its OWN independent sample slot) is NOT reproduced in aloop. This is a deliberate, documented model difference, not a silent gap.
Why: aloop's home stack is ONE shared Faust program (ADR-001's single-compile design) built around the 20-looper engine (`loop.dsp`), each looper being one feedback-delay ring bound to a fixed control group (`looper%2i/rec` etc, generated once via `par` at compile time). Drum-record-mode needs a DIFFERENT, independently-addressable per-key sample store (up to ~25 keybed keys, each its own record/play slot, unrelated to the 20 loopers) — a second, structurally distinct engine bolted onto the same audio path. Building it in Faust would mean either (a) a second fixed `par`-generated bank of 25 more feedback rings sized for one-shot sample playback rather than continuous looping (a different enough state machine — arm-then-record-one-shot vs the loopers' continuous record/play — that reusing `oneLooper` would be a misfit, not a reuse), or (b) native C++ sample storage, which reintroduces hand-written DSP maintenance the single-Faust-program design (ADR-001) exists to avoid.
Evidence: `dsp/loop.dsp`'s `oneLooper` is a continuous record/hold ring control-gated by `rec`/`play`/`erase` (no concept of a discrete one-shot capture-then-trigger cycle); `src/control/apc_grid.h/.cpp`'s `ApcGrid` has no per-key sample-slot storage, mirroring the engine's shape. No CI codegen attempt was made (unlike ADR-010's rwtable attempt) because the shape mismatch is evident from the existing engine design, not a Faust-language limitation to work around.
Consequence: the keybed's 25 pad-adjacent note range that looper repurposes for drum-record-mode is unbound in `config/controls.conf` (available for remapping); no equivalent feature exists. Every other APC Key25 gesture (grid, presets, tap/hold, live-pitch, microrepeat, SHIFT-gated formant range + monitor-fold) maps 1:1 (docs/COMMAND-SURFACE.md).
Alternatives rejected: (a) reusing `oneLooper` for one-shot sample slots — wrong state machine (continuous vs one-shot-then-hold), would need its own control surface anyway; (b) native C++ per-key sample engine — defeats ADR-001; (c) leaving it undocumented — false-by-omission, the same failure ADR-010 exists to avoid.
Revisit if: a concrete use case needs it — then scope it as its own bounded native-C++ module (like the LV2 host) rather than forcing it into the Faust home stack.
