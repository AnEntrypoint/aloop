# Faust compiler-flag A/B harness — DawDreamer, real libfaust, real Linux LLVM backend

AGENTS.md's "Faust DSP compiler optimization pass" section documents that
`-fm def` (Faust's fast-math approximations for sin/cos/tan/atan/exp/log/pow/
sqrt) was evaluated and explicitly NOT shipped, blocked by a toolchain
artifact: the old synthetic A/B harness was Windows/MinGW-only, and MinGW g++
rejects the linkage `faust/dsp/fastmath.cpp` needs. That entry calls for
"either a Linux-side build of the same harness or genuine on-device
verification" before `-fm def` can be shipped with the same rigor `-mapp` (a
narrower, already-verified flag) received.

`ab_fm_def.py` is that Linux-side harness. It uses
[DawDreamer](https://github.com/DBraun/DawDreamer)'s `FaustProcessor`, which
bundles a real `libfaust` (LLVM JIT backend) and exposes `compile_flags`
directly, to compile the same real DSP source twice — once with aloop's
shipped flags, once with those flags plus `-fm def` — and diff the rendered
output sample-for-sample. Each render runs in its own subprocess
(`_render_worker.py`) so a crash in one case is captured as data instead of
killing the whole run.

`-nvi` (drop the `virtual` keyword from generated C++) is omitted from the
"shipped flags" baseline here: it's a `cpp`/`ocpp`-backend-only class-layout
flag with no effect on the LLVM JIT backend `FaustProcessor` uses, and no
effect on the numeric question `-fm def` raises either way.

## Result: `-fm def` is NOT safe to ship — it segfaults, not just diverges numerically

Every one of the 87 real-usage-pattern cases below **crashes the render with
SIGSEGV** the instant `-fm def`'s generated code actually calls one of the
affected functions. This is a stronger, more direct finding than "the numbers
differ" — the previous open question in AGENTS.md was whether `-fm def`'s
*approximation error* was small enough to ship; the real answer is that this
flag doesn't run at all against DawDreamer's bundled `libfaustwithllvm.a`
(Faust 2.81.10, the official Linux release binary).

Root cause, confirmed by direct inspection: `-fm def` generates calls to
`fast_tanf`/`fast_powf`/`fast_expf`/etc. These functions live in
`faust/dsp/fastmath.cpp`, which is a Faust **architecture file** — meant to be
compiled alongside Faust-generated C++ text output (the `-lang cpp` backend
aloop's own CI uses), not something baked into `libfaust` itself.
`nm thirdparty/libfaust/ubuntu-x86_64/Release/lib/libfaustwithllvm.a` has no
`fast_*` symbols at all. The LLVM JIT backend `FaustProcessor` uses therefore
emits calls to symbols nothing resolves; the crash happens at first execution
(inside `engine.render()`), not at `compile()` — Faust's LLVM factory happily
reports success, matching this project's own "never trust compiles-clean as
proof of runtime safety for a numeric-approximation flag" lesson (see the
`-mapp` history in AGENTS.md) with an even sharper example: this one doesn't
even need real hardware or real audio content to fail, it fails on any input.

This does **not** by itself prove `-fm def` would crash on aloop's actual
`-lang cpp` → g++ → musl/aarch64 pipeline, since that pipeline compiles
against real object code rather than a JIT-resolved symbol table — but it
firmly rules out "verify it here first" as a safe path, and independently
confirms the flag needs its own real link-time proof (does `fastmath.cpp` get
compiled and linked into the real `aloop` binary at all if `-fm def` is ever
added to `build-binary.yml`'s `faust -lang cpp` invocation? Nothing in the
current build does this) before ever being added to a real invocation site.

Confirmed empirically (see "Why the sweep needed runtime hsliders, not
compile-time constants" below): the crash requires the transcendental
function to be a genuine runtime call. A DSP where the transcendental's input
happens to be a compile-time constant survives fine under `-fm def` — because
Faust constant-folds the whole expression away at compile time and the
`fast_*` call is never emitted at all, not because the flag is safe.

## Coverage

Only the files/formulas in `effects/home/faust/` that actually call a
`-fm def`-affected transcendental function are exercised:

- `filters.dsp` (`tan()`, `pow()` in `hpG`/`lpG`) — swept across the real
  `HPCUT`/`LPCUT`/`LPRES` ranges `effects_runtime.dsp` exposes as live
  hsliders (see below for why this must be an hslider, not a constant).
- `compressor.dsp` (`exp()`, `log10()`, `pow()`) — swept across its real
  runtime `COMPRESSAMT` hslider range.
- `pitch.dsp`'s `pow(2.0, SEMIS/12.0)` formula, isolated into a standalone
  snippet with `SEMIS` as a real hslider. The file itself cannot be compiled
  here: `pitchTick` is a Faust `ffunction` bound to `pitch_ffi.h`'s real
  `dubfx_pitch_tick` C++ symbol, which DawDreamer's JIT compile of a bare
  string has nothing to link against. This is fine for `-fm def`'s
  purposes — the `pow()` call is the only affected operation in that file.

`reverb.dsp` and `delay.dsp` use no `-fm def`-affected functions and are not
included.

### Why the sweep needed runtime hsliders, not compile-time constants

The standalone `filters.dsp`/`pitch.dsp` files pin `HPCUT`/`LPCUT`/`LPRES`/
`SEMIS` as literal Faust constants (`HPCUT = 0.0;`), matching how they're
compiled standalone. A first version of this harness substituted new literal
values in directly (`HPCUT = 0.5;`) — every case came back bit-identical
between flag sets, which looked like good news until closer inspection showed
`-fm def` never actually ran: with a literal-constant input, Faust
constant-folds `tan(...)`/`pow(...)` away at compile time, so no `fast_*` call
is ever emitted either way. In real production, `effects_runtime.dsp` wires
these in as genuine runtime `hslider`s
(`component("effects/home/faust/filters.dsp")[HPCUT=HPCUT; ...]`), so the
harness now declares them the same way (hslider, not literal, with the swept
value as the default) to match what actually ships. That's what turned "0/87
differences" into "87/87 crashes" — the literal-constant version was silently
testing the wrong regime.

## Running

```
pip install -e /path/to/DawDreamer   # or otherwise make `import dawdreamer` resolve
python3 test/faust-flags/ab_fm_def.py
```

Writes `results.json` (every case's crash/diff status) next to the script and
prints a summary table.
