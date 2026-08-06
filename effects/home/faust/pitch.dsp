import("stdfaust.lib");

pitchTick = ffunction(float dubfx_pitch_tick(float, float, float, float), "pitch_ffi.h", "");

SEMIS   = 0.0;
FORMANT = 0.0;
ENGAGED = 0.0;

scale = pow(2.0, SEMIS / 12.0);

process = _, scale, FORMANT, ENGAGED : pitchTick;
