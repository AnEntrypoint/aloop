declare name "PitchStageBrowserStub";
declare author "aloop";
declare license "GPLv3";
declare description "Browser-only substitute for effects/home/faust/pitch.dsp. The real file's pitchTick calls dubfx_pitch_tick via ffunction, which links against pitch_ffi.h's real hardware SNAC/solad engine -- no Faust JIT (this codebase's own DawDreamer verification harness included, see AGENTS.md's 'DawDreamer verification harness' section) can link an ffunction-declared external symbol, and a browser libfaust-wasm JIT has the identical restriction. This file keeps the exact same external interface pitchStage's caller depends on (one audio input, one audio output, module-level SEMIS/FORMANT/ENGAGED overridden by the same component()[SEMIS=SEMIS;...] environment substitution effects_runtime.dsp already uses) so every surrounding control target (fx/pitch, fx/pitchbend, fx/pitchbend_engaged) reaches a real, functioning Faust pitch-shift instead of a silent passthrough -- ef.transpose is the same crossfaded-delay-line shifter multitranspose.dsp's own local xpose reimplements for polyphonic voice-count reasons documented in AGENTS.md's Faust-stdlib-buffer entry; a single mono instance here has no such buffer-count pressure, so the stdlib call is used directly. FORMANT is accepted for interface parity but not applied -- formant-independent shifting is pitch_ffi.h's own hardware-only behavior and out of reach the same way the pitch algorithm itself is.";

import("stdfaust.lib");

SEMIS   = 0.0;
FORMANT = 0.0;
ENGAGED = 0.0;

pitchWindowSamples = 30.0 * ma.SR / 1000.0;
pitchXfadeSamples  = 20.0 * ma.SR / 1000.0;

shifted(x) = x : ef.transpose(pitchWindowSamples, pitchXfadeSamples, SEMIS);

process(x) = select2(ENGAGED > 0.5, x, shifted(x));
