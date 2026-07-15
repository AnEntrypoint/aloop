// aloop loop engine — in Faust. Matches the real hardware setup:
//   * 20 INDEPENDENT loopers (each its own record buffer + play head),
//   * record / play only — NO OVERDUB (the hardware has no overdub control),
//   * each looper's play RATE tracks manual half/double-speed AND the Ableton
//     Link grid (TRUE varispeed: tape-style pitch+duration change together),
//   * the 20 looper outputs sum to the engine output.
//
// TRUE VARISPEED (this generation): the WRITE side is still a simple
// monotonic-index ring write (record replaces the loop, no overdub, no
// read-modify-write of the SAME cell -- this is why the design compiles).
// The READ side is now a genuinely SEPARATE fractional accumulator
// (`readPos ~ _`, ordinary Faust signal recursion) advancing by `effSpeed`
// SAMPLES PER SAMPLE (not per block), wrapping at the loop's own length,
// driving TWO `rwtable` reads (floor/ceil index) linearly blended by the
// fractional part. This is NOT a read-modify-write: the read never writes
// back into the table at (or near) the position it read -- it only reads,
// exactly like looking up two arbitrary indices in an array -- so it does
// not hit the RMW rejection that blocked the EARLIER (different, now
// abandoned) attempt at a preserve-on-hold ADDRESSABLE playhead (which
// needed to write the read-back value into the SAME cell, a genuine RMW
// Faust's evaluator rejects, "endless evaluation cycle"/"stack overflow in
// eval", witnessed across 4 CI codegen attempts, see docs/DECISIONS.md ADR +
// .wfgy/lessons.md). Mark-point restart (SET/CLEAR_LOOP_START) and immediate
// re-trigger (LOOP_IMMEDIATE) still are NOT wired (a deliberate model
// difference documented in docs/COMMAND-SURFACE.md, unrelated to varispeed).
//
// Ported from looper's real mechanism (C:\dev\looper\loopClipUpdate.cpp's
// loopClip::update(), m_playPos/m_playRate; Looper.h's setMasterBlocks();
// loopMachine.cpp:637-663): `effectiveRate = m_playRate * g_globalSpeedMul`,
// where m_playRate = m_nativeBlocks/currentMasterBlocks (recomputed
// ABSOLUTELY, never accumulated, from the loop's ORIGINAL recorded length
// vs whatever length Link's current tempo implies) and g_globalSpeedMul is
// PURELY the manual half/double-speed button state (1.0/0.5/2.0), entirely
// separate from Link. aloop's native shell (audio_thread.cpp) computes this
// SAME product every block and pushes it in as `effSpeed`, a single
// process()-level SIGNAL INPUT (see the ROOT CAUSE comment below for why
// this must never become a UI-declared control).

import("stdfaust.lib");

SR       = 48000.0;
MAXLEN   = 48000 * 60;    // 60 s max loop per looper
NLOOPERS = 20;            // 20 independent loopers (the hardware setup)

// ---- one independent looper ----
// Controls (native shell drives these from MIDI + Link), indexed per looper i:
//   rec[i]   : 1 while recording looper i (replaces its loop; NO overdub)
//   play[i]  : 1 = looper i playing
//   len[i]   : looper i loop length in samples (from Link tempo for varispeed)
//   vol[i]   : looper i output level
// A looper is a one-loop-length feedback delay: while recording, write the live
// input; else recirculate (hold) the loop. There is no overdub path.
// One looper. The control labels use Faust's "[N]" group-index substitution so
// each of the 20 instances gets its own rec/play/len/vol control ("looper0/rec"
// … "looper19/rec"). Record replaces the loop; else it holds — NO overdub.
// Global controls (shared across all loopers): clear wipes every loop; speedMul
// scales the effective loop length for the momentary half/double-speed commands.
//
// ROOT CAUSE (2nd generation, THE REAL ONE): 382e775 hoisted clearAllGlobal/
// speedMulGlobal's button()/hslider() DECLARATIONS to file scope, outside the
// par(i, NLOOPERS, vgroup(...)) textually, and threaded them into oneLooper
// as ordinary function parameters -- but this did NOT stop Faust's compiler
// from still emitting 20 separate UI zones. WITNESSED via inspecting the
// generated C++ (build/loop.cpp): `grep -c '"speed"'` and `grep -c '"clear"'`
// each returned 20, and every occurrence is INSIDE its own "looper N" vgroup
// (`openVerticalBox("looper 0"); ... addHorizontalSlider("speed", ...)`),
// exactly like before the "fix". Root cause: Faust's par(...) combinator is
// a CODE-GENERATING combinator, not a runtime loop -- passing an expression
// (clearAllGlobal, itself a button() box) as an argument to oneLooper, which
// par() instantiates 20 times, means that expression is substituted/inlined
// at each of the 20 call sites. Each inlined copy of the button()/hslider()
// box gets its own UI declaration, and since the call site is lexically
// inside vgroup("looper%2i"), each inlined declaration lands inside that
// specific looper's group too. Hoisting the TEXT of the declaration outside
// the par loop doesn't matter: what determines UI-zone identity in Faust is
// where the box is ELABORATED (i.e. every place the expression is used),
// not where it is textually written once in the source. There is no Faust
// mechanism for "declare this UI control once, reference the same zone from
// many call sites" when the reference crosses a par() replication boundary --
// the language does not have that concept; par() genuinely duplicates
// whatever signal graph (including UI primitives) sits in its body.
//
// REAL FIX: stop declaring clear/speed as Faust UI controls (hslider/button)
// at all. Make them plain SIGNAL inputs to process() instead -- exactly the
// same technique already proven for prevFiltIn below (a native block-rate
// value pushed in from audio_thread.cpp's fins[] array, never a UI zone).
// A signal input threaded through par() is NOT re-elaborated per instance
// the way a UI-primitive box is -- it's just a wire, so every oneLooper
// instance genuinely reads the exact same sample-accurate value every block,
// with no zone-lookup, no FaustUI::set() suffix-matching, and no possibility
// of the compiler duplicating it. audio_thread.cpp now writes cmd/clearall
// and the computed speed multiplier directly into fins[2]/fins[3] (constant
// across the block) instead of calling fui.set("clear"/"speed", ...).

// prevFiltIn: previous block's FULLY-EFFECTED mix output (audio_thread.cpp's
// prevFiltOut, dsp/aloop.dsp's recordTap/4th process() output), native
// one-block-lag fold-in -- the same proven technique as the old glitchIn tap
// (see dsp/aloop.dsp's top-of-file comment for why this is a DEDICATED
// record-only input rather than being folded into `in`/`fin`: adding it to
// the engine's live/dry input would make it flow into `fx` again next block,
// re-entering every stage -- the WITNESSED feedback whine, see
// audio_thread.cpp's "REVERTED here" comment, now would apply to the WHOLE
// fx chain not just microStage). Routed ONLY into the record capture term
// below: recording now ALWAYS captures the fully-effected signal
// (pitch/delay/reverb/microrepeat/filters), matching the user's explicit
// requirement ("all effects should record"), not just raw dry input.
// REPLACES the old (in + glitchIn) composition entirely -- `in` (raw pre-fx
// input) is no longer part of the record term at all, since prevFiltIn one
// block later already contains everything `in` would have contributed (this
// block's `in` becomes part of next block's filtOut via the normal fx path),
// and the old glitchIn term is redundant now that prevFiltIn already carries
// post-glitch content (effects_runtime.dsp: microStage feeds both
// filterStage and rawGlitchTap, so microStage's output is upstream of and
// already baked into filtOut/recordTap).
oneLooper(in, prevFiltIn, clearAll, effSpeed) = out : attachLevel
with {
    recN  = button("rec");
    playN = checkbox("play");
    lenN  = hslider("len", 48000, 64, MAXLEN, 1);
    volN  = hslider("vol", 1.0, 0.0, 1.0, 0.001);
    eraseN = button("erase");   // per-looper wipe (hardware ERASE_TRACK 0x60)
    // wipe this loop when EITHER the global clear or this looper's erase is held.
    wipe   = max(clearAll, eraseN);

    // ---- WRITE side: unchanged semantics from the old de.fdelay ring ----
    // Monotonic write index, wraps at lenN (the CURRENT loop length -- the
    // Link-driven length update, same as before). No overdub: writing
    // prevFiltIn*recN REPLACES whatever was in that cell; when not
    // recording, nothing is written to this index (record content persists
    // until the next recording pass or an explicit wipe below). This is
    // structurally the exact same write-index behavior de.fdelay's internal
    // ring implicitly performed -- only now it's an explicit rwtable, so the
    // read side can be independent.
    wrapLen = max(1, int(lenN));
    writeIdxStep(prev) = (prev + 1) % wrapLen;
    writeIdx = writeIdxStep ~ _;
    // record: capture the PREVIOUS block's fully-effected mix (prevFiltIn),
    // one-block-lag, so every recording is ALWAYS effected (pitch/delay/
    // reverb/microrepeat/filters) -- matching the user's explicit
    // requirement, not just live input passed through raw. This also
    // captures glitch/microrepeat content (matching looper's "stutter
    // becomes ... the record source", loopMachine.cpp:806-833) since
    // prevFiltIn already contains microStage's output one block later, and
    // captures SHIFT/glitch-held loop content too, since the native fold
    // already routes that content through `fx` into filtOut before this tap
    // is taken. prevFiltIn never touches `in`/dry, only this record term, so
    // it structurally cannot re-enter `fx` on any later block. `wipe` zeroes
    // the write too (a wiped/erased loop must not silently resurrect old
    // ring content next read pass), matching the old hold*(1-wipe) gating.
    writeVal = prevFiltIn * recN * (1.0 - wipe);
    ring = rwtable(MAXLEN, 0.0, writeIdx, writeVal, readIdx0);

    // ---- READ side: the NEW genuinely-independent fractional accumulator
    // driving true varispeed (tape-style pitch+duration change together).
    // effSpeed is a plain process()-level SIGNAL INPUT (see ROOT CAUSE
    // comment above oneLooper for why it must never be a UI hslider/button
    // threaded through par() -- would be re-elaborated into 20 duplicate
    // zones, exactly the varispeed/clear-all bug class already fixed once
    // this session). Clamp to a sane nonzero range so a pathological
    // Link-tempo ratio or manual speed can never stall (0) or explode
    // (runaway) the read accumulator.
    speedClamped = max(0.1, min(8.0, effSpeed));
    // wrap into [0, wrapLen) using the CURRENT wrapLen every sample -- this
    // handles both forward overflow (speed>1) and, defensively, negative
    // values (never produced today since speedClamped>0, but keeps the wrap
    // well-defined if that ever changes).
    readPosStep(prev) = wrapReadPos(prev + speedClamped)
    with { wrapReadPos(p) = p - floor(p / float(wrapLen)) * float(wrapLen); };
    readPos = readPosStep ~ _;
    // TEMPORARY diagnostic (tracked for removal): re-investigating "plays a
    // part of the loop once, does not repeat" -- expose readPos/wrapLen as
    // hbargraph OUTPUTS (read-only telemetry, same pattern as levelMeter
    // below) so audio_thread.cpp can log them and confirm whether readPos is
    // genuinely wrapping back to a valid range, or drifting/staying stuck
    // outside [0, wrapLen) after a fresh recording changes wrapLen.
    readPosDiag = readPos : hbargraph("readposdiag", 0.0, 999999.0);
    wrapLenDiag = float(wrapLen) : hbargraph("wraplendiag", 0.0, 999999.0);
    readIdx0 = int(readPos) % wrapLen;             // floor tap
    readIdx1 = (readIdx0 + 1) % wrapLen;             // ceil tap, wrapped
    readFrac = readPos - floor(readPos);
    // Second read of the SAME ring at the ceil index for linear
    // interpolation. Both reads are plain lookups (no write), so this is
    // NOT a read-modify-write -- the ring's only writer is writeIdx/writeVal
    // above, entirely independent of where the read side is looking.
    ringCeil = rwtable(MAXLEN, 0.0, writeIdx, writeVal, readIdx1);
    delayed = ring + (ringCeil - ring) * readFrac;   // linear-interpolated read
    // hold/recirculate exactly as before -- the wrap-around READ POSITION is
    // what changes speed now (not effLen), so hold's own gating (not
    // recording, not wiped) is unchanged from the old design.
    hold = delayed * (1.0 - recN) * (1.0 - wipe);
    // record (live monitoring term): the OLD de.fdelay-ring design's `out`
    // was `step = record+hold` fed straight back into the delay -- since
    // write and read were the SAME point in that ring, recording produced
    // zero-added-latency monitoring (you hear yourself as you record) for
    // free. The new independent read/write heads lose that automatically
    // (the read position is generally somewhere else in the ring while
    // writeIdx advances), so this term reinstates it explicitly: while
    // actively recording, `out` carries the live writeVal directly, exactly
    // matching the old ring's same-point read=write behavior.
    record = writeVal;
    loopSig = record + hold;
    out = loopSig * playN * volN;
    // LEVEL meter: an hbargraph UI OUTPUT (never fui.set() from ParamStore --
    // read-only via fui.get(), same pattern as the existing rec/play/vol
    // telemetry reads in audio_thread.cpp), fed via Faust's attach() idiom so
    // the meter signal rides along the real audio signal without adding a
    // second audible output channel. Raw abs-peak magnitude in the looper's
    // own [0,1] float range (not dB, not looper's raw s32 scale) -- matching
    // looper's vuLow/vuMid/vuHigh thresholds means the C++ side documents its
    // own equivalent thresholds in aloop's normalized range (see apc_leds.cpp).
    // ba.slidingMax gives a fast-rise/slow-decay envelope (a real "peak
    // meter" shape rather than a raw instantaneous sample, which would
    // flicker the LED color every block) over a ~4096-sample (~85ms @48kHz)
    // window.
    levelMeter = hbargraph("level", 0.0, 1.0);
    // ba.slidingMax(n, maxN): sliding window max over the last n samples,
    // maxN is the compile-time buffer-size bound (n <= maxN). Window ~4096
    // samples (~85ms @48kHz), bound equal to the window since it's a fixed
    // compile-time constant here, not runtime-variable.
    attachLevel(x) = attach(x, abs(x) : ba.slidingMax(4096, 4096) : levelMeter);
};

// The engine outputs (dry-thru, loop-sum) SEPARATELY rather than pre-summed, so
// the caller (aloop.dsp) can implement looper's SHIFT-held monitor-fold: fold
// the loop-sum into the effect chain's input while complementarily suppressing
// the dry loop contribution at the final mix (loopMachine.cpp:709-730's
// g_fold/g_dry crossfade). `par` with a "%2i"-labelled vgroup gives each
// instance its own addressable controls (looper00/rec … looper19/rec).
//   in -> [ thru , (looper0 + looper1 + … + looper19) ]
// vgroup label "looper%2i" → Faust substitutes the par index, giving group names
// "looper 0" … "looper19" (a space for single digits). The native shell's
// targetToZone normalizes to this exact form so each looper is addressable.
// Second process() input (prevFiltIn, previous block's fully-effected mix
// output) is broadcast to every looper's record-only tap (see oneLooper's
// comment) -- it never appears in the dry-thru output (only `in` does), so
// the fully-effected signal is recordable but never re-enters the live/dry
// path (which would flow back into `fx` next block).
// clearAllGlobal/speedMulGlobal are evaluated ONCE here, OUTSIDE any vgroup,
// then passed as ordinary parameters into every oneLooper instance -- this is
// what actually makes them single shared zones (see oneLooper's comment above
// for why referencing them by bare name from INSIDE the vgroup-wrapped par
// failed to do this).
// clearAll/effSpeed are now genuine process() SIGNAL inputs (see the ROOT
// CAUSE comment above oneLooper) -- plain wires, not UI-declared button()/
// hslider() boxes, so par()'s per-instance code generation cannot duplicate
// them into 20 separate zones. They are broadcast identically to every
// oneLooper instance the same way prevFiltIn already is. effSpeed REPLACES
// the old speedMul: it is audio_thread.cpp's precomputed product of the
// manual half/double-speed multiplier AND the Link-tempo-driven ratio
// (recordedBpm/currentLinkBpm), matching looper's
// `effectiveRate = m_playRate * g_globalSpeedMul` exactly (see top-of-file
// comment) -- one combined signal, since Faust's read accumulator only ever
// needs the FINAL rate, not the two factors separately.
loopEngine(in, prevFiltIn, clearAll, effSpeed) = in, (par(i, NLOOPERS, vgroup("looper%2i", oneLooper(in, prevFiltIn, clearAll, effSpeed))) :> _);

process(in, prevFiltIn, clearAll, effSpeed) = loopEngine(in, prevFiltIn, clearAll, effSpeed);   // (dry, loopSum) — two outputs, see aloop.dsp's fold mix
