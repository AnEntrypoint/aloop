// aloop home stack — the loop engine + the dubfx effect chain as ONE Faust
// program (both are Faust, so they compose directly). This is the fixed home
// stack that runs on Core 1; the user's swappable effect is a separate LV2 on
// Core 3 (loaded by the in-process host). See docs/ARCHITECTURE.md.
//
// Signal: input -> loop.dsp (20 independent record/play loopers, no overdub) -> the dubfx
// effect chain (pitch/delay/reverb/microrepeat/filters) -> output.
//
// Composing this way means the ENTIRE home audio path is one Faust compile — the
// maintainability win: change a knob mapping or a stage in Faust, rebuild, done.
// No hand-written C++ DSP anywhere in the home stack.

import("stdfaust.lib");

// The loop engine (this dir).
loop = component("loop.dsp");
// The verified dubfx effect chain (vendored under effects/home/faust/).
fx   = component("../effects/home/faust/chain.dsp");

// Home stack = loop then effects. Mono in/out.
process = loop : fx;
