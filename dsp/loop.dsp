import("stdfaust.lib");
SR     = 48000.0;
MAXLEN = 48000 * 60;

rec     = button("rec");
overdub = button("overdub");
play    = checkbox("play");
loopLen = hslider("loopLen", 48000, 64, MAXLEN, 1);
loopVol = hslider("loopVol", 1.0, 0.0, 1.0, 0.001);

// Canonical cycle-free feedback looper: the loop signal recirculates through a
// one-loop-length delay. `_ ~ f` feeds the output back through f. Here f delays
// by loopLen and mixes new input per the mode. The `~` provides the required
// implicit one-sample delay; de.fdelay provides the loop-length delay.
loopEngine(in) = loopSig * play * loopVol
with {
    // feedback function: given the recirculating loop, produce the next loop.
    step(loop) = record + layer + hold
    with {
        delayed = de.fdelay(MAXLEN, max(1.0, loopLen), loop);
        record  = in * rec;
        layer   = (delayed + in) * overdub * (1.0 - rec);
        hold    = delayed * (1.0 - rec) * (1.0 - overdub);
    };
    loopSig = step ~ _;
};

process(in) = in + loopEngine(in);
