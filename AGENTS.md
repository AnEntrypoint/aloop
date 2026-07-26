# aloop — agent debugging caveats

Hard-won gotchas from live debugging on the real Pi 4 hardware (192.168.137.100,
root/aloop). Read this before touching the device or its build/deploy pipeline —
every entry here cost real time to discover once; don't rediscover it.

## No comments in code, ever — self-explanatory code replaces them

Same absolute policy as `../gm`'s own AGENTS.md. A name, a function boundary,
an extracted variable, or a small type IS the explanation — prefer
renaming/restructuring over annotating, every time the urge to comment
appears. No inline, block, or doc comments anywhere (C++, Faust `.dsp`,
JS, shell, YAML, config). A multi-line or paragraph-long comment is the
same violation at higher volume, not a lesser one — explaining a "why" is
not an exemption; the urge to explain is itself the signal that a name or
structure is doing too little work, so restructure instead of narrating
around the gap.

Hardware quirks, historical root-causes, design-decision rationale, and
any fact that genuinely needs to survive across sessions belongs in THIS
file (AGENTS.md) or `.wfgy/lessons.md`, never inline next to the code —
this file is exactly where hardware quirks and memorables go.

A comment encountered anywhere — pre-existing, another session's, a
vendored copy — is converted to self-explanatory code the moment it's
seen, same turn, not left for a later cleanup pass: read it, understand
what it was compensating for, fix that root cause so the comment's
content becomes redundant, then delete it. "Already there, not part of
this task" is not an exemption — one sighting spawns a full-tree sweep of
that file (and, when time allows, nearby files sharing the same pattern).

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

**The automatic path's SHA-tracking is blind to changes in the packaging
scripts themselves** (`image/lib-boot-tree.sh`, `image/build-netboot.sh`) —
it only compares `build-binary.yml`/`build-lv2.yml`'s own latest green-run
SHAs, and neither workflow lists `image/**` in its trigger `paths:`.
WITNESSED live: a real fix to `lib-boot-tree.sh` (excluding a
duplicate-DSP `.lv2` bundle from what gets packaged — see the
`aloop.lv2` entry below) was committed and pushed, but the auto-updater's
`.netboot-update-sha` never changed (neither workflow re-ran), so
`.netboot-serve/`'s actual packaged content silently stayed on the OLD,
unfixed packaging indefinitely — a device that rebooted from netboot
picked the stale image back up, undoing a fix that had already been
verified live via manual on-device patching minutes earlier. Any change to
the packaging scripts themselves (not just `dsp/`/`effects/`/`src/dsp/`
content) requires a **manual** `.netboot-serve/` rebuild
(`ALOOP_BIN=<path> LV2_DIR=<path> OUT=.netboot-serve
NETBOOT_SERVER=192.168.137.1 bash image/build-netboot.sh`, using
already-downloaded `.netboot-update-work/{bin,lv2}` artifacts if the
binary/LV2 content itself hasn't changed) — do not assume the automatic
poll loop will ever pick it up on its own.

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

**Update (2026-07-25): the ~1Hz periodic stall documented above no longer
reproduces** on the fully-fixed device (all 3 fixes below applied, live,
survived a real reboot). Re-ran `test/hardware/bisect-1hz-stall.js` (after
fixing two real bugs in the tool itself, see below) three times: 20s and
60s captures with the Core-3 LV2 host both active and disabled (identical
in both configurations — 2 minor diag-gap lines each, zero "big" ≥10ms
events), plus a clean 90-second direct log capture showing ZERO diag-gap
lines at all. Given the stall's own documented signature is "fires at
almost exactly a 1.000-second period," a 60s+ clean window is decisive —
at the previously-measured rate it would have fired ~60+ times. No
specific mechanism was proven (this is an A/B-disappeared result, not a
root-cause trace), but the most likely explanation is that it was caused
or exacerbated by one of the 3 bugs below — most plausibly the MIDI-remap
loop's growing per-block cost (bug #1), which could plausibly have
produced periodic pressure spikes as its cost scaled with bound-control
count, or contention from the pre-fix `aloop.lv2` duplicate-DSP load (bug
#3) intermittently competing for the same core. Do not re-open this as
"still unresolved" without a fresh live re-test first — the evidence
currently on hand says it is gone.

Two real, unrelated bugs were found and fixed IN the bisection tool itself
while re-running it: (1) `writeFileContent()`'s SFTP `rename()` call failed
with a bare `Failure` status when the destination already existed (it
always does — `/etc/aloop.conf` is never missing) — SFTPv3's rename has no
POSIX atomic-overwrite guarantee; fixed by `unlink()`-ing the destination
first. (2) The "is `disable_core3_lv2` already active" detector's regex
matched the shipped config's own commented-out documentation line
(`# disable_core3_lv2 = 1      # DIAGNOSTIC ONLY: ...`), producing a
false-positive warning on every normal, unmodified config; fixed by
anchoring the regex to line-start with no leading `#`.

## Severe continuous readi()-slowdown (NOT the ~1Hz periodic stall above — a distinct, separate issue, now resolved): a per-block string/map-lookup path scaling with control count, not USB hardware

A different symptom from the periodic-stall section above: `readi()` itself
taking 2.2-2.7ms against a 1.333ms expectation, continuously (not
periodically), with xruns climbing without bound, reproducible at idle with
nothing recording or playing. An initial investigation pass wrongly
concluded this was an inherent USB full-duplex isochronous-transfer-timing
hardware limitation, "not something fixable in software" — that conclusion
was wrong. The real cause: `worker()`'s "apply remappable controls" block
(`src/dsp/audio_thread.cpp`) called `targetToZone()` (a `std::string`
allocation + `snprintf`) then two string-keyed map lookups
(`ParamStore::get` by name, then `FaustUI::set`'s `zones.find` with an O(n)
linear suffix-scan fallback) for **every bound MIDI/effect control, every
single audio block** (750/sec) — unlike `sidechainSrcSlot` a few lines
above it (already a correct resolve-once-per-slot cache), this exact path
was never converted, so its cost scaled directly with how many
controls/effects have been bound over the project's life. The block's own
comment claimed this was already cheap and alloc-free; it was neither —
never trust an in-repo comment's claim of "already optimized" over actually
reading what the code does (see the "verify the spec" lesson below).

**How this was actually found** (not by guessing harder at the USB
hypothesis, but by checking a specific, previously-unchecked signal):
`/proc/<tid>/schedstat`'s `sum_exec_runtime` showed the RT audio-worker
thread on-CPU ~95% of wall-clock uptime, but with only ~46 voluntary
context switches/sec — a thread genuinely blocking-and-waking once per
`readi()` call at 750 blocks/sec should show ~750 voluntary sleeps/sec, not
46. A thread that's supposedly blocked in a real hardware wait sleeps; one
that's actually spinning/working shows exactly this low-voluntary-switch,
high-on-CPU signature. That pointed straight at the **untimed** span
between `readi()` returning and the already-instrumented `compute()` timer
starting — the remap loop lives exactly there.

**Fix**: cache each bound target's resolved `(ParamStore slot, Faust zone
float*)` pair once, rebuilt only when `ParamStore::count` grows (a new
mapping gets bound) — the identical lazy-resolve-while-unresolved
discipline `sidechainSrcSlot` already used. The per-block hot path is now a
flat array walk: one atomic load + one pointer store per control, zero
string construction, zero map lookups, zero allocation.

**Verified live on the real device**: xruns went from 39047 (accumulated,
continuously climbing) to 0 and holding over a clean 20+ second window;
zero new `[diag-gap]` lines over that same window (previously continuous,
multiple per second); the audio-worker thread's `/proc/<tid>/stat` `state`
field changed from `R` (running/spinning) to `S` (sleeping) between blocks.
The user's original reported symptom (first recording ending up "a
fraction" of what was played) was independently verified resolved via
byte-level MIDI injection (`tcp/9401`): a real 5128ms press-to-press
interval produced a recorded `wraplen` of 246079 samples against an
expected 246144 — a 65-sample (~1.35ms) difference consistent with ordinary
MIDI-dispatch scheduling jitter, not truncation.

A separate, real regression was found while verifying this on the live
device, then fixed in two more steps — tracked separately, do not conflate
with the readi-slowdown fix above, which is independently resolved
regardless of either issue below:

1. Both home LV2 plugins faulted and got disabled by the crash-isolation
   watchdog on their very first `runOne()` call, every single startup,
   after logging `lilv found no plugin matching bundle — falling back to
   .so-only load (no port wiring)`. Cause: `readTtl()`'s bundle-matching
   check (`src/host/lv2_host.cpp`) was a raw `bundlePath.find(bpath) == 0`
   prefix comparison; lilv's resolved bundle path carries a trailing slash
   the passed-in `bundlePath` never has, so the comparison silently and
   permanently failed even for well-formed bundles — taking the no-port-
   wiring fallback, whose first `run()` call then dereferences unconnected
   port pointers (the same class of bug as the nullptr-features fix
   elsewhere in this file). Fixed by stripping trailing slashes from both
   sides before an exact-equality compare.

2. Once that fix let both plugins actually load with real port wiring,
   `core_busy` jumped from the normal ~22-27% baseline to ~63-65% with
   xruns climbing continuously — a NEW regression exposed only by fixing
   #1 (a crashed-and-disabled plugin costs ~nothing; a genuinely running
   one costs whatever its real DSP does). Isolating each plugin (moving
   one at a time out of `/effects/home`, restarting, remeasuring) showed
   `aloop.lv2` alone reproduces the high `core_busy`/climbing xruns:
   `build-lv2.yml`'s `home-fx-lv2` job compiles `dsp/aloop.dsp` — the exact
   same Faust source `audio_thread.cpp`'s `faustHome` already compiles
   NATIVELY and runs every block — into a standalone LV2 bundle, purely as
   a CI reproducibility/packaging check (ADR-003). It was never meant to
   run a second time in the live effects chain; `image/lib-boot-tree.sh`'s
   packaging step copied every `*.lv2` it found under `LV2_DIR` with no
   distinction, so `aloop.lv2` ended up deployed and loaded right alongside
   `guitar_lofi_fx.lv2` (a genuinely standalone effect with no dependency
   on `aloop.dsp`/`loop.dsp`). Fixed by excluding `aloop.lv2` by name from
   that copy step. Verified live: with `aloop.lv2` removed and
   `guitar_lofi_fx.lv2` kept, `core_busy` returned to ~23-30% with xruns at
   0, held over a clean 15+ second window — `guitar_lofi_fx.lv2`'s own real
   DSP cost (genuinely running for the first time after fix #1) is
   sustainable on its own.

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
- **`-ct 0`** (disable Faust's table range-checking) added to all 3
  invocation sites. Verified safe by tracing every `rwtable` index in this
  codebase by hand: `dsp/loop.dsp`'s `readIdx0`/`readIdx1` are always
  `... % wrapLen` (explicit modulo, provably in `[0,wrapLen)`) with
  `writeIdx` clamped to `MAXLEN-1`; `effects/home/faust/microrepeat.dsp`'s
  `wpos`/`rpos` are likewise always clamped against `MR_MAX`/modulo'd
  against `sliceLen` before use. Every table access in every `.dsp` file
  this codebase compiles is software-bounded by existing logic already —
  this hand-trace *is* the "explicit per-table proof" this file's own
  earlier draft of this section said would be required before shipping.
  `guitar_lofi_fx.dsp` has no `rwtable`/`rdtable` at all, so the flag is
  unconditionally safe there too.
- **`kFoldStep/N` hoisted out of `audio_thread.cpp`'s per-sample SHIFT/
  glitch fold-gain ramp loop** (was recomputed via division up to 4x per
  sample; both operands are block-constant) — a direct application of the
  Faust manual's "multiply rather than divide" principle to the native C++
  hot path, not just the Faust-generated code.

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
  once under live suspicion for the ~1Hz stall (see "Diagnosing periodic
  audio stalls" above — no longer reproduces as of the 2026-07-25 update in
  that section, but the multiply-dispatch-overhead argument against
  splitting still stands on its own), directly risking the
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

Investigated and found not applicable (corrected a wrong initial premise):

- **`-mcd`/`-dlt` (delay-line threshold tuning)**: these flags govern only
  Faust's `de.delay`-family delay-line codegen, NOT the `rwtable` primitive
  — confirmed directly against the manual. `dsp/loop.dsp`'s 20×
  `MAXLEN=48000*60` rings and `microrepeat.dsp`'s ring are both `rwtable`,
  so entirely unaffected by these flags. Only `delay.dsp`
  (`de.delay(MAXD=96000,...)`) and `reverb.dsp` (`de.delay(8192,L)`, L up to
  4057) actually use delay-line codegen, and both are comfortably above the
  default `-mcd 16` threshold already — Faust's defaults (`-mcd 16`,
  `-dlt <INT_MAX>`) are already the memory-efficient choice for these sizes,
  with no measured bottleneck to justify a change. No action taken.

**`-mapp` — verified and SHIPPED** (was HIGH RISK, now proven safe): a real
A/B numeric harness settled this rather than leaving it as a docs-driven
guess. Built natively (no Docker needed — a native Windows Faust install
already exists at `C:\faust`; `mingw g++` compiles the generated C++
directly): generated `dsp/aloop.dsp` twice (current shipped flags, and the
same plus `-mapp`), compiled both into a standalone harness reusing the
EXACT `FaustUI`/`FaustDspBase` shim `audio_thread.cpp` uses in production
(see that file's own `#include "loop.cpp"` block), and drove both through an
identical synthetic cycle exercising precisely the arithmetic this
codebase's own history flags as fragile: first-recording immediate arm,
finish-quantization's EXTEND case (`gridStep = masterLen/16` snapping),
varispeed engage → disengage (the re-snap-to-`absPos` edge case), a second
loop's own quantization, and dual-loop playback under two different
`wrapLen`s — 9280 samples total. Result: **byte-identical output, 0 diff
lines, identical md5sum**, with and without `-mapp`. One caveat found along
the way (Windows-specific, harmless): `AloopLoopDsp` is ~232MB
(`sizeof()`) — 20 loopers × `MAXLEN` rwtable storage each — so it must be
heap/static-allocated, never stack-allocated, in any future standalone
harness (a stack allocation blew the default 1MB Windows thread stack
immediately, `STATUS_STACK_OVERFLOW`, before a single sample was even
processed).

**Real measured CPU improvement — closed the loop from theoretical to
proven.** Added a `faust2bench`-based benchmark step to CI
(`build-binary.yml`'s "Benchmark CPU usage" step, real Ubuntu Linux host —
`faust2bench`'s own bundled `bench.cpp` needs `pwd.h`, which doesn't exist
under MinGW, so this genuinely cannot run on Windows). 20 runs each, `-bs 64`
(matching aloop's real block size), same `dsp/aloop.dsp` source:
- **Baseline** (no flags): DSP CPU ≈ **4.27%**, ≈44.9 MBytes/sec.
- **Shipped** (`-vec -fun -dfs -vs32 -nvi -ct0 -mapp`): DSP CPU ≈ **4.12%**,
  ≈47.5 MBytes/sec.

A real, reproducible ~3.5% relative CPU reduction and ~6% throughput
increase from the flag change alone (same DSP math, verified byte-identical
output earlier in this section) — measured on the CI runner's x86_64 core,
not the Pi 4's aarch64 Cortex-A72 specifically, but the same flags apply
there via `-mcpu=cortex-a72` (also shipped, see below) and the underlying
codegen-strategy win (fewer scalar loop overheads, inlined functions, no
vtable indirection) transfers across architectures even if the exact
percentage doesn't.

**`-mcpu=cortex-a72`** (the Faust manual's `-march=cpu` advice, aarch64
form) added to every real target-compile step: `src/CMakeLists.txt` and
both LV2 `.so` link steps in `build-lv2.yml`. Deliberately NOT added to the
two native-host `.ttl`-metadata-generation `g++` compiles in
`build-lv2.yml` — those run on the CI runner's own x86_64 to print static
metadata and must never target aarch64.

**`-fm def` — still NOT shipped**, correctly, not as a hedge: it's a
strictly broader flag than `-mapp` (adds sin/cos/tan/atan/exp/log/pow/sqrt
approximations, touching `filters.dsp`'s `tan()`, `reverb.dsp`,
`compressor.dsp`'s `exp()`/`log10()`/`pow()`, and `pitch.dsp`'s `pow()` —
not just the floor/ceil/fmod/remainder class `-mapp` covers), so it needs
its own separate proof, not a free ride on `-mapp`'s result. Blocked on this
machine by a genuine toolchain artifact: Faust's own bundled
`faust/dsp/fastmath.cpp` uses `dllexport`/`EXPORT` linkage that MinGW g++
rejects for internally-linked symbols on Windows — unrelated to aloop's own
code, and irrelevant to the real target (Alpine musl/aarch64 via CI, a
different toolchain that may not hit this at all). Needs either a
Linux-side build of the same harness or genuine on-device verification
before it can be shipped with the same rigor `-mapp` now has.

Resolved (parameter-smoothing order): **no change made, confirmed
deliberate.** `effects_runtime.dsp`'s `filterStage`/`delayStage`/
`reverbStage`/`pitchStage` take raw `hslider` values straight into
`pow()`/`exp()`-bearing math with no `si.smoo` upstream — this is NOT an
oversight. `effects/home/faust/param_mapping.md` explicitly documents that
the audio path is verified against per-render-constant normalized CC
values, with the all-defaults state producing a byte-exact passthrough
(verified `maxAbs=0`); `filters.dsp`'s own header states params are
compile-time/per-render constants "matching the looper's per-block
piecewise-constant param behaviour." Adding `si.smoo` would change the
transient response when a knob value changes between renders, directly
contradicting this already-verified hardware-parity requirement. Left
as-is.

## `apc_grid.cpp`'s rec/play cycle: `rec` must be explicitly zeroed on FINISH

`rec` is a persistent `ParamStore` value, not a momentary Faust `button()`
the widget itself releases. An earlier version set `rec=1` AND `play=1` in
the same press with nothing ever resetting `rec` back to 0 — `dsp/loop.dsp`
(`record = in*recN`) then re-recorded live input over the loop forever,
which looks identical to "not playing" from outside ("loops don't play,
they just stay paused"). `applyRecPlayCycle` (`src/control/apc_grid.cpp`)
now explicitly sets `rec=0` on FINISH. The real per-looper press cycle is:
empty → ARM (`rec=1`, held for the whole recording pass) → FINISH (`rec=0`,
`play=1`) → pause (`play=0`) → resume (`play=1`) → ...

ARM and FINISH must fire on PRESS, not release (both are "the exact instant
must land precisely" cases — release-triggered dispatch would add hold
duration as timing jitter to the start or end of the take). Pause/resume
stay on release. `m_looperArmedOnPress` suppresses the matching release
from double-firing the tap.

## `apc_grid.cpp` master-phrase length: always read writeIdx telemetry, never wall-clock, for sample-accurate boundaries

Ported from `../looper`'s `masterLoopBlocks` design: the shared phrase
length every looper quantizes to is established from the FIRST recorded
clip's own actual duration, not a fixed default. `dsp/loop.dsp`'s ring
length used to be driven only by the Link-synced branch, so a standalone
(no Link) recording left it frozen at the Faust-compiled default (1s),
truncating/looping real recordings short.

User's explicit, current spec (supersedes an earlier "TRUE PHRASE-LOCK"
design that re-derived the loop length from the tempo solver's own
beats-at-BPM reconstruction — that old design is WRONG, do not resurrect
it): loop 1 must play back at EXACTLY its raw recorded duration, like a
commercial looper. A wall-clock (`now_ms`) estimate can never satisfy this
— it isn't sample-accurate relative to the audio thread's own per-block
timeline. Fix: read `AudioThread::snapshotTelemetry().looperWriteIdx[looper]`
(the DSP's own true elapsed-sample count since the real grid-aligned arm
instant) instead of a wall-clock estimate; wall-clock is kept only as a
defensive fallback when `audio` is null (should not happen in practice).
`deriveTempoQuant` is used ONLY to propose a BPM to Link — it must never
resize `m_masterLenSamples`/`cmd/master_len` itself.

## `apc_grid.cpp` successive-recording quantization: 4 design rounds, final spec is power-of-2-only + log-space midpoint

Mirrors `../looper`'s `_calcQuantizeTarget`: a subsequent recording's raw
duration snaps to a musical subdivision/multiple of the established master
phrase length M, rather than collapsing to exactly M regardless of hold
duration. History of what was tried and why each round was replaced:

1. **Small fixed candidate set** `{..., 2M, 4M}`, nearest-raw-distance pick.
   Bug: a big gap between 2M and 4M meant anything past ~3M jumped all the
   way to 4M, recording far past the performed content ("took a piece of
   the end"); also capped at 4M with no way to reach 8M/16M/64M takes.
2. **Linear 68% threshold** between brackets. Bug: a fixed linear fraction
   of a full-octave span could trim up to 0.68×M off a genuine recording
   (e.g. a real 1.5M take cut down to M).
3. **M/16-linear-step grid** (any multiple of M/16). Rejected live by the
   user: "loops should always be clean multiples... guaranteed no matter
   how many times they play" — a linear M/16 grid could still land on
   musically-meaningless fractions like 5/16 or 11/16.
4. **Final/shipped**: candidates are POWERS OF 2 ONLY relative to M (M/16
   floor, M/8, M/4, M/2, M, 2M, 4M, 8M, ...). Decision between the two
   bracketing powers of 2 is the LOG-SPACE (geometric) midpoint —
   `sqrt(lowerCand*upperCand)` — not a linear threshold: this is symmetric
   and scale-independent regardless of which octave the recording lands in.
   Because every candidate is a genuine power of 2, any two loopers'
   `wrapLen`s are always in a clean power-of-2 ratio, which (combined with
   `dsp/loop.dsp`'s `cycleOffset` fix) guarantees perfect drift-free repeat
   alignment forever (a power-of-2 ratio between two phase-locked rings can
   never phase-drift).

Also applies: use `writeIdx` telemetry (not wall-clock) for the raw
duration input here too, for the same press-to-grid-tick timing-gap reason
as the first-recording case above.

## `apc_grid.cpp`: real APC Key25 hardware re-sends note-on for an already-held pad

Unlike the synthetic MIDI-inject test path, real hardware can re-send
note-on for a pad that's physically still held down. Without a guard, each
repeat unconditionally reset the hold-start timer (defeating long-hold
erase accumulation) and, far worse, could re-enter the ARM/FINISH dispatch
mid-recording — prematurely finishing a take after only the repeat interval
(a fraction of a second) and immediately re-arming a new recording on what
the user believes is still their original press. This is a direct
structural mechanism for "recording came out blank/near-silent," independent
of any DSP or erase-timing issue. Fix in `onPadPress`: track `m_looperHeld`
per pad and treat a repeat note-on for an already-held pad as a no-op.

## `apc_grid.cpp`: momentary Faust gates (`erase`, `finishreq`, `cmd/clearall`) must be explicitly released, never fire-and-forget

Every one-shot Faust gate driven from the control thread (`looperN/erase`,
`looperN/finishreq`) needs an explicit delayed release, or it sticks at 1
forever with nothing else ever writing it back to 0:

- **`erase` stuck at 1**: `dsp/loop.dsp`'s `wipe = max(clearAll, eraseN)`
  gates ring recirculation (`hold *= (1-wipe)`) every block. A fire-and-forget
  `erase=1` silently wiped playback on every subsequent block forever —
  recording still worked (unaffected by wipe), so the symptom was "after
  clearing it, the second round didn't play after recording." Fix:
  `pollHolds` records a release deadline (~50ms, many DSP blocks later) and
  clears it on a later tick — setting then immediately clearing in the same
  call would race the audio thread's plain-atomic read with no ordering
  guarantee (could read 0 and never observe the wipe at all).
- **`finishreq`**: same momentary-pulse shape. `dsp/loop.dsp`'s
  `finishRequestedStep` only needs to see `finishreq>0.5` for one sample
  (it latches into `finishRequested` until the next armEdge), so holding it
  ~50ms then releasing is correct — it does not need to still be 1 by the
  time the DSP-side target is actually reached.
- **`cmd/clearall`**: a genuinely HELD value (note-on sets it, the user's
  own later note-off releases it) — real wall-clock time passes in between
  by construction, so this one doesn't need the deadline pattern.

## `apc_grid.cpp` CLEAR_ALL: resetting the C++ shadow state does NOT stop the Faust DSP — both `play` and `rec` gates must be explicitly zeroed too

Two rounds of the same bug class, found live on real hardware:

1. `onClearAll` reset `m_looperPlaying[lp]=false` (C++ shadow only) but never
   told Faust to stop: `dsp/loop.dsp`'s `out = loopSig * playN * volN` kept
   outputting whatever the ring held, gated by `playN`, which was never
   zeroed — "clearing doesn't stop them." `wipe` only silences the ring's
   own recirculated content, it does not touch the play gate. Fix: explicitly
   `setLooper(ps, lp, "play", 0.0f)` in the clear path, not just the shadow.
2. Same bug for `rec`: CLEAR_ALL pressed WHILE a looper was mid-recording
   left that looper's Faust `rec` zone stuck at 1 forever (shadow reset only).
   `hold = delayed*(1-recN)*(1-wipe)` stays zero for as long as `recN==1`, so
   that looper could never play back ANY content again, even after a fresh,
   otherwise-correct ARM/FINISH cycle — "loops don't play, only passthrough."
   Fix: explicitly zero `rec` too, matching the `play` fix.

Same file's `onStopImmediate` also explicitly zeros `rec` for any
mid-recording looper (unlike plain `cmd/stopall`, which only zeroes `play`)
— stopping mid-recording is an abort, and an aborted looper stays "empty,"
never "has content."

## `dsp/loop.dsp`'s `clear`/`speed` engine-globals: Faust's `par()` re-elaborates UI primitives at EVERY instantiation site, hoisting the declaration alone does not collapse the zone count

`oneLooper` is instantiated 20× via `par(i, NLOOPERS, vgroup(...))`. A first
fix attempt hoisted the `button()`/`hslider()` declarations for `clear`/
`speed` outside the `par`/`vgroup` and passed them in as parameters —
this looked like it should produce one shared zone, but Faust's `par()`
re-elaborates whatever UI primitives sit inside an argument expression at
EACH of its 20 sites (confirmed via generated C++: still 20 separate zones,
one per "looper N" vgroup, even after that fix). The real fix removes
`clear`/`speed` as Faust UI zones entirely and threads them in as plain
`process()` signal inputs instead (like `prevFiltIn`) — `audio_thread.cpp`
writes them into `fins[2]`/`fins[3]` every block. `par()` cannot duplicate a
plain signal input since there's no UI primitive to re-elaborate.
`cmd/clearall` now correctly wipes every looper's ring in one write.

## `apc_grid.cpp`: an emptied rig must reset the shared master phrase length, from ANY path that can empty it

Per-looper long-hold erase (not just the PLAY/CLEAR_ALL button) can also
leave the rig with zero loopers holding content. `m_masterLenSamples`/
`cmd/master_len` (and `cmd/recorded_bpm`, which rides with it) must reset
to 0 whenever the LAST looper with content is erased this way, or the next
recording reuses the stale phrase length from before — witnessed live as
"the second loop became a continuation of what the first loop was set up to
do instead of starting a new song," landing the quantize branch (not the
first-establish branch) and truncating to the wrong length from the wrong
point. `pollHolds` checks `anyHasContent` after the per-looper erase loop
and resets both values if the rig just went empty, mirroring `onClearAll`'s
own reset.

## `dsp/effects_runtime.dsp`'s old `fx/bank` 3-way crossfade cost all 3 banks every block regardless of selection — removed, not fixed in place

The old design used one in-Faust `fx/bank` selector zone to crossfade
between Dub/Guitar/Lofi-Fx effect chains. WITNESSED live on a real Pi 4:
Faust has no runtime branching, so the crossfade computed all 3 full effect
chains every single block regardless of which was "selected" — a real
~7pp `core_busy` regression causing continuous audio dropouts.
`effects_runtime.dsp` is restored to its pre-LOFI dub-only chain with no
`fx/bank` zone; Guitar and Lofi-Fx moved to their own permanent Core-3 LV2
bundle (`guitar_lofi_fx.dsp`), always active, never gated by a selector.
`ApcGrid`'s 3-bank fx control surface (`onDubFxPress`/`onGuitarFxPress`/
`onLofiFxPress`) is now a pure UI/state change — each bank press only flips
which knob-target table the next CC touch reaches (`m_activeBank`) and
starts an LED flash; there is nothing left to re-push to Faust on a bank
switch, since Guitar/Lofi-Fx targets are LV2 controls on the always-active
Core-3 bundle, not a Faust zone that needs redirecting.

## `apc_grid.cpp` CC53 formant constants were verified wrong against `../looper`, corrected to match exactly

Direct cross-codebase comparison found the prior aloop constants both off
from looper's real values: deadzone was 62-65 here vs looper's real 60-68,
and the unshifted range was ±1.5 here vs looper's real ±1.0 (looper: default
±1 "musical territory", SHIFT expands to ±3). Also the normalization formula
itself used `/63.5` (differently centered) vs looper's real
`((data2-64)/63.0)*range`. `onFormantCC` now matches looper exactly:
deadzone 60-68, range ±1 unshifted / ±3 shifted, `/63.0` formula.

## `apc_grid.cpp` sidechain-pump gesture: guitar-fx held REDIRECTS looper pad presses entirely, does not layer on top

While `m_guitarFxHeld` is true, a looper pad press is consumed entirely by
`onSidechainLooperToggle` (toggles that looper's sidechain-source
designation) and never reaches the normal ARM/FINISH dispatch — it does not
touch `m_looperHeld`/`m_looperHoldStart` at all, since this is a one-shot
toggle gesture, not a "hold this pad" gesture. The sidechain-source
designation auto-clears whenever that looper's content is wiped (long-hold
erase or CLEAR_ALL) — a source tied to specific recorded content shouldn't
silently survive the content being erased.

## FaustUI shim: `addHorizontalBargraph`/`addVerticalBargraph` must register zones too

`src/dsp/audio_thread.cpp`'s hand-written `FaustUI` shim (the param-binding
`UI` Faust's generated code calls into) had `addHorizontalBargraph`/
`addVerticalBargraph` as empty no-ops, so every `hbargraph()` zone (level/
writeidx/wraplen, for all 20 loopers) was never inserted into `zones` at
all. Every `fui.get("looperN/level"/"writeidx"/"wraplen")` call therefore
missed the exact-match `find` AND fell through to the full O(n) linear
suffix-scan over the entire `zones` map, every time, for nothing — 60 wasted
full-map scans per audio block (3 fields x 20 loopers), 750 blocks/sec =
45,000 wasted linear scans/sec on the RT audio thread, permanently starving
`snd_pcm_readi` of CPU time between iterations. WITNESSED: telemetry's
level/wraplen fields read back all-zero on a live, actively-playing looper.
Fix: register these exactly like every other control type (`zones[full(l)]
= z`).

## `targetToZone`'s `fx/bank` mapping was silently missing

`src/dsp/audio_thread.cpp`'s `targetToZone()` (control-map target name →
Faust zone label) had no case for `fx/bank` (the LOFI 3-bank fx-crossfade
control, a straight passthrough since `effects_runtime.dsp` declares
`nentry("fx/bank", ...)` under its own literal name, not a renamed control
label). WITNESSED live: `ApcGrid::pushBankValuesToZones` wrote
ParamStore's `"fx/bank"` target correctly, but the generic forEach-push
loop's fallthrough `return ""` meant it never reached the real Faust zone —
bank-select buttons updated C++ state but the DSP's own bank-crossfade
never saw the change, staying pinned on Dub forever. Fixed by adding the
passthrough case.

## Faust `par()`-replicated UI controls silently duplicate per instance — use signal inputs instead

A `UI` control (`button()`/`hslider()`) passed as an argument into a Faust
function that `par()` instantiates N times gets RE-ELABORATED (UI
declaration included) at each of the N call sites — even when the
declaration text is hoisted outside the `par`/`vgroup` — silently producing
N duplicate zones instead of one shared zone. WITNESSED via the generated
C++ (`build/loop.cpp`): `grep -c '"speed"'`/`'"clear"'` both returned 20
(once per looper's own vgroup) even after an earlier fix attempt (commit
`382e775`) that looked correct in the `.dsp` source — that fix was cosmetic
and never actually collapsed to one zone, which is why half/double-speed
and clear-all kept only affecting one of 20 loopers even after landing. Fix:
thread the value through as a plain **signal input** to `process()` instead
(a wire, not a UI primitive — nothing to duplicate). `dsp/loop.dsp`'s
`oneLooper` now takes `clearAll`/`speedMul`/`masterPhase`/`masterLen`/
`sidechainEnv` this way; `audio_thread.cpp` fills each corresponding buffer
with a block-constant value every block (`std::fill`) rather than calling
`fui.set()`.

## `AloopLoopDsp` (the Faust home stack) must be heap-allocated, never a stack-local

`sizeof(AloopLoopDsp)` is ~320 MiB (20 loopers x `MAXLEN=48000*60`, 60s
delay-line rings each). WITNESSED live on a real Pi 4 (gdb + a real core
dump, `-g -O0` debug build): declaring it as a stack-local inside `worker()`
SIGSEGV'd at `setRealtimeSelf`'s very first local-variable stack write — no
pthread stack size (musl's small default, or an explicit 8 MiB, both tried)
could ever be large enough; the frame was simply unmapped from the moment
the thread's stack pointer moved to make room for a 320 MiB local later in
the same function. Fixed via `std::make_unique<AloopLoopDsp>()` — a
one-time allocation at thread startup, never in the per-block RT hot path,
so it carries none of the no-malloc-in-the-callback risk the rest of this
file is written to avoid. The `Sampler` (~5.3MB) is heap-allocated the same
way, for the same class of reason, well under the threshold that made it
mandatory for `AloopLoopDsp` but still worth keeping off the thread stack.

## Instrument USB audio device is S32_LE, not S16_LE — and ALSA silently ignores a wrong format request

The M-Audio AIR 192|4 (and most class-compliant USB audio interfaces) only
supports S32_LE (24-bit data left-justified in a 32-bit word — confirmed
via `/proc/asound/card0/stream0`: "Format: S32_LE, Bits: 24"); there is no
S16_LE fallback. WITNESSED live: requesting `SND_PCM_FORMAT_S16_LE` via
`snd_pcm_hw_params_set_format` previously succeeded at the `hw_params()`
call (no error returned) while the device silently negotiated S32_LE
anyway — the return value was never checked, so the mismatch went
undetected until it produced loud static, because the code still
normalized captured samples by the 16-bit divisor (32768) on 32-bit-wide
data (values ~65536x too large before Faust ever saw them). Fixed: buffer
type is `int32_t`, normalization divisor is `2147483648.0f`
(INT32_MAX-equivalent), and the negotiated format is read back via
`snd_pcm_hw_params_get_format` and compared against what was requested,
warning loudly on mismatch instead of silently corrupting audio. The OTG
gadget mirror is a genuinely separate S16_LE device (`f_uac2-gadget.sh`
sets `c_ssize`/`p_ssize=2` explicitly), so the two output paths need
separate wire buffers in their own native formats — never share one.

## ALSA playback stream needs `start_threshold` lowered to one period, or it never leaves PREPARED

The hw_params default `start_threshold` for a PLAYBACK stream is the full
`buffer_size`. WITNESSED live on a real Pi 4:
`/proc/asound/.../pcm0p/sub0/status` stayed stuck in `PREPARED` forever,
because this codebase's block loop only ever writes one N-frame period per
`snd_pcm_writei()` call and immediately blocks on the next capture read, so
the ring never reached a full `buffer_size` of queued frames to cross the
default threshold — meanwhile CAPTURE (which starts as soon as any data is
available, not gated on a full buffer) ran fine, so the two streams
silently desynced and playback underran on every write. Fixed by
`snd_pcm_sw_params_set_start_threshold(pcm, sw, period)` (one period, not
the full buffer) so playback triggers on the very first `snd_pcm_writei()`,
matching capture's own behavior.

## ALSA period/buffer sizing: 2 periods (256 frames) xruns constantly on this USB gadget PCM; 4 periods is the minimum that holds

WITNESSED live on a real Pi 4: an initial 2-period buffer (256 frames
total, the ALSA minimum) produced 690 xruns within seconds on the
instrument-device PCM — too tight for a USB gadget path, where each
read/write also rides USB's own transfer-scheduling jitter on top of this
thread's SCHED_FIFO jitter (unlike a bare-metal build with no OS/USB-stack
contention at all). 4 periods (at the real `block_size` N per period) keeps
the same per-period latency-determining granularity while giving the ring
enough slack to absorb that jitter. The OTG gadget mirror uses even looser
timing on purpose (period = 4xN, buffer = 4x that) since it doesn't need
tight latency, just enough buffer that its own scheduling jitter doesn't
underrun constantly.

## `masterPhaseBuf` must ramp per-sample within the block, not step at block boundaries

WITNESSED live: "loops now sound bitcrushed." Root cause: `masterPhaseBuf`
(the phrase-lock shared-clock signal input) was filled via `std::fill()`
with the same block-start value across all N samples — correct for
genuinely block-constant commands like `clearBuf`/`speedBuf` (momentary,
slow-changing), but `dsp/loop.dsp`'s `absPos` formula treats `masterPhase`
as this looper's actual per-sample READ POSITION at `effSpeed==1.0`.
Holding it constant for a whole 64-sample block meant `readIdx0`/`readIdx1`
never advanced within a block, only jumping 64 samples at each block
boundary — a stepped/aliased readback pattern, audibly indistinguishable
from bitcrushing. Fixed: ramp smoothly within the block (`masterPhaseBuf[i]
= masterPhaseSamples + i`, wrapped at `masterLen`) so it behaves as a real
per-sample position signal, not a step control.

## Sampler capture must read from a separate pre-mix buffer, not `fin`, to avoid self-recording

User requirement: SHIFT should route loop content into sample recording the
same way input and effects already get recorded into samplers. First fix
attempt (caught before shipping): moving `captureBlock` to run on `fin[]`
after the SHIFT/glitch fold reintroduced a genuine self-recording bug,
because by that point `fin[]` also contains this block's own
`renderInto`-mixed sample-playback voices — a sample recorded while another
sample/drum hit is playing would record itself. Real fix:
`src/dsp/audio_thread.cpp` snapshots `captureFin = fin` BEFORE
`renderInto()` ever touches `fin`, applies the identical SHIFT/glitch fold
to `captureFin` independently in the same fold loop, and captures from
`captureFin`. Net effect: `captureFin` = dry input + folded loop content,
deliberately excluding this block's own sampler playback; `fin` = dry input
+ sampler voices + folded loop content (the correct DSP-facing signal). The
two must stay structurally separate buffers, not a single buffer read at
two different times.

## Recording must tap a dedicated post-fx Faust input, never fold a post-fx signal into `fin`

Feeding any post-effects tap into `fin` (the live dry/input signal) makes
it become next block's `dsp` input again, which then flows through `fx`
AGAIN every block — stages reprocessing their own prior output produces a
fast, aliased whine (WITNESSED-BROKEN, originally found via the glitch-only
`prevGlitchTap` wiring). Fixed by giving `loop.dsp`'s `process()` a
dedicated second input (`prevFiltIn`) that ONLY the record-capture term
consumes (`record = prevFiltIn * recN`) — this input never joins the
dry/live path, so it structurally cannot flow back through `fx` on this or
any later block. `audio_thread.cpp` feeds this from `prevFiltOut`, a
snapshot of the PREVIOUS block's fully-effected mix (`rawFiltTap`, one of
`aloop.dsp`'s `process()` outputs), always the full effects chain
regardless of SHIFT state (user requirement: recording must always capture
the fully-effected signal, not raw pre-fx input). This replaced an earlier
separate glitch-only tap: `prevFiltOut` already contains post-glitch
content one block later since `microStage` is upstream of `filterStage` in
`effects_runtime.dsp`, making a separate glitch term redundant/
double-counting once both were live.

## Flush-to-zero/denormals-are-zero must be set explicitly on the audio thread — no portable C++ API on ARM

Denormal (subnormal) floats occur naturally in any decaying IIR
filter/feedback loop asymptotically approaching zero (reverb tails, delay
feedback, envelope followers). On both ARM and x86, denormal arithmetic can
be 10-100x slower than normal-range floats because the FPU falls back to a
microcoded slow path. There is no portable C++ standard API for this on
ARM (the compiler intrinsic `_MM_SET_FLUSH_ZERO_MODE` only exists for x86
SSE); `src/dsp/audio_thread.cpp`'s `setFlushToZero()` sets the AArch64
FPCR's FZ bit directly via inline assembly (`mrs`/`msr fpcr`) on
`__aarch64__`, and the SSE intrinsics on x86. Applied once at thread
startup before any DSP compute runs — a startup-only cost, no per-block
overhead. Became relevant when the LOFI feature added 3 parallel bank
chains (dub/guitar/lofi-fx) computed every block regardless of audibility
(Faust has no runtime branching — `select2`/`ba.if` choose among
already-computed signals, they don't skip computing them), tripling the
places a silent/near-silent signal can spend sustained time in denormal
territory.

## Two ALSA devices, never conflate them: instrument device is the real path, OTG gadget is a best-effort mirror

`src/dsp/audio_thread.cpp`'s `worker()` opens TWO distinct PCM devices,
matching the historical hardware split: the instrument device (default
`hw:0,0`, e.g. the M-Audio AIR 192|4) is the real tight-latency
capture+playback path a musician actually hears — `cap`/`play` are always
this device, opened blocking, retried up to 30 times at 1s intervals if the
interface isn't plugged in yet. The OTG gadget (`f_uac2`,
`hw:UAC2Gadget,0`) is a best-effort MIRROR of the same processed output,
opened NONBLOCK so a missing/non-streaming host on the other end can never
stall or desync the instrument device's real-time path. `-EAGAIN` on the
OTG write is expected/silently skipped (ring still has enough queued, not
an error); any other negative return triggers a one-shot recover, and if
the device is gone for good, later blocks' errors keep being silently
absorbed rather than ever blocking or crashing the RT path. A failed OTG
open at startup is silent-degrade-only — the instrument-device path is
already fully functional without it.

## Sampler capture must tap the fully-effected post-fx signal, including glitch/microrepeat

User requirement: sample recording must capture the loop content AFTER the
whole effects chain (pitch/glitch/filter/delay/reverb), not a pre-fx
snapshot — the same fully-effected signal the loopers themselves record
from. `src/dsp/sampler/sampler.h`'s own header comment previously
documented `captureBlock` as reading `fin` (dry input) plus the SHIFT/
glitch fold applied to a COPY of `fin` — a pre-fx snapshot, missing
filter/delay/reverb and, before the chain reorder, even glitch itself.

Fixed in `src/dsp/audio_thread.cpp`'s `worker()`: `captureBlock` now reads
from `prevFiltOut` (the same one-block-lagged, fully-effected snapshot the
loopers' own `prevFiltIn` record-only input already uses — see the
"Recording must tap a dedicated post-fx Faust input" entry above) instead
of the pre-fx `captureFin` buffer, which was removed entirely (it had no
other reader once this changed). Same one-block-lag discipline as the
looper record path: never fed back into `fin`, so it cannot re-enter `fx`
on any later block.

## SHIFT-hold recording latency compensation: `recordStartPhaseOffset` is
biased backward at FINISH when SHIFT (`fx/monitorfold`) was held during the take

CORRECTED (a first implementation pass of this feature wrongly gated on
`fx/pitchbend_engaged`, the SNAC pitch/varispeed engine, instead of SHIFT
— caught live: user held SHIFT during a recording and reported the
looper's playback was audibly LATE relative to the other loops, i.e. the
recorded content lagged behind where it should sit). The two controls are
genuinely distinct: `fx/pitchbend_engaged` (mod-wheel/absolute-pitch/
keybed-note gestures) drives the SNAC pitch-shift engine
(`effects/home/faust/pitch_ffi.h`), while `fx/monitorfold`
(`ApcGrid::onShiftPress`/`onShiftRelease`) is the SHIFT-fold glitch
gesture — only the latter is the one that needs compensating here.

SHIFT's own real mechanism (`src/dsp/audio_thread.cpp`'s `worker()`):
`fin[i] += prevLoopSum[i] * combinedFold` feeds the PREVIOUS block's loop
output back into this block's input whenever `fx/monitorfold` is engaged
(ramped over 16 samples via `kFoldStep`, negligible next to the block-size
lag) — `prevLoopSum` is always exactly one block (`g_cfg.blockSize`, 64
samples by default) behind the live signal. Since loopers record from
`prevFiltOut`/`prevFiltIn` (already one block lagged on its own, see the
sampler-capture entry above), SHIFT engagement adds this same block-size
lag into what's actually captured, on top of the baseline pipeline lag —
recorded content ends up effectively delayed relative to the live
performance, which is exactly the symptom reported ("behind" the other
loops).

Fix: `dsp/loop.dsp`'s `latencyBiasN = hslider("latencybias", 0, -MAXLEN,
MAXLEN, 1)` is subtracted from `masterPhase` at the exact instant
`recordStartPhaseOffset` latches (`recordStartPhaseOffsetStep(prev) =
ba.if(finishEdge, masterPhase - latencyBiasN, prev)`). A smaller
`recordStartPhaseOffset` makes `absPos = wrapAbs(masterPhase -
recordStartPhaseOffset + cycleOffset, wrapLen)` LARGER for the same
`masterPhase` — i.e. playback reads further ahead into the ring at any
given moment, catching the content up to compensate for it having arrived
late. `src/control/apc_grid.cpp`'s `applyRecPlayCycle` writes this zone at
FINISH: `kShiftFoldBlockLatencySamples` (64) if
`m_looperShiftHeldDuringTake[looper]` was ever set true during the take,
else 0. The flag is sampled every `pollHolds` tick (not just at ARM/
FINISH) against `fx/monitorfold`, so a SHIFT engagement at ANY point
mid-take is captured, not only if it happened to be held at the exact ARM
or FINISH instant, and is reset to false at ARM. This mirrors the
`finishtarget` zone's own established per-looper-hslider pattern exactly
(a genuine `par()`-replicated UI zone, confirmed via generated C++ showing
20 distinct instances, not the shared-signal-input class of bug
documented elsewhere in this file) — no new signal-input wiring was
needed since this value only needs to change once per take, unlike
`effSpeed`/`clearAll`/`masterPhase`, which need per-sample ramping.

## Glitch/microrepeat steps widened one notch: divisor halved in
`sliceLen`, not the note-to-div mapping

User's explicit spec: every one of the 5 glitch/microrepeat divisions
(notes 82-86, `apc_grid.cpp`'s `div[5] = {1, 2, 4, 8, 16}`) should produce
a slice as long as the NEXT-widest division currently does — `div=16`'s
slice becomes as long as the old `div=8`'s, `div=8` as long as old `div=4`,
etc.; `div=1` (already the widest) has nowhere further to go and stays
put. `effects/home/faust/microrepeat.dsp`'s `sliceLen = (masterLoopBlocks
/16/div)*64` formula is what actually determines slice length — the
note-to-div table in `apc_grid.cpp` is untouched, so which physical pad
triggers which relative step doesn't change, only how long each resulting
slice is.

Fix, ROUND 2 (round 1 was wrong, caught live): the first attempt used
`divSafe = max(1, int(DIV / 2))` — halving the divisor BEFORE the
division. This collapsed `div=1` and `div=2` to the identical `divSafe=1`
(`int(1/2)=0` floors to `1` via the `max` guard; `int(2/2)=1` is already
`1`), so the widest and second-widest glitch steps produced the SAME
slice length — user confirmed this live ("the widest glitch and the
second widest glitch is the same length"). The reasoning that `div=1`
should be a no-op (nothing wider above it) was wrong: the user's actual
spec is every step, including the current widest, shifts one notch
wider — there is no floor.

Fixed, correctly: `sliceBlocks = max(1, int(beatBlocks / divSafe)) * 2`
with `divSafe` back to the original `max(1, DIV)` — multiply the
ALREADY-COMPUTED slice length by 2 instead of halving the divisor going
in. A multiply after the integer division can never collide the way
halving the divisor before it did (there's no floor step for it to hit).
`div=16` now lands exactly on old `div=8`'s slice length (matches round
1's intent for every step except the top), and `div=1` now genuinely
doubles past its own old value instead of staying pinned — no two
adjacent steps can ever produce the same `sliceBlocks` again. Verified:
native Faust compile clean standalone AND as part of the full production
`dsp/aloop.dsp` stack (this file is imported by `effects_runtime.dsp`'s
`microStage` and separately by the constants-baked `chain.dsp` reference
file — both pick up the fix automatically since neither declares its own
copy of the divisor logic).

## aloop <-> esp-idf-link mesh: paired invariants (change BOTH or the mesh splits)

aloop (Pi 4) and `../esp-idf-link` (ESP32, the "ticker" box) form ONE ad-hoc
single-AP mesh so Ableton Link's multicast peer discovery reaches every
device. There is no credential provisioning: exactly one device hosts the
open SSID `ticker` and everyone else joins it as a station. Every value
below exists in BOTH trees — changing it in one project alone silently
stops the two from meshing, with no error on either side.

| Invariant | aloop | esp-idf-link |
|---|---|---|
| Mesh SSID | `src/net/config/hostapd.conf` `ssid=ticker`, `wpa_supplicant.conf` `ssid="ticker"` | `main.cpp` `wifi_scan_best_bssid("ticker")` / `wifi_start_link_ap("ticker")` |
| Auth | open (`key_mgmt=NONE`; `wpa=` lines commented out) | `wifi_connect_sta("ticker", "")` (empty password) |
| AP address / DHCP | `192.168.4.1/24`, dnsmasq `.2-.20` | `esp_netif_set_ip_info` `192.168.4.1/255.255.255.0` |
| Channel | `hostapd.conf` `channel=6` | SoftAP ch6 |
| Link multicast | Link's own hardcoded `224.76.78.75:20808` | same (hardcoded in Link itself — cannot drift) |
| Link quantum | `link_bridge.cpp` `quantum = 16.0` | `main.h` `#define LINK_QUANTUM 16.0` |
| Start/stop sync | `link_bridge.cpp` `enableStartStopSync(true)` | `main.cpp` `g_link->enableStartStopSync(true)` |
| Host election | lowest MAC/BSSID wins | lowest MAC/BSSID wins |

`PHRASE_BEATS 64.0` in esp-idf-link is NOT the Link quantum — it is that
project's own transport-correction/SPP boundary (16 bars). It intentionally
differs from `LINK_QUANTUM 16.0` and does not affect phase agreement.

### Why the host election is MAC-ordered, not "host if scan found nothing"

Two devices cold-booting together can each scan before the other's AP
exists, so a naive "nothing found -> host" makes BOTH host, producing two
isolated L2 domains Link can never cross. Both projects instead hold for a
duration strictly monotonic in their own MAC (lowest MAC ≈ 0s, highest
≈ 6s), rescanning every second during the hold and joining the instant a
peer's AP appears. This gives a total order needing no cross-device
visibility during the hold. A genuinely lone device just hosts when its
own hold expires. Both supervisors additionally yield if another `ticker`
AP with a strictly-lower BSSID appears — but never while clients are
attached, since that would drop peers mid-session.

### `autoap.sh` was structurally broken before this pass (all WITNESSED)

Four independent defects, any one of which alone prevented meshing:
1. **Wrong SSID.** It hosted `aloop`, never `ticker` — the two projects
   hosted different networks and could never see each other.
2. **No joinable network.** `wpa_supplicant.conf` had ZERO active
   `network={}` blocks (the only `ssid=` line was inside a commented-out
   example), so `known_net_available()` could never associate and the
   script always fell through to hosting.
3. **AP-mode rescan matched a placeholder.** The switchback test built its
   pattern with `grep -oE 'ssid="[^"]*"' wpa_supplicant.conf` — grep does
   not skip comments, so the pattern file contained the literal
   `YourHomeWiFi`. In AP mode it scanned for a network that cannot exist.
4. **That same line was a hard syntax error on the device.** It used
   `grep -qFf <(...)` — process substitution is a bashism; `autoap.sh` is
   `#!/bin/sh` = busybox ash on Alpine. Verified: a POSIX shell rejects it
   with `Syntax error: "(" unexpected`. So the AP→STA path could not run at
   all. Keep this file POSIX-clean; check with `dash -n src/net/autoap.sh`.

### By-the-book Ableton Link integration checklist (both projects)

Derived from a full audit of both trees against Link's own header docs. Check
any change to either project's Link usage against this list.

- **Thread-correct session-state API.** `captureAppSessionState()` /
  `commitAppSessionState()` from any non-audio thread;
  `captureAudioSessionState()` / `commitAudioSessionState()` from the audio
  thread ONLY. aloop deliberately calls only the App variants and hands the
  audio thread a lock-free double-buffered `LinkSnapshot` instead (ADR-005) —
  that is a legitimate alternative, but it means audio-side beat/phase is up
  to one control-tick stale, so shortening that interval is the lever if
  phase accuracy is ever questioned.
- **`enableStartStopSync(true)` must be paired with actually reading
  `isPlaying()` AND with setting it.** A peer that enables the capability but
  only consumes transport is half-wired; one that never calls
  `setIsPlaying()` is invisible to peers' transport. aloop now does both
  (`LinkSnapshot::isPlaying` + `LinkBridge::setTransportPlaying`, published on
  every play-state edge from `ApcGrid`). esp-idf-link only CONSUMES, and that
  is correct for it — it has no local play/stop control at all, it translates
  the session's transport into outgoing MIDI Start/Stop for downstream gear.
- **The three notification callbacks** — `setNumPeersCallback(std::size_t)`,
  `setTempoCallback(double)`, `setStartStopCallback(bool)`. Link's own header
  documents each as "invoked on a Link-managed thread" and **"Realtime-safe:
  no"**, so a callback may do bounded logging / touch atomics and nothing
  else: never allocate, never lock, never reach into the audio thread.
- **Tempo authority.** `setTempo` rewrites the tempo for EVERY peer. Calling
  it unconditionally lets two devices fight. aloop's `proposeTempo` now
  refuses when peers are already present and aloop never set the tempo
  itself; esp-idf-link only sets tempo on an explicit LTMP command.
- **Quantum is a shared constant, not a local literal.** `kLinkQuantum` in
  `src/link/link_bridge.h` and `LINK_QUANTUM` in esp-idf-link's `main.h` are
  both `16.0` and must move together.
- **Peer count belongs in telemetry, not just a bool.** `synced` (peers>0)
  cannot distinguish 1 peer from 3, which the multi-device mesh test needs.
  aloop's `status.json` now carries `link.peers` and `link.playing`.
- **Interface readiness is a real race.** Link opens its multicast socket
  during `enable()`. esp-idf-link waits 500ms before constructing Link and
  re-asserts IGMP membership for ~10s after every connection, its own comment
  noting a single join at GOT_IP can race netif readiness so membership does
  not stick. On aloop the equivalent hazard was structural: the `aloop` and
  `autoap` OpenRC services both declared only `after local` and neither
  referenced the other, so Link could start before `wlan0` had an address.
  Fixed by `depend() { after local autoap; ... }` plus a bounded
  `waitForNetworkInterface()` before `link.start()`.

### Still unproven: AP-mode multicast forwarding on the Pi

Whether Link's multicast actually crosses between the Pi's own AP and its
associated stations on Broadcom `brcmfmac` is UNVERIFIED. `ap_isolate=0` is
set, which may be sufficient — but the ESP32's SoftAP needed a full
unicast relay beyond isolation (`wifi_config.cpp`'s
`link_multicast_relay_task` re-emits each Link datagram to the group, to
the AP's own IP, and unicast to every associated station, preserving the
original source IP because Link needs it for direct peer connect). Do NOT
port that relay to aloop speculatively — confirm the gap is real on real
hardware first (`docs/LINK-MESH-TESTING.md` Tests 1-3). If it is real, the
Linux-side fix is a networking-layer daemon fanning out to the dnsmasq
lease IPs alongside `hostapd`; it does not require touching
`link_bridge.cpp`.
