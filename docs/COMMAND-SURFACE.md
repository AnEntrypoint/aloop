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
**FULLY WIRED** in `ApcGrid`/`midi.cpp`, and (per the Core-3 redesign below)
so is every bank's own effect chain — all 3 banks are simultaneously live.

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
stored values (`m_fxBankValues[bank][knob]`, seeded in `ApcGrid::bindAll`).

**REDESIGN (Core-3 move, superseding an earlier in-Faust 3-bank crossfade
design — see "Guitar/LofiFx bank DSP wiring" below for why that was
abandoned):** each bank's 7 knob positions now have their OWN permanent
target (`FxKnobTarget`/`kDubTargets`/`kGuitarTargets`/`kLofiFxTargets` in
apc_grid.cpp) — Dub's targets are unchanged Core-1 Faust zones; Guitar/LofiFx
targets are either an `fx2/*` LV2 control port on the new permanent Core-3
`guitar_lofi_fx.dsp` bundle, or (for attack/release/granulator) a native
`Sampler` setter call. All 3 banks' targets are simultaneously, permanently
live — switching banks is now a pure UI/state change (which bank's stored
values the physical knobs currently show/edit), never a DSP-routing change:

| CC | Dub target | Guitar target | LofiFx target |
|---|---|---|---|
| 48 | `fx/reverb` (Faust) | `fx2/FLANGEAMT` (LV2) | `fx2/BITCRUSHAMT` (LV2) |
| 49 | `fx/delay` (Faust) | `fx2/TREMOLOAMT` (LV2) | `fx2/VINYLAMT` (LV2) |
| 50 | `fx/time` (Faust) | `fx2/BANKSPEED` (LV2) | `fx2/FLUTTERAMT` (LV2) |
| 51 | `fx/hp` (Faust) | `fx2/PHASERAMT` (LV2) | `fx2/SRRAMT` (LV2) |
| 54 | `fx/lpres` (Faust) | attack ms (Sampler) | grain size ms (Sampler) |
| 55 | `fx/lp` (Faust) | release ms (Sampler) | grain density Hz (Sampler) |
| 57 | `fx/pitch` (Faust) | `fx2/COMPRESSAMT` (LV2) | scan rate (Sampler) |

(CC53/formant is handled separately by `onFormantCC`, unaffected by bank
switching — same as before this feature.) Granulator's pitch-spray and
position-jitter controls stay fixed at their passthrough defaults — only 7
physical knobs exist, so lofi-fx's bottom row picked 3 of the sampler's 5
granulator params (grain size/density/scan rate), matching guitar bank's 4+3
knob shape exactly (both decisions made explicitly, not silently guessed).

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

### Guitar/LofiFx bank DSP wiring — LANDED (Core-3 redesign)
**Superseded design, kept here for history:** an earlier iteration of this
feature made `dsp/effects_runtime.dsp` compute all 3 banks' effect chains
every block and blend them via an 8th `fx/bank` Faust zone (a continuous
`si.smoo`-smoothed triangular crossfade). **WITNESSED live on a real Pi 4:**
Faust has no runtime branching — `select2`/crossfade math chooses among
ALREADY-COMPUTED signals, it never skips computing the other banks — so this
design computed guitar+lofi-fx's 8 effects on Core 1 continuously regardless
of which bank was "selected," a real ~7 percentage-point `core_busy`
regression (23%→30%) that caused audible continuous audio dropouts. Confirmed
via a real CI-built pre-feature baseline (zero xruns) vs the crossfade build
(continuous dropouts), and via a second isolated A/B test reverting only that
file.

**Current, landed design:** `dsp/effects_runtime.dsp` is restored to its
pre-feature dub-only chain (`pitchStage : delayStage : reverbStage :
microStage <: (filterStage, _)`) — Core 1's cost is back to its original
budget. Guitar+lofi-fx's 8 effects moved to their own standalone Faust
program, `effects/home/faust/guitar_lofi_fx.dsp`, compiled by CI
(`.github/workflows/build-lv2.yml`'s `guitar-lofi-fx-lv2` job) into its own
LV2 bundle under `/effects/home`, and hosted in-process on Core 3 (previously
fully idle) via a SECOND, PERMANENT `Lv2Host` instance (`audio_thread.cpp`'s
`homeFx`, distinct from the existing swappable `userFx`) — running in series
after Core 1's full dub-chain output, every block, unconditionally:
`homeFx.process(fout.data(), N); userFx.process(fout.data(), N);`. Per the
confirmed design, this is a genuinely free capacity win, not a
redistribution: all 3 banks' effects are now simultaneously, permanently
active (never gated by bank selection), matching "Both guitar and lofi-fx
always stack together with dub."

- `guitar_lofi_fx.dsp`'s chain: `flanger -> tremolo -> phaser -> compressor ->
  bitcrush -> vinyl -> flutter -> samplerate`, each stage reading its own
  permanent `fx2/*` zone directly (no shared-zone reinterpretation).
- `Lv2Host::setControl(symbol, value)` (new) pushes a live control value into
  every loaded plugin's matching LV2 port by symbol — `ApcGrid::onFxKnobCC`
  calls this for every Guitar/LofiFx knob whose target is `FxKnobKind::
  Lv2Control` (see the per-bank target table above), the same way it calls
  `ParamStore::setByName` for Dub's Faust zones.
- `effects/home/faust/compressor.dsp` (guitar bank's "compress" control) is
  now wired to CC57/`fx2/COMPRESSAMT`, filling the 7th guitar-bank knob slot
  the earlier design left unassigned.
- Verified via `dsp_cli`: `guitar_lofi_fx.dsp` compiles clean, exposes
  exactly the 9 expected `fx2/*` zones, byte-exact passthrough at all
  defaults, and audibly engages when driven (flange+bitcrush test case).
  CI-built and confirmed green (`guitar-lofi-fx-lv2` artifact uploads
  alongside the existing `home-fx-lv2` job).

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
