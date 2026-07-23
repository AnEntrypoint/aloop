# Command surface — the looper control behavior aloop clones

100% clone includes the control/command behavior, not just audio. aloop
reproduces looper's full command surface (the ported apcKey25*.cpp logic + loop
commands), with the input source moved to ALSA rawmidi (src/control/midi.cpp).

## Loop commands (ported from looper loopMachine command dispatch)
record · play · stop · clear/erase (per track) · halve/double speed (varispeed) · loop-immediate · set/clear loop-start — for each of the 20 independent loopers. NO overdub.
· set/clear mark point · pause (mute-with-advancing-head) · quantize-to-grid.

### Exact opcode → aloop control target (commonDefines.h → config/controls.conf)
The hardware speaks `LOOP_COMMAND_*` opcodes; aloop's control map speaks named
targets that resolve to Faust zones (src/dsp/audio_thread.cpp `targetToZone` +
the engine-global `clear`/`speed` handling). This is the authoritative parity map:

| looper opcode | value | aloop target | how it's realized |
|---|---|---|---|
| `LOOP_COMMAND_RECORD` (per track `TRACK_BASE 0x20`) | 0x80 / 0x20+i | `looper<i>/rec` | Faust `button("rec")` — record replaces the loop (NO overdub) |
| `LOOP_COMMAND_PLAY` | 0x81 | `looper<i>/play` | Faust `checkbox("play")` — gates the looper output |
| `LOOP_COMMAND_STOP` / `STOP_IMMEDIATE` | 0x03 / 0x02 | `cmd/stopall` | audio thread clears **every** `looper<i>/play` to 0 |
| `LOOP_COMMAND_STOP_TRACK_BASE` | 0x40+i | `looper<i>/play`=0 | per-track stop = clearing that one play checkbox |
| `LOOP_COMMAND_CLEAR_ALL` | 0x01 | `cmd/clearall` | engine-global — a plain `process()` signal input (3rd input, `clearBuf` in audio_thread.cpp), NOT a Faust UI zone — wipes all 20 loops |
| `LOOP_COMMAND_ERASE_TRACK_BASE` | 0x60+i | `looper<i>/erase` | per-looper Faust `button("erase")` — wipes that one loop |
| `LOOP_COMMAND_HALFSPEED_ON/OFF` | 0x0C/0x0D | `cmd/halfspeed` | engine-global `speed`=0.5 while held (varispeed read rate) — a plain `process()` signal input (4th input, `speedBuf`), NOT a Faust UI zone (see below) |
| `LOOP_COMMAND_DOUBLESPEED_ON/OFF` | 0x0E/0x0F | `cmd/doublespeed` | engine-global `speed`=2.0 while held (2× read rate), same signal-input mechanism |
| `LOOP_COMMAND_ABORT_RECORDING` | 0x06 | `looper<i>/rec`=0 | releasing rec ends the take; record replaces in place so there is nothing to "un-append" |
| `LOOP_COMMAND_LOOP_IMMEDIATE` | 0x08 | *(model difference)* | needs an addressable read head — see the note below |
| `LOOP_COMMAND_SET_LOOP_START` | 0x09 | *(model difference)* | needs an addressable read head — see the note below |
| `LOOP_COMMAND_CLEAR_LOOP_START` | 0x0A | *(model difference)* | needs an addressable read head — see the note below |
| Link tempo/phase | — | `looper<i>/len` | audio thread sizes every loop from the Link BPM (varispeed grid sync) |

### The one deliberate model difference: mark-point / immediate re-trigger (0x08–0x0A)
These three commands reposition an **addressable read head** (set a restart mark at
the current play position; jump all heads to their mark). The aloop loop engine is a
Faust **feedback-delay ring** — record replaces the loop, play recirculates it — which
has NO addressable read position, so it cannot express a mark-point jump.

Why not a buffer+playhead (rwtable) engine, which *does* have an addressable head?
Because a preserve-on-hold playhead looper must **read the buffer and write the
read-back to the same buffer** (so the loop survives while not recording) — a
read-modify-write that Faust's pure-signal evaluator rejects. This was witnessed
across **4 CI codegen attempts** (`syntax error` → `stack overflow in eval` →
`endless evaluation cycle of 8 steps`); the delay ring sidesteps RMW by construction
and is the correct Faust looper. See `.wfgy/lessons.md` and the ADR in `DECISIONS.md`.

Everything ELSE maps 1:1 (record/play/stop/stop-all/erase/clear/half-double-speed,
plus Link-driven varispeed loop length). Mark-point is the single behavior traded for
a single-Faust-program, maintainable, RMW-free engine — a documented, evidence-backed
model choice, not a silently dropped feature.

Momentary semantics match the hardware exactly: `HALFSPEED`/`DOUBLESPEED` and
`clear`/`erase` are **held** (value 1 = active, release = neutral), driven each
block from the atomic ParamStore the MIDI thread writes. `speed` composes with the
Link-set `len`: Link sets the base loop length, `speed` divides it (loop stays
grid-locked, plays at 0.5×/2×). The `TRACK_BASE 0x20` / `STOP_TRACK_BASE 0x40` /
`ERASE_TRACK_BASE 0x60` families are 20 contiguous per-track slots — the default
`config/controls.conf` binds all 20 of each; remap freely (no recompile).

**`clear`/`speed` are process() signal inputs, not Faust UI zones (fixed in
commit 9806835, 2nd-generation fix on top of 382e775's incomplete attempt):**
Faust's `par(i, NLOOPERS, vgroup(...))` combinator re-elaborates whatever UI
primitive (`button()`/`hslider()`) is textually referenced inside its body at
EACH of the 20 instantiation sites — even when the declaration itself is
hoisted to file scope and threaded in as an ordinary function parameter. This
was WITNESSED via the generated C++ (`build/loop.cpp`): `"speed"`/`"clear"`
each appeared 20 times, one per `"looper N"` vgroup, even after 382e775
believed it had collapsed them to a single shared zone. There is no Faust
mechanism for "declare a UI control once, reference the same zone from many
call sites" across a `par()` boundary. The real fix removes `clear`/`speed`
as Faust UI controls entirely: `dsp/loop.dsp`'s `process()` now takes 4
signal inputs — `(in, prevFiltIn, clearAll, speedMul)` — and `clearAll`/
`speedMul` are plain wires threaded through `par()`, which cannot duplicate a
signal the way it duplicates a UI primitive. `audio_thread.cpp` fills
`clearBuf`/`speedBuf` (constant across each block) and passes them as
`fins[2]`/`fins[3]` to `compute()` instead of calling `fui.set("clear"/
"speed", ...)`.

## APC Key25 controls (the exact mapping — src/control/midi.cpp, param_mapping.md)
- CC48–55 → reverb/delay/time/HP/LP/resonance (all `/127`)
- CC53 → formant depth (deadzone + range, SHIFT expands) — `ApcGrid::onFormantCC`
- CC52 / keybed / mod-wheel → live pitch semitones
- notes 82–86 → microrepeat divisors {1,2,4,8,16}
- notes 70/71 → global speed-scrub (varispeed)
- SHIFT (note 0x62/98, channel 0 only) → held state gating CC53's range AND the
  loop-fold/monitor routing: while held, the running loops are folded INTO the
  effect input (`fx/monitorfold` → Faust `MONITORFOLD`, `dsp/aloop.dsp`'s
  `foldMix`) with the dry loop contribution complementarily suppressed, so the
  loops are heard once, through the effects — `ApcGrid::onShiftPress/Release`,
  `AudioThread::Telemetry::monitorMode`. Drum-record-mode (looper's per-key
  keybed sampler) is NOT ported — see `docs/DECISIONS.md` ADR-012.
- the 8×5 grid → track/clip select + state display

## 3-bank fx control-surface (LOFI feature — src/control/apc_grid.h/.cpp, midi.cpp)
The 8-button control row above the pad grid is transpose / sample / drum-sample /
dub-fx / guitar-fx / lofi-fx / varispeed / varispeed. The first three and last two
are the existing, unchanged buttons (`onLiveEngageToggle`, `onSamplerBtn65/66Press`,
note70/71 speed-scrub). Positions 4-6 are 3 new, symmetric bank-select buttons —
**FULLY WIRED** in `ApcGrid`/`midi.cpp` (confirmed by reading both), unlike the
per-bank effect chains themselves (see the DSP-wiring status below).

- **dub-fx** = note 67, **guitar-fx** = note 68, **lofi-fx** = note 69 (channel 0
  only) — WITNESSED live on real APC Key25 hardware (originally assumed 87/88/89
  from an unverified "free note range" guess; real button presses, isolated one
  at a time and read from the live `[midi] note decoded` log, showed 67/68/69 —
  which also makes physical sense: they sit immediately before notes 70/71
  (the varispeed scrub buttons), i.e. these ARE "positions 4/5/6 of the row,
  left of the two varispeed controls" exactly as originally specified).
- Tap-to-select, radio-button style — no cycling, no hold-preview. Pressing one
  makes it the `activeBank()` immediately (`ApcGrid::onDubFxPress`/
  `onLofiFxPress`/`onGuitarFxPress`) and re-pushes every one of that bank's 7
  stored knob values into the shared Faust zones right away
  (`pushBankValuesToZones`) — otherwise switching banks would leave the audible
  effect showing the PREVIOUS bank's values until every knob was retouched,
  which reads as the switch silently failing.
- Selecting a bank starts a `bankFlashActive()` window (150ms, `kBankFlashMs`),
  cleared by `pollHolds` — a **transient** LED flash on selection, not a
  persistent "which bank is active" indicator.
- CC48/49/50/51/54/55/57 (the 7 physical fx knobs) are intercepted directly in
  `midi.cpp`, ahead of the flat `config/controls.conf` map — that flat map no
  longer binds any of these 7 targets at all (see its own comment block: a flat
  binding here would race `ApcGrid`'s bank-aware write with no defined winner).
  `ApcGrid::onFxKnobCC` writes the normalized `data2/127` value into the
  **currently active bank's own stored slot** for that knob position, AND into
  the shared Faust zone, so turning a knob still has the usual instant effect.

### Per-bank independent knob storage
Each bank stores its own value per physical knob position — switching banks
changes what the 7 knobs currently mean/show without touching any other bank's
stored values (`m_fxBankValues[bank][knob]`, seeded in `ApcGrid::bindAll`). The
knob→zone mapping (index-matched `kFxZoneNames`/`kFxKnobCcNumbers` in
apc_grid.cpp) is the same physical CC layout for every bank; only the Faust
zone identity attached to a bank's *stored value* changes what it does:

| CC | Faust zone | Dub bank meaning |
|---|---|---|
| 48 | `fx/reverb` | reverb amount |
| 49 | `fx/delay` | delay amount |
| 50 | `fx/time` | delay time |
| 51 | `fx/hp` | HP filter |
| 54 | `fx/lpres` | LP resonance |
| 55 | `fx/lp` | LP filter |
| 57 | `fx/pitch` | pitch |

(CC53/formant is handled separately by `onFormantCC`, unaffected by bank
switching — same as before this feature.) Guitar and LofiFx banks reuse this
same 7-slot array with their own defaults (0.0 = passthrough for every new
effect's "amount" control, `m_fxBankValues[1]`/`[2]` in apc_grid.h) but — see
below — nothing downstream of `ApcGrid` yet turns those stored values into
sound for those two banks.

### guitar-fx's dual gesture
guitar-fx is the one bank button with a second, independent gesture layered on
top of plain tap-to-select:
- **Plain tap** (press+release, no looper pad pressed in between) selects the
  Guitar bank exactly like dub-fx/lofi-fx. The bank-select actually fires on
  PRESS (not release) — same shape as the main pad grid's ARM/FINISH, which
  also commits on press while a separate flag suppresses only the matching
  release.
- **Hold guitar-fx + press a looper pad** instead toggles that looper's
  sidechain-pump SOURCE designation (`ApcGrid::onSidechainLooperToggle`) — the
  press is redirected entirely away from the normal ARM/FINISH pad dispatch
  (never touches `m_looperHeld`/hold-erase timing at all). Toggle, not set:
  press again while still held to remove it. **Multiple loopers can be
  sidechain sources simultaneously.** The designation persists after guitar-fx
  is released (not cleared on release), and auto-clears if that looper is
  erased (per-looper hold-erase) or a clear-all fires (`cmd/clearall`) — both
  paths confirmed by reading `apc_grid.cpp` (the erase and clear-all branches
  each explicitly reset `m_looperIsSidechainSource`).
- `looperIsSidechainSource(int)` is the read accessor. The ducking/pump signal
  itself IS now wired at the DSP level (see "Sidechain-pump DSP routing"
  below) — a designated source looper's own level telemetry drives a shared
  ducking envelope applied to every OTHER looper's output.

### Guitar/LofiFx bank DSP wiring — LANDED
`dsp/effects_runtime.dsp` now consumes the same 7 shared `fx/*` zones the Dub
bank always used, reinterpreted per-bank via a new 8th zone, `fx/bank`
(`nentry`, 0=Dub/1=Guitar/2=LofiFx, matching `FxBank`'s own enum values) —
`ApcGrid` writes this zone whenever `activeBank()` changes, alongside the
existing per-bank knob-value re-push.

- `guitarChain = flanger -> tremolo -> phaser`, reading `REVAMT->FLANGEAMT`,
  `DELAYAMT->TREMOLOAMT`, `TIME->BANKSPEED` (shared tremolo/phaser rate),
  `HPCUT->PHASERAMT`.
- `lofiFxChain = bitcrush -> vinyl -> flutter -> samplerate`, reading
  `REVAMT->BITCRUSHAMT`, `DELAYAMT->VINYLAMT`, `TIME->FLUTTERAMT`,
  `HPCUT->SRRAMT`.
- The 3 banks' fully-computed chains are blended by `fx/bank`, smoothed via
  `si.smoo` (same idiom/time-constant class as `aloop.dsp`'s existing
  `MONITORFOLD`/`GLITCHFOLD` ramps) into a continuous triangular crossfade —
  not a hard switch — so a bank change mid-performance does not click/pop.
  Verified: `fx/bank=0.5` produces a genuine partial blend, not a
  discontinuity.
- Verified byte-exact passthrough at each bank's own true stored defaults
  (Dub: unchanged from before this feature; Guitar: `TIME=0.5`, others 0;
  LofiFx: `TIME=0.0`, others 0 — note the LofiFx default for that shared slot
  deliberately differs from Dub's) via the CLI DSP harness.
- `effects/home/faust/compressor.dsp` (the guitar bank's "compress" bottom-row
  control) exists as a standalone file but is **NOT YET wired** into
  `guitarChain` — it has no assigned slot in the current 7-knob array
  (`apc_grid.h`'s own comment: "compress lives outside this 7-knob array"), so
  it needs its own zone/CC assignment before it can be reached from the
  control surface at all. Its internal gain-staging (byte-exact passthrough
  at 0, monotonic loudness increase with no clipping across the full dial
  sweep) was still being iterated on as of this write — treat its behavior as
  unconfirmed until both the internal math and its chain wiring land.

### Sidechain-pump DSP routing — LANDED
`dsp/loop.dsp`'s `oneLooper` gained a 7th `process()`-level signal input,
`sidechainEnv` — broadcast identically to all 20 loopers via the same
par()-duplication-avoidance technique as `masterPhase`/`masterLen`/
`effSpeed`/`clearAll` (confirmed safe scaling from 6 to 7 signal inputs via a
standalone Faust repro this session, no zone duplication, correct per-
instance broadcast) — plus a per-looper `sidechainsrc` checkbox (safe as a
per-instance UI control, unlike the shared broadcast signal). `audio_thread.cpp`
computes `sidechainEnv` each block as the max/peak of every currently-
source-designated looper's own `level` telemetry (one-block-lag, same
staleness class already accepted for `prevFiltIn`/`prevLoopSum`'s own
one-block-lag folds). Ducking: `out = loopSig * playN * volN * duckGain`,
where `duckGain = 1.0 - sidechainEnv*(1.0-isSourceN)` — a source looper's own
`isSourceN` gates the duck factor back to 1 so it is never ducked by its own
signal. Verified via a standalone Faust repro isolating the exact formula: a
non-source looper's RMS dropped under a full-envelope step while a source
looper's RMS stayed byte-identical to its unducked value under the identical
step.

## Link / transport
Ableton Link tempo/phase drives the loop grid; loop record start quantizes to the
Link phase; the first loop can propose the tempo to the session (ported logic).

Every one of these behaviors is the *ported looper logic* — same code, same
result. The mapping table above is the same one dubfx verified against the real
control plane (param_mapping.md).
