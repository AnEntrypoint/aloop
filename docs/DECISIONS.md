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
