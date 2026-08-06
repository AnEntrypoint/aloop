import("stdfaust.lib");

SR = 48000.0;

FLANGEAMT = hslider("FLANGEAMT", 0.0, 0.0, 1.0, 0.01);

LFO_RATE_MIN_HZ = 0.2;
LFO_RATE_MAX_HZ = 3.0;
lfoRateHz = LFO_RATE_MIN_HZ + FLANGEAMT * (LFO_RATE_MAX_HZ - LFO_RATE_MIN_HZ);

DEPTH_MIN_MS = 1.0;
DEPTH_MAX_MS_BASE  = 10.0;
DEPTH_MAX_MS_CRAZY = 45.0;
depthMaxMs    = DEPTH_MAX_MS_BASE + FLANGEAMT * (DEPTH_MAX_MS_CRAZY - DEPTH_MAX_MS_BASE);
depthCenterMs = (DEPTH_MIN_MS + depthMaxMs) / 2.0;
depthSwingMs  = (depthMaxMs - DEPTH_MIN_MS) / 2.0;

FEEDBACK_MIN = 0.5;
FEEDBACK_MAX = 0.93;
feedback = FEEDBACK_MIN + FLANGEAMT * (FEEDBACK_MAX - FEEDBACK_MIN);

MAXD = 4096;

lfo = os.osc(lfoRateHz);
delayMs = depthCenterMs + lfo * depthSwingMs;
delaySamples = delayMs * SR / 1000.0;

flangeFC(x) = (loop ~ _) : fracRead
with {
    fracRead(w) = de.fdelay(MAXD, delaySamples, w);
    loop(w) = (x + feedback * fracRead(w)) : ma.tanh;
};

flangeMix(x) = x + FLANGEAMT * (flangeFC(x) - x);

process = _ <: flangeMix;
