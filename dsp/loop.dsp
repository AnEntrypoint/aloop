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
    wipe = max(clearAll, eraseN);     // clear this loop's stored audio when held

    // WRITE pointer: an integer sample counter that advances only while recording,
    // wrapping at L. Feedback is a plain +1 accumulator through `_` (no table).
    wp = (+(recN) : %(L)) ~ _ : int;
    // what to store: the live input while recording, else 0 into the wiped slot so
    // a held wipe overwrites the loop with silence as the write head passes (and,
    // on record, replaces — NO overdub). When neither recording nor wiping, we
    // still write the existing sample back (read-through) so the loop is preserved.
    wdata = in * recN;

    // READ pointer: a resettable phase accumulator. Each sample it advances by
    // speedMul (the varispeed read rate) and wraps at L via fmod; on `reset` it
    // jumps to the mark point. This is the witnessed cycle-free form (a plain 1-into
    // -1 recursion through `_`, the table is never in the pointer's feedback path).
    //   reset = loopNow (LOOP_IMMEDIATE) OR markset (jumping to a just-set mark).
    // ba.if(cond, then, else) selects without branching.
    reset = max(loopNow, marksetN);
    rp = ( \(prev). ba.if(reset, mark, ma.frac((prev + speedMul) / L) * L) ) ~ _ ;

    // MARK point (restart origin): a sample-and-hold state. markset latches the
    // current read position; markclear latches 0; else it holds. Fed by rp with a
    // 1-sample delay (rp' ) to avoid a same-sample cycle with the reset above.
    mark = ( \(prev). ba.if(marksetN, rp', ba.if(markclearN, 0.0, prev)) ) ~ _ ;

    // The buffer: write wdata at wp while recording, read at rp. On wipe, force the
    // output to 0 so a held clear/erase silences immediately. rwtable(size,init,
    // writeIdx, writeSig, readIdx). Read index truncated to an int sample position.
    stored = rwtable(MAXLEN, 0.0, wp, wdata, int(rp));
    loopOut = stored * (1.0 - wipe);
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
