import("stdfaust.lib");
SR = 48000;
MAXLEN = 48000 * 4;

rec = button("rec");
LEN = 48000;   // fixed loop length for the probe

// A phase counter 0..LEN-1 that wraps. `%` on an integer accumulator.
phase = (+(1) ~ _) : -(1) : int : \(p).(p % LEN);

process(in) = out
with {
    idx = phase;
    // rwtable(size, initval, writeIndex, writeSignal, readIndex).
    // Write live input at idx while recording; read the same ring at idx.
    out = rwtable(MAXLEN, 0.0, idx, in*rec, idx);
};
