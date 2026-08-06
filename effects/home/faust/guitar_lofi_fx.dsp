// aloop Core-3 "always-on" guitar + lofi-fx chain — LOFI feature, Core-3
// half. WITNESSED live on a real Pi 4 (this session): folding guitar+lofi-fx
// into the SAME always-on Core-1 home Faust program as a 3-way tent-weighted
// crossfade against the dub bank was a real, measured ~7 percentage-point
// core_busy regression (23%->30%) and directly caused audible continuous
// dropouts -- Faust has no runtime branching (select2/ba.if choose among
// ALREADY-COMPUTED signals, they never skip computing them), so 3 parallel
// effect chains computed every block regardless of which was "selected" was
// a real, structural cost with no in-Faust fix.
//
// REDESIGN (this file): guitar-fx and lofi-fx are NOT bank-exclusive with
// dub after all (confirmed directly: "all effects should be together" / "Both
// guitar and lofi-fx always stack together with dub") -- the 3 dub-fx/
// guitar-fx/lofi-fx buttons on the control surface were only ever meant to
// select which bank's stored knob VALUES the 7 physical knobs currently show/
// edit (ApcGrid's own existing per-bank storage), never which effects are
// AUDIBLE. So there is no "only compute the active bank" optimization
// available at all -- all 3 banks' effects genuinely process the signal in
// series, all the time. Given that, the real fix is not clever DSP, it's
// TOPOLOGY: move guitar+lofi-fx's compute OFF Core 1 (which already carries
// the 20-looper engine + the dub chain, always at its own real-time budget)
// and onto Core 3 via the SAME in-process LV2 host mechanism this codebase
// already uses for user-loaded effects (src/host/lv2_host.h) -- Core 3 was
// completely idle (0 user effects loaded) before this change, so this is
// genuinely free capacity, not a redistribution that costs something
// elsewhere. Runs AFTER Core 1's full dub-chain output (in series, per the
// "stack together" confirmation), loaded from /effects/home alongside the
// existing home-FX bundle so it's a fixed, permanent part of the home stack
// (never hot-swappable like /effects/user's plugins).
//
// Each of the 8 controls below is its OWN independent, always-live Faust
// zone (fx2/FLANGEAMT, fx2/TREMOLOAMT, etc, the "fx2/" prefix distinguishing
// them from the dub bank's existing fx/* zones) -- NOT shared/reused zone
// names the way the abandoned in-Faust crossfade design bound them to dub's
// 7 slots (REVAMT/DELAYAMT/etc), since guitar+lofi-fx's parameters must now
// hold their OWN persistent values simultaneously with dub's, not borrow
// dub's storage while "selected". ApcGrid's control-surface side still owns
// which bank's values the 7 physical knobs currently show/edit (unchanged
// UX), but every knob turn now writes directly into THIS file's own
// permanent zone for that parameter, live, regardless of which bank happens
// to be "selected" for editing at that moment.
import("stdfaust.lib");

// ---- Guitar-bank controls (top row: flange/tremolo/speed/phaser; bottom
// row: attack/release/sidechain-pump/compress -- attack/release live
// natively in sampler.h per this session's own confirmed design, sidechain-
// pump lives in dsp/loop.dsp's duckGain, so only 4 guitar-bank knobs are
// audio-chain controls reaching this file: flange/tremolo/speed/phaser, plus
// compress) ----
FLANGEAMT   = hslider("fx2/FLANGEAMT",   0.0, 0.0, 1.0, 0.01);
TREMOLOAMT  = hslider("fx2/TREMOLOAMT",  0.0, 0.0, 1.0, 0.01);
BANKSPEED   = hslider("fx2/BANKSPEED",   0.5, 0.0, 1.0, 0.01);
PHASERAMT   = hslider("fx2/PHASERAMT",   0.0, 0.0, 1.0, 0.01);
COMPRESSAMT = hslider("fx2/COMPRESSAMT", 0.0, 0.0, 1.0, 0.01);

// ---- Lofi-fx-bank controls (top row: bitcrush/vinyl/flutter/samplerate;
// bottom row: 4 granulator controls, which live natively in sampler.h per
// this session's own confirmed design, not audio-chain controls here) ----
BITCRUSHAMT = hslider("fx2/BITCRUSHAMT", 0.0, 0.0, 1.0, 0.01);
VINYLAMT    = hslider("fx2/VINYLAMT",    0.0, 0.0, 1.0, 0.01);
FLUTTERAMT  = hslider("fx2/FLUTTERAMT",  0.0, 0.0, 1.0, 0.01);
SRRAMT      = hslider("fx2/SRRAMT",      0.0, 0.0, 1.0, 0.01);

// Each effect stage reads its OWN zone directly (no component() parameter
// remapping needed now, unlike the abandoned in-Core-1 design) -- every
// stage file already declares its natural zone name (FLANGEAMT, TREMOLOAMT,
// etc), so binding here is a straight 1:1 passthrough of this file's own
// hslider values into each imported stage's identically-named control.
flangerStage    = component("flanger.dsp")[ FLANGEAMT=FLANGEAMT; ];
tremoloStage    = component("tremolo.dsp")[ TREMOLOAMT=TREMOLOAMT; BANKSPEED=BANKSPEED; ];
phaserStage     = component("phaser.dsp")[ PHASERAMT=PHASERAMT; BANKSPEED=BANKSPEED; ];
compressorStage = component("compressor.dsp")[ COMPRESSAMT=COMPRESSAMT; ];
bitcrushStage   = component("bitcrush.dsp")[ BITCRUSHAMT=BITCRUSHAMT; ];
vinylStage      = component("vinyl.dsp")[ VINYLAMT=VINYLAMT; ];
flutterStage    = component("flutter.dsp")[ FLUTTERAMT=FLUTTERAMT; ];
samplerateStage = component("samplerate.dsp")[ SRRAMT=SRRAMT; ];

// Both chains stacked together IN SERIES, per the confirmed "all effects
// together" design -- guitar chain first (flanger->tremolo->phaser->
// compressor), then lofi-fx chain (bitcrush->vinyl->flutter->samplerate).
// At every control's default (0.0), each stage is independently verified
// byte-exact passthrough (see each stage file's own header comment for its
// verification), so the WHOLE chain is byte-exact passthrough at all
// defaults by construction (composing passthrough stages in series stays a
// passthrough).
process = _ : flangerStage : tremoloStage : phaserStage : compressorStage
            : bitcrushStage : vinylStage : flutterStage : samplerateStage;
