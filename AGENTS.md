# aloop — agent debugging caveats

Hard-won gotchas from live debugging on the real Pi 4 hardware (192.168.137.100,
root/aloop). Read this before touching the device or its build/deploy pipeline —
every entry here cost real time to discover once; don't rediscover it.

## Never add audio-path latency to fix anything

The pre-LOFI baseline (commit `4cb6587`) was measured **100% hitch-free** on
real hardware. The existing ~7ms block latency is already considered too much
by the user — it must never grow, not even temporarily, not even to work
around an unrelated bug. If a fix seems to require a bigger ALSA
buffer/period, more block lag, or any added buffering stage, stop and ask
before landing it. Any audio glitch found after this baseline is a real
regression to root-cause, not a hardware limit to negotiate around.

## The REBOOT:<token> UDP listener lives INSIDE the aloop process

`config/aloop.conf`'s `[remote] token=` enables a `udp/4446` listener
(`src/control/remote_control.cpp`) that accepts `REBOOT:<token>` and reboots
the Pi — but this listener is part of the `aloop` binary itself. **If `aloop`
has crashed, nothing is listening, and `image/aloop-reboot.js` silently does
nothing at all** (no error, no timeout — the UDP packet just goes nowhere).
`/etc/init.d/aloop`'s `respawn_max=0` means OpenRC will NOT restart a crashed
`aloop` on its own either, so a crashed device stays crashed indefinitely
unless something else reboots it.

**Symptom this caused, live**: several consecutive `aloop-reboot.js` calls
appeared to "not pick up" a freshly-deployed binary — in reality the device
never rebooted at all; it kept running the last binary that successfully
booted, for over 20 minutes across multiple deploy attempts, while every
`REBOOT:<token>` packet vanished into a dead process.

**Always verify a reboot actually happened before trusting any "device state"
observation**: check `cat /proc/uptime` (should be small/recent) and
`md5sum /opt/aloop/aloop` against the binary you just deployed. Do this
BEFORE reading logs or drawing conclusions from device state — a stale
device silently produces stale, misleading data that looks like a fresh
test.

**If `rc-service aloop status` shows `crashed`, `REBOOT:<token>` cannot work.**
Use a real SSH-triggered reboot instead: `node ssh-exec.js 192.168.137.100
"reboot"` (see the JS SSH client below). Only use the UDP REBOOT path once
`aloop` is confirmed actually running.

## SSH access: use the JS client, never Windows ssh.exe or sshpass

Password auth (root/aloop), not key auth. The user has explicitly rejected
both bare Windows `ssh.exe` (pops up asking for a password) and an
sshpass-wrapping approach — use a pure-JS `ssh2`-based client instead
(`npm install ssh2` in the scratchpad, then a small script that does
`new Client().connect({host, port:22, username:'root', password:'aloop', ...})`).
A fresh netboot generates a new host key every boot, which breaks raw
`ssh`/known_hosts but doesn't affect `ssh2` (it doesn't consult
`~/.ssh/known_hosts` the same way).

## The device runs Alpine/musl/aarch64 — glibc/x86_64 build artifacts silently fail to load

`build-lv2.yml`'s original `faust2lv2 <file>.dsp` step ran on bare
`ubuntu-latest`, compiling the `.so` with the HOST's own g++ (glibc/x86_64).
The device (Alpine, aarch64) can dlopen the resulting `.so` with no
bundle-discovery error, but it fails at actual load time:
`Error relocating .../foo.so: unsupported relocation type 7`. This meant
**every** home-FX LV2 effect (not just anything added later) had never
actually reached the device's audio path, ever — CI staying green only ever
meant "the x86_64 build compiled," never "the plugin runs on target."

Fix pattern (see `.github/workflows/build-lv2.yml`): `faust2lv2`'s own script
(`which faust2lv2`) cleanly separates concerns — `faust -i -a lv2.cpp ...`
emits a self-contained `.cpp` (verified: only libc/libstdc++/lv2/boost
includes, no Faust dependency), a `$HOST_CXX` compile+run of that same `.cpp`
emits the plugin's `.ttl` metadata (runs on the host only, never touches
target arch/libc), and only the FINAL `-shared .so` link needs to target the
real device. Cross-compile that one step inside a real Alpine aarch64
container via `docker/setup-qemu-action` + `docker run --platform linux/arm64
alpine:3.20`, matching `build-binary.yml`'s own already-proven pattern.
Verify the result is genuinely target arch:
`objdump -p foo.so | grep NEEDED` should show `libc.musl-aarch64.so.1`, never
`libc.so.6`.

**Passing `CPPFLAGS` (which contains escaped quotes, e.g.
`-DPLUGIN_URI=\"...\"`) into a nested `docker run ... sh -c "..."` string via
shell interpolation loses the escapes across the nested-shell boundary** —
`PLUGIN_URI` expands to bare unquoted text and the compiler tries to parse
the URL as code (`'https' was not declared in this scope`). Pass such values
via `docker run -e VAR="$VAR"` instead of string-interpolating them into the
outer script.

## `actions/upload-artifact@v4`'s `path:` wildcard-vs-literal behavior

`path: effects/home/*.lv2` (a wildcard match) zips the matched directory
WITH its own basename preserved inside the archive. `path:
effects/home/guitar_lofi_fx.lv2` (a literal, non-wildcard single-directory
path) zips that directory's CONTENTS flattened at the zip root instead,
silently dropping the `.lv2/` wrapper. A CI job that changes from wildcard to
literal path for no functional reason will silently break every downstream
consumer that expects the `.lv2/` directory to exist inside the artifact.
Always use the wildcard form for LV2 bundle artifacts.

## LV2 hosting: never pass a bare `nullptr` for the features array

`Lv2Host::instantiate()` (`src/host/lv2_host.cpp`) must pass a real,
NULL-TERMINATED `LV2_Feature* const*` array to `d->instantiate(...)` — never
a bare `nullptr`. Faust's generated `lv2.cpp` architecture does
`for (int i = 0; features[i]; i++)` with no null-check on `features` itself,
so a bare `nullptr` deref's immediately at `features[0]` — genuine SIGSEGV
(exit 139), reproducible on every single plugin load. This bug existed since
before this session but never surfaced because no LV2 plugin had ever
successfully dlopen()'d until the musl/aarch64 CI fix above landed — so
fixing the CI packaging bug is what turned a previously-silent bug into a
crash. Use `static const LV2_Feature* const kNoFeatures[] = { nullptr };` (a
real, valid, empty-but-terminated array) instead.

Also wrap `instantiate()`/`activate()` in the same sigsetjmp crash-isolation
watchdog `runOne()` already uses (ADR-002) — a bad plugin crashing during
LOAD is just as much "untrusted code" as one crashing during `run()`, and
taking the whole process down during load is strictly worse than degrading
gracefully.

## `core.autocrlf=true` on this Windows clone silently corrupts shell scripts

Editing/re-checking-out any `.sh`/`.start` file on this Windows machine can
silently convert its line endings to CRLF. Alpine's busybox ash chokes on
`#!/bin/sh\r` (breaks the shebang lookup) and every line's trailing `\r`
merges into the next token (`illegal option -`, `: not found`). This broke
`kernel/rt-tune.sh`'s boot-time CPU-governor pin with **zero visible error
anywhere in the pipeline** — CI stayed green (nothing there runs the
script), the packaging step (`image/lib-boot-tree.sh`) just copies bytes —
the only symptom was the real device silently still running `schedutil`
instead of `performance`.

Fixed with a repo-level `.gitattributes` forcing `eol=lf` on `*.sh *.start
*.conf *.yml *.yaml Makefile cmdline.txt config.txt usercfg.txt` (already
committed). If a shell script is ever edited and behaves strangely on the
device despite looking correct in the editor, check for CRLF first:
`file path/to/script.sh` should say "with CRLF line terminators" if
corrupted; fix via `rm path/to/script.sh && git checkout -- path/to/script.sh`
(the checkout re-applies the now-correct `.gitattributes` rule).

## The netboot self-update pipeline: two different rebuild paths, easy to confuse

- **Automatic path**: `image/serve-netboot-win.js` (run elevated, needs
  `GITHUB_TOKEN`/`gh auth token` and `PI_TOKEN` env vars) polls
  `build-binary.yml`/`build-lv2.yml`'s latest green run on `main` every 30s,
  downloads BOTH artifacts into `.netboot-update-work/{bin,lv2}`, and calls
  `image/build-netboot.sh` itself when the combined SHA changes. It tracks
  state in `.netboot-update-sha` (format `<binSha>:<lv2Sha>`) — if this file's
  content already matches the latest state, the poll loop does nothing, ever,
  even if `.netboot-serve/`'s actual content is stale/wrong/manually
  overwritten.
- **Manual path** (used for A/B testing specific commits, e.g. bisecting a
  regression): `ALOOP_BIN=<path> LV2_DIR=<path> OUT=.netboot-serve
  NETBOOT_SERVER=192.168.137.1 bash image/build-netboot.sh` rebuilds
  `.netboot-serve/` directly from arbitrary local artifacts, bypassing the
  poll loop entirely.

**Always verify the ACTUAL deployed binary/bundle checksum matches what you
intended, immediately after every manual rebuild, BEFORE rebooting** —
`tar -xzf .netboot-serve/aloop.apkovl.tar.gz -C <fresh-empty-dir>
./opt/aloop/aloop && md5sum <fresh-empty-dir>/opt/aloop/aloop` vs the source
binary. Extracting to stdout (`-O`) or reusing a not-freshly-emptied
extraction directory can silently compare against stale leftover files from
an earlier extraction and give a false-positive match. This was the actual
cause of several confusing "wrong binary keeps getting deployed" incidents
in one session — the rebuild and the checksum were both real, but a stale
comparison target made a correct rebuild look like it had failed, OR (the
more dangerous direction) masked a genuinely stale device that never
rebooted at all (see the REBOOT-listener caveat above — always cross-check
`/proc/uptime` too, not just the checksum, since a checksum match against
`.netboot-serve/` proves the SERVER state, not that the DEVICE actually
picked it up).

When manually testing an old/historical commit for bisection purposes, that
commit's binary won't have current fixes — expect it to crash if it predates
the nullptr-features fix above and any LV2 bundle is present in
`/effects/home` or `/effects/user`; this is expected, not a new bug.

## Diagnosing periodic audio stalls: always add wall-clock timestamps, not just magnitudes

A gap-logging line that only fires when the gap exceeds some threshold
(`[diag-gap] readi gap=X ms`) has line-count density that is NOT a reliable
proxy for real elapsed time — quiet stretches between events are invisible
in the log, and a burst of activity can look identical in line-count to a
long quiet stretch with one big spike. Always log a wall-clock timestamp
(`clock_gettime(CLOCK_MONOTONIC, ...)`, printed as `t=<sec>.<ms>`) alongside
the magnitude — this is what let a "seems like it's happening often" symptom
resolve into a hard, provable "fires at almost exactly a 1.000-second
period" measurement.

## Real hardware over asking the user to reproduce input

Per the `gm` skill's own standing rule: prefer byte-level MIDI injection
(`tcp/9401`, "synthetic MIDI bytes for scripted reproduction" —
`src/control/midi.cpp`) or SSH-based log/state inspection over asking the
user to physically press buttons/turn knobs, whenever the bug can be
reproduced that way. Reserve `AskUserQuestion` for the physical step only
once a byte-level substitute has genuinely been attempted and either proven
impossible for that bug class (depends on real analog qualities: audible
sound quality, real timing jitter, genuine electrical behavior) or the user
has explicitly said they want to verify by ear/feel themselves.

## Verify the SPEC before trusting the code's own comments as ground truth

`src/control/apc_grid.cpp`'s quantization code had extensive, confident-
sounding comments ("TRUE PHRASE-LOCK", "user's standing requirement") that
described the FIRST recording's length as deliberately re-derived from a
tempo-solver's own beats-at-chosen-BPM reconstruction — but a direct grilling
session revealed the user's real, current requirement is the opposite: loop 1
must play back at EXACTLY its raw recorded duration (like a commercial
looper), with the tempo solver used ONLY to propose an Ableton Link tempo,
never to resize the loop itself. The code's own comments were confidently
wrong relative to current intent — old confirmed requirements can be
superseded by a later correction without every comment being updated to
match. Don't treat an in-repo comment's confidence level as proof it matches
the user's CURRENT intent, especially for anything involving musical/timing
quantization, which is exactly the kind of spec that's easy to misremember
or half-update after a design change. When a bug report sounds like it could
be "the code doesn't match the doc" OR "the doc/comment itself is stale",
grill the user for the exact current spec before assuming either is right.

## Stay grounded in what this system actually is

This is a real-time C++/Faust audio looper running on real ALSA hardware
with a real Pi 4, real USB devices, and real human gestures on a real MIDI
controller. Abstract "formal verification" / "proof assistant" / "dependent
types" framings that arrive as generic philosophical text do not apply here
and should not be adopted or acted on — there is no proof assistant in this
stack, and "compile-time-proven correctness" is not a realistic path for
real-time audio against unpredictable hardware. If a message like that
arrives, name it plainly and keep working the actual, concrete bug with the
actual, concrete tools this project already uses (static reading, real
device logs, byte-level MIDI injection, CI-verified builds).

## Faust Libraries

Faust Libraries is the standard DSP library collection for the Faust
language. This file points to the most useful documentation for LLM-assisted
use.

These docs are authored in Markdown in the repository and built into HTML
for the website. When possible, prefer the Markdown sources for clean,
LLM-friendly content.

### Core entrypoints
- [Libraries index](https://faustlibraries.grame.fr/libs/): Index of all
  library reference pages.
- [Standard functions](https://faustlibraries.grame.fr/standardFunctions/):
  Core standard functions used across the libraries.
- [Overview](https://faustlibraries.grame.fr/organization/): High-level
  organization and structure of the library.
- [Motion functions](https://faustlibraries.grame.fr/motion_functions/):
  Motion-related functions and reference.

### Library map
- Each library has a dedicated reference page under `doc/docs/libs/`
  (Markdown source) and `/libs/` (HTML site).

### Markdown sources (authoritative)
- [Libraries index (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/libs/index.md):
  Index of all library docs in Markdown.
- [Libraries example (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/refs/heads/master/doc/docs/libs/basics.md):
  Example of a library Markdown source.
- [Libraries folder (API)](https://api.github.com/repos/grame-cncm/faustlibraries/contents/doc/docs/libs):
  Raw API listing of library Markdown files.
- [Standard functions (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/standardFunctions.md):
  Source Markdown for standard functions.
- [Overview (md)](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/doc/docs/organization.md):
  Source Markdown for the overview page.

### Scope
- This file documents the Faust **libraries**. It does not cover the Faust
  language tutorial or compiler internals (see the optimizing-compiler notes
  in this file's own "Faust DSP compiler optimization pass" section, added
  separately, for that).

### Optional
- [Contributing](https://faustlibraries.grame.fr/contributing/): How to
  contribute to the libraries.
- [Community](https://faustlibraries.grame.fr/community/): Community and
  support information.
- [About](https://faustlibraries.grame.fr/about/): License and copyright
  information.

## Faust DSP compiler optimization pass — what shipped, what was rejected

Following [faustdoc.grame.fr/manual/optimizing/](https://faustdoc.grame.fr/manual/optimizing/),
a pass was made to apply every safe, behavior-preserving optimization from
Faust's own optimizing-compiler manual plus the native C++ hot path. What
shipped:

- **`-vec -fun -dfs -vs 32 -nvi`** added to every real `faust` invocation
  (`build-local.sh`, `.github/workflows/build-binary.yml`'s `loop.cpp`
  codegen, both jobs in `.github/workflows/build-lv2.yml`). Pure
  codegen-strategy flags — vectorized codegen, function inlining,
  depth-first scheduling, no-virtual C++ backend (Faust's own docs call
  `-nvi` "especially useful in embedded devices context", directly on point
  for the Pi 4). Same `.dsp` source, same signal graph — only how the
  compiler schedules/inlines it changes.
- **`Lv2Host`'s `LV2_Descriptor*` is now cached** at `instantiate()` time
  (`Lv2Plugin::descriptor`) instead of being re-resolved via `dlsym` +
  URI-matching linear scan on every single `runOne()` call — that call runs
  on the real-time audio block path (Core 1 home-fx + Core 3 user-fx) every
  block, so eliminating a repeated symbol-scan from the hot path was a safe,
  zero-risk win.

What was evaluated and explicitly REJECTED (not silently skipped):

- **`ba.tabulate`-ing `filters.dsp`'s `pow(1000.0, cutoff)` (SVF
  cutoff-to-Hz) and `pitch.dsp`'s `pow(2.0, SEMIS/12.0)` (pitch-shift ratio)**:
  both files' own headers explicitly claim an *exact port*/*bit-identical*
  match to the original hardware DSP. `ba.tabulate` (even its `.cub`
  cubic-interpolation mode) is inherently an approximation — introducing it
  would trade a real, explicitly-claimed fidelity guarantee for a CPU saving
  that, in `pitch.dsp`'s case, is negligible anyway (the stage's dominant
  per-sample cost is the `dubfx_pitch_tick` ffunction call into the real C++
  pitch engine, not the one `pow()` feeding it). Any future push to tabulate
  either of these must first prove the tabulation error is smaller than the
  hardware-parity tolerance already established for that file, not just
  benchmark the CPU win in isolation.
- **Splitting the home Faust stack (`aloop.dsp`/`loop.dsp`/
  `effects_runtime.dsp`) or the Core-3 guitar+lofi-fx bundle into multiple
  separate per-effect LV2 bundles** "for performance/modularity": this would
  multiply per-plugin dispatch overhead (`findDescriptor`/`connect_port`/
  `instantiate` once more per split-out effect) in the exact code path
  already under live suspicion for the still-unresolved ~1Hz stall (see
  "Diagnosing periodic audio stalls" above), directly risking the
  never-add-latency constraint, and gives up the proven single-Faust-
  compile-unit maintainability the home stack was deliberately designed
  around ("change a knob mapping or a stage in Faust, rebuild, done" —
  `aloop.dsp`'s own top-of-file comment). Do not re-attempt this without
  re-deriving the same tradeoff from scratch.
- **Faust's own internal multicore/`-omp`/`-sch` work-stealing scheduler**:
  not applicable here. aloop already manually pins one Faust program per
  physical core via `pthread_setaffinity_np` (home stack on Core 1,
  guitar+lofi-fx on Core 3) — a coarser-grained, already-proven
  parallelization strategy. Layering Faust's own internal scheduler on top
  would fight that existing pinning architecture rather than complement it.
  (Also: this specific concern turned out not to be documented on the
  `optimizing/` manual page at all — it's covered, if anywhere, by a
  different part of Faust's docs not consulted this pass.)

What still needs real numeric verification before being considered fully
landed (evaluate-only rows, CI-buildable but not yet proven safe):

- **`-mcd`/`-dlt` (delay-line threshold tuning)** for `loop.dsp`'s 20×
  `MAXLEN=48000*60` rwtable rings vs. `delay.dsp`/`reverb.dsp`'s much
  smaller comb/tape delay lines — needs a real CPU/size comparison, not just
  reading the docs.
- **`-ct 0`** (disable Faust's table range-checking, on by default since
  Faust v2.53.4) for every `rwtable` in the hot path — every index in this
  codebase is already software-bounded by existing modulo/clamp logic
  (`writeIdx`/`readIdx0`/`readIdx1` all wrapped mod `wrapLen`, microrepeat's
  `sliceLen`-bounded `rpos`/`wpos`), so the extra check is *probably*
  redundant — but "probably" isn't enough to disable a safety check;
  needs an explicit per-table proof, not an assumption.
- **`-fm`/`-mapp`** (fast-math / experimental floor-ceil-fmod replacements):
  treated as HIGH RISK by default given this codebase's hard-won history of
  subtle Faust arithmetic bugs (`writeIdx`/`wrapLen` mutual-recursion,
  `armEdge`/`finishEdge` same-instant-cycle issues — see `dsp/loop.dsp`'s
  own extensive comments). Requires an explicit A/B numerical comparison
  (matching `compressor.dsp`'s own established raw-float-probe verification
  pattern) proving no drift in the floor/int/modulo-heavy `loop.dsp`
  arithmetic before ever shipping — do not ship on the strength of the docs
  alone.
- **Parameter-smoothing order** (`effects_runtime.dsp`'s `filterStage`/
  `delayStage`/`reverbStage`/`pitchStage` all take raw `hslider` values
  straight into `pow()`/`exp()`-bearing math with no `si.smoo` upstream):
  the Faust manual's documented pattern is to smooth *after* a costly
  conversion, at control rate. Whether this codebase's current
  no-smoothing design is deliberate (matching exact hardware knob-response
  timing) or an oversight has not yet been determined — needs listening/
  measurement, not just a docs-driven guess, before changing knob feel.
