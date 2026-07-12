// aloop loop engine — in Faust. Matches the real hardware setup:
//   * 20 INDEPENDENT loopers (each its own record buffer + play head),
//   * record / play only — NO OVERDUB (the hardware has no overdub control),
//   * a BUFFER + PLAYHEAD model (rwtable) — the same shape as the hardware, so it
//     supports an addressable read position: mark-point (SET/CLEAR_LOOP_START) and
//     immediate re-trigger (LOOP_IMMEDIATE), which a feedback-delay ring cannot do,
//   * each looper's play position tracks the Ableton Link grid (varispeed sync),
//   * the 20 looper outputs sum to the engine output.
//
// WHY a buffer+playhead (not a feedback-delay ring): the earlier de.fdelay ring was
// cycle-free and simple, but it has NO addressable playhead — you cannot jump the
// read position to a mark point or re-trigger all loops to a start. The hardware's
// loopMachine IS a buffer+playhead and its control surface actively drives those
// commands (apcKey25Notes.cpp SHIFT+pad → LOOP_IMMEDIATE; SET/CLEAR_LOOP_START).
// So the engine is an rwtable: write the live input at a recording write-pointer,
// read at a free-running phase read-pointer. Crucially the read/write POINTERS are
// plain integer phase accumulators (feedback only through `_`, never through the
// table), so there is no read-modify-write cycle — this is what compiles cleanly
// (witnessed originally in loop_min.dsp: ftbl[wp]=rec*in ; out=ftbl[rp]).

import("stdfaust.lib");

SR       = 48000.0;
MAXLEN   = 48000 * 60;    // 60 s max loop per looper (rwtable size)
NLOOPERS = 20;            // 20 independent loopers (the hardware setup)

// Global controls (shared across all loopers): clear wipes every loop; speedMul is
// the varispeed read-rate for the momentary half/double-speed commands; loopNow is
// the synchronized re-trigger (LOOP_IMMEDIATE) that jumps every read head to its
// mark point.
clearAll = button("clear");
speedMul = hslider("speed", 1.0, 0.25, 4.0, 0.001);   // read rate: 0.5=half, 2=double
loopNow  = button("loopnow");                          // LOOP_IMMEDIATE: jump to mark

// ---- one independent looper (buffer + playhead) ----
// Controls (native shell drives these from MIDI + Link), indexed per looper i:
//   rec[i]      : 1 while recording (writes live input into the buffer; NO overdub)
//   play[i]     : 1 = playing (gates the read output)
//   len[i]      : loop length in samples (from Link tempo for varispeed)
//   vol[i]      : output level
//   erase[i]    : wipe this loop
//   markset[i]  : SET_LOOP_START — capture the current read position as the mark
//   markclear[i]: CLEAR_LOOP_START — reset the mark to 0 (loop's natural start)
// The control labels use Faust's "%2i" group-index substitution so each of the 20
// instances gets its own addressable controls ("looper 0/rec" … "looper19/rec").
oneLooper(in) = loopOut * playN * volN
with {
    recN      = button("rec");
    playN     = checkbox("play");
    lenN      = hslider("len", 48000, 64, MAXLEN, 1);
    volN      = hslider("vol", 1.0, 0.0, 1.0, 0.001);
    eraseN    = button("erase");      // per-looper wipe (hardware ERASE_TRACK 0x60)
    marksetN  = button("markset");    // SET_LOOP_START (setMarkPoint)
    markclearN= button("markclear");  // CLEAR_LOOP_START (clearMarkPoint)

    L    = max(1.0, lenN);            // active loop length in samples (>=1)
    wipe = max(clearAll, eraseN) > 0.5;   // clear this loop's stored audio when held

    // WRITE head: a free-running phase advancing 1 sample/sample, wrapping at L — a
    // plain phasor, NO mark and NO table in its feedback path. Record writes here.
    whStep(prev) = ma.modulo(prev + 1.0, L);
    wh = whStep ~ _ : int;

    // READ phase: an INDEPENDENT free-running phase advancing by speedMul (varispeed
    // read rate — half/double speed) each sample, wrapping at L. Separate from the
    // write head, so playback can read faster/slower than the write. Also a plain
    // phasor (no mark, no table in its feedback) → no cycle.
    rphStep(prev) = ma.modulo(prev + speedMul, L);
    rph = rphStep ~ _ ;

    // MARK point (restart origin): a one-sample sample-and-hold driven ONLY by the
    // read phase (never by the table output), so there is no mutual recursion.
    // markset latches the current read phase; markclear latches 0; else hold. mark
    // is an additive read OFFSET — re-trigger is a read-position shift, not a head
    // mutation, so nothing feeds back and there is no eval cycle.
    markStep(prev) = select2(marksetN > 0.5,
                             select2(markclearN > 0.5, prev, 0.0),
                             rph');
    mark = markStep ~ _ ;

    // LOOP_IMMEDIATE re-trigger: while loopNow is held, read from the mark point
    // (ignore the running read phase) — a synchronized snap to the loop start/mark.
    // Otherwise read at (read-phase + mark) so a set mark shifts the loop origin.
    ridx = int(ma.modulo(select2(loopNow > 0.5, rph + mark, mark), L));

    // WRITE-BACK is the key to record-vs-hold WITHOUT a table cycle. While recording
    // write the live input (replaces — NO overdub); else write back the stored value
    // so the loop is preserved; on wipe write 0 (silence). We feed back `stored'`
    // (stored DELAYED one sample) so there is provably no instantaneous table cycle
    // — the write head has moved on by the time the value is re-stored, and the
    // 1-sample skew is inaudible for a hold/preserve write.
    wdata = select2(wipe, select2(recN > 0.5, stored', in), 0.0);
    stored = rwtable(MAXLEN, 0.0, wh, wdata, ridx);
    loopOut = stored;
};

// The engine: live input (thru) + the sum of 20 INDEPENDENT loopers. `par` with
// a "%2i"-labelled vgroup gives each instance its own addressable controls
// (looper00/rec … looper19/rec). All loopers see the same live input; the input
// is fanned to each looper and to the dry thru, then everything sums.
//   in -> [ thru | looper0 | looper1 | … | looper19 ] -> sum
// vgroup label "looper%2i" → Faust substitutes the par index, giving group names
// "looper 0" … "looper19" (a space for single digits). The native shell's
// targetToZone normalizes to this exact form so each looper is addressable.
loopEngine = _ <: (_ , par(i, NLOOPERS, vgroup("looper%2i", oneLooper))) :> _ ;

process(in) = loopEngine(in);
