// Tape delay — exact port of apcEffectsProcessor::processSends (delay portion).
//
// C++ per-sample (mono):
//   newDelay = curLen + (target - curLen)*0.0001     // delay length slews
//   d        = linInterp(ring, writePos - newDelay)  // fractional read (past)
//   ring[writePos] = x + d*feedback                  // feedback comb write
//   writePos++;  out = x + d*amount                  // wet mix
// feedback = amount*1.05; curLen starts at 0 (cold ring).
//
// Feedback comb: w[n] = x[n] + fb*D(w)[n], D = linear-interp fractional delay
// of `newDelay` samples. Output y = x + amount*D(w). The tap is >=1 sample in
// the past, so the `~` loop is well-formed.

import("stdfaust.lib");

SR   = 48000.0;
MAXD = 96000;
SLEW = 0.0001;

DELAYAMT = 0.0;
TIME     = 0.5;

MIN_DELAY_MS = 1000.0 / SR;

targetSamples(t) = max(1.0, min(MAXD - 1.0, (t*(1000.0 - MIN_DELAY_MS) + MIN_DELAY_MS) * SR / 1000.0));

curStep(target, c) = c + (target - c)*SLEW;
curDelayRec(target) = c letrec { 'c = curStep(target, c); };
newDelayFrom(target, c) = c + (target - c)*SLEW;

// Feedback comb: w = x + fb*D(w); output y = x + amount*D(w).
// The recursive block emits w and feeds it back; a trailing fractional delay
// D applied to the loop output yields d = D(w) (same computation, well-formed
// because the tap is >= ~1 sample in the past).
// Exact 2-point linear interpolation delay matching readDelayInterp:
//   idx0 = floor(pos); fr = pos - idx0; out = buf[idx0]*(1-fr) + buf[idx1]*fr
// In Faust, reading signal w at fractional delay `len`: integer delay int(len)
// is idx0's tap, int(len)+1 is idx1's tap. de.delay(maxd, n, sig) = n-sample
// integer delay. This reproduces the C++ interpolation exactly (no library
// convention ambiguity).
fracDelay(len, w) = tap0*(1.0 - fr) + tap1*fr
with {
    i0  = int(len);
    fr  = len - float(i0);
    tap0 = de.delay(MAXD, i0, w);
    tap1 = de.delay(MAXD, i0 + 1, w);
};

delayFC(amount, t, x) = x + amount * d
with {
    fb     = amount * 1.05;
    target = targetSamples(t);
    cd     = curDelayRec(target);        // currentDelay[n], letrec state init 0
    len    = newDelayFrom(target, cd);   // read tap = newDelay[n]
    d   = (loop ~ _) : fracDelay(len)
    with {
        loop(w) = x + fb * fracDelay(len, w);
    };
};

process = delayFC(DELAYAMT, TIME);
