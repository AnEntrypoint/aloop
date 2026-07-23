// aloop APC Key25 grid engine — public interface (impl in apc_grid.cpp).
//
// Ports looper's apcKey25Notes.cpp pad-grid + tap/hold + preset logic onto
// aloop's ParamStore/named-target model. This is the part of the real hardware
// surface config/controls.conf's flat note table cannot express: the 5x8 grid
// (row*8+col), tap-vs-hold disambiguation, and 10-slot preset capture/recall.

#ifndef ALOOP_APC_GRID_H
#define ALOOP_APC_GRID_H

#include <cstdint>
#include "midi.h"

namespace aloop {

class Sampler;      // dsp/sampler/sampler.h -- forward-declared, ApcGrid only ever holds a pointer
class AudioThread;  // dsp/audio_thread.h -- forward-declared, for ARM-quantization telemetry reads

constexpr int kApcRows = 5;
constexpr int kApcCols = 8;
constexpr int kLooperCount = 20;
constexpr int kPresetCount = 10;
constexpr unsigned kHoldEraseMs = 1000;   // apcKey25.h APC_HOLD_ERASE_MS
constexpr int kSampleRate = 48000;        // dsp/loop.dsp's SR -- fixed throughout aloop, no runtime config path yet
constexpr int kMaxLoopSamples = 48000 * 60;  // dsp/loop.dsp's MAXLEN -- the delay ring's hard ceiling
constexpr int kApcBtnShift = 0x62;         // apcKey25.h APC_BTN_SHIFT (98) -- channel 0 only, see onShiftPress

// --- 3-bank fx control-surface indirection (LOFI feature) -------------------
// The 8-button control row (transpose | sample | drum-sample | dub-fx |
// guitar-fx | lofi-fx | varispeed | varispeed) as confirmed in a design
// session: transpose/sample/drum-sample are the EXISTING onLiveEngageToggle/
// onSamplerBtn65/onSamplerBtn66 buttons (unchanged, no new code). dub-fx/
// guitar-fx/lofi-fx are 3 NEW, symmetric, tap-to-select bank buttons -- tap
// switches which bank the 7 physical fx knobs (CC48/49/50/51/54/55/57, plus
// CC53 formant, handled separately in onFormantCC) currently drive, radio-
// button style (no cycling, no hold-preview). Each bank stores its OWN
// independent value per knob position -- switching banks changes what the
// physical controls currently MEAN without touching other banks' stored
// values (confirmed explicitly, including the one exception: guitar bank's
// "speed" knob and dub bank's "TIME" knob share the same physical CC/slot
// but remain two independently-stored values).
enum class FxBank : uint8_t { Dub = 0, Guitar = 1, LofiFx = 2 };
constexpr int kFxBankCount = 3;
// WITNESSED BUG (live, real Pi 4): originally assigned 87/88/89, picked from
// an assumed-free note range above the microrepeat latch (82-86) WITHOUT
// ever confirming what the real APC Key25 hardware actually transmits for
// these 3 physical buttons -- the same class of mistake this codebase's own
// controls.conf comments already warn about for halfspeed/doublespeed
// (notes, not CCs, discovered only by watching real hardware). Live log
// capture (real button presses, isolated one at a time, [midi] note decoded
// lines read directly) showed the actual notes are 67/68/69, not 87/88/89 --
// the 3 assumed-unused notes were never actually reached by any physical
// press, so every bank-select press before this fix silently did nothing.
constexpr int kApcBtnDubFx    = 67;
constexpr int kApcBtnGuitarFx = 68;
constexpr int kApcBtnLofiFx   = 69;
// Per-bank Faust zone label for each of the 7 CC-mapped dub-bank knobs, so
// ApcGrid can look up "whichever bank is active's stored value" generically.
// index order matches config/controls.conf's cc48/49/50/51/54/55/57 bindings
// (fx/reverb, fx/delay, fx/time, fx/hp, fx/lpres, fx/lp, fx/pitch).
constexpr int kFxKnobCount = 7;

// row*8+col grid index -> looper index (cols 2-5) or -1 (apcKey25Notes.cpp _looperFromPad)
inline int gridLooperIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 2 || col > 5) return -1;
    int idx = row * 4 + (col - 2);
    return (idx >= 0 && idx < kLooperCount) ? idx : -1;
}
// row*8+col grid index -> preset index (cols 0-1) or -1 (apcKey25Notes.cpp _presetFromPad)
inline int gridPresetIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 0 || col > 1) return -1;
    int idx = row * 2 + col;
    return (idx >= 0 && idx < kPresetCount) ? idx : -1;
}

// Owns the tap/hold state machine for the pad grid + the 10 preset slots, and
// the continuous live-pitch (mod-wheel CC1 / CC52) state. `now_ms` is a
// monotonic millisecond clock the caller supplies (from the same clock the
// MIDI read loop uses) so this stays independent of any specific timer API.
class ApcGrid {
public:
    // Register every target name this engine can ever write, ONCE, before the
    // audio thread starts reading (called from runMidiLoop's startup, same
    // moment controls.conf's bindings are registered). ParamStore::bind()
    // takes bindMtx but setByName/get/forEach do NOT -- calling bind() from a
    // hot dispatch path (onShiftPress, onFormantCC, etc, all reachable from
    // the MIDI thread mid-flight) races the audio thread's unlocked forEach
    // over the same unordered_map. Pre-binding here and never bind()ing again
    // from onPadPress/onShiftPress/etc keeps the "bind at startup, read-only
    // after" invariant midi.h's ParamStore doc actually promises.
    static void bindAll(ParamStore& ps);


    // A pad (note 0..39, channel 0) was pressed. Writes commands into `ps`.
    // `link` (may be null): the finish-recording transition proposes the
    // just-established master phrase's tempo to the Link session (two-way
    // integration -- see applyRecPlayCycle). `audio` (may be null):
    // ARM-QUANTIZATION compensation -- reads the looper's TRUE elapsed
    // sample count (dsp/loop.dsp's writeIdx telemetry) at the finish press,
    // instead of estimating duration from wall-clock press-to-press timing
    // (which would be biased by however long the grid-tick wait took).
    void onPadPress(int note, unsigned now_ms, ParamStore& ps, class LinkBridge* link = nullptr, class AudioThread* audio = nullptr);
    // A pad (note 0..39, channel 0) was released. Writes commands into `ps`.
    void onPadRelease(int note, unsigned now_ms, ParamStore& ps, class LinkBridge* link = nullptr, class AudioThread* audio = nullptr);
    // Poll for long-holds that must fire without waiting for release (erase
    // trigger at >= kHoldEraseMs, and preset-capture at the same threshold).
    // Call once per control-thread tick (e.g. on every MIDI byte, cheap).
    void pollHolds(unsigned now_ms, ParamStore& ps);

    // Live pitch: CC1 (mod-wheel, deadzone 59-69) or CC52 (absolute 0-127).
    void onModWheel(uint8_t data2, ParamStore& ps);     // CC1
    void onAbsolutePitch(uint8_t data2, ParamStore& ps); // CC52

    // Live-pitch master engage toggle (note 0x40/64, channel 0). Previously
    // UNHANDLED entirely -- fell through to the flat controls.conf map (no
    // binding for note64 exists there either) and was silently dropped. This
    // is the button the user calls "transpose on/off" (apcKey25.cpp:97-102's
    // m_liveEngaged): a master enable gating onModWheel/onAbsolutePitch --
    // when off, live pitch stays disengaged regardless of mod-wheel/CC52
    // position (looper: !m_liveEngaged forces m_livePitchSemitones=0).
    void onLiveEngageToggle(ParamStore& ps);
    bool liveEngaged() const { return m_liveEngaged; }

    // Keybed (channel 1) note press: previously UNHANDLED entirely -- aloop's
    // midi.cpp only ever inspected channel 0, so a keybed key had NO path to
    // engage live-pitch at all. Confirmed via cross-codebase research against
    // ../looper (apcKey25.cpp:103-125): "any keybed key press unconditionally
    // sets m_liveEngaged=true and derives a semitone offset from the key" --
    // this is looper's PRIMARY way live-pitch actually gets engaged/played in
    // practice (the note-64 button is more of a manual override), so its
    // absence directly explains "keys didnt arm transpose". `sampler` (may be
    // null) gates the routing exactly as looper does: a keybed key plays
    // sampler content (chromatic pitched, or a drum one-shot if that key has
    // its own loaded slot) when the sampler has content for it, otherwise
    // falls through to live-pitch (apcKey25.cpp:110-125).
    void onKeybedNoteOn(int note, ParamStore& ps, Sampler* sampler);
    void onKeybedNoteOff(int note, Sampler* sampler);

    // Sampler record-arm buttons (channel 0): note 65 held records ONE shared
    // chromatic sample; note 66 held arms drum-record-mode (while held, each
    // keybed key records into THAT key's own drum slot). Direct port of
    // apcKey25.cpp:104-125,157-168 -- previously entirely unimplemented
    // (docs/DECISIONS.md ADR-012), now built per explicit user request.
    void onSamplerBtn65Press(Sampler* sampler);
    void onSamplerBtn65Release(Sampler* sampler);
    void onSamplerBtn66Press();
    void onSamplerBtn66Release(Sampler* sampler);
    bool drumRecordMode() const { return m_drumRecordMode; }

    // SHIFT+STOP_ALL (note 0x51/81, channel 0): looper's
    // LOOP_COMMAND_STOP_IMMEDIATE (apcKey25Notes.cpp:171) -- stops ALL
    // playback AND aborts any in-progress recording (unshifted STOP_ALL only
    // stops playback, matching audio_thread.cpp's existing cmd/stopall).
    void onStopImmediate(ParamStore& ps);

    // PLAY button (note 0x5B/91, channel 0) unshifted = CLEAR_ALL
    // (LOOP_COMMAND_CLEAR_ALL, apcKey25Notes.cpp:175). WITNESSED live: this
    // was previously routed ONLY through config/controls.conf's flat
    // note91->cmd/clearall binding, which ApcGrid never observes -- the DSP
    // (dsp/loop.dsp's `clear` button) wipes every looper's content, but
    // ApcGrid's own local shadow state (m_looperHasContent/Playing/Recording,
    // m_masterLenSamples) kept believing every looper still had content from
    // before the clear, so a fresh press after clearing went straight to the
    // pause/resume branches of applyRecPlayCycle instead of re-arming record
    // on what the DSP now considers empty loopers -- exactly the reported
    // "doing a new set didn't work" after clear. Reset all shadow state here.
    void onClearAll(bool held, ParamStore& ps);

    // Microrepeat latch notes 82-86 (channel 0 only); div in {1,2,4,8,16}, 0=off.
    void onMicrorepeatOn(int note, ParamStore& ps);
    void onMicrorepeatOff(int note, ParamStore& ps);

    // SHIFT (channel 0 note APC_BTN_SHIFT, apcKey25.h 0x62/98). Mirrors looper's
    // apcKey25.cpp:96,185 channel-0-only gating (the keybed's channel-1 note 98
    // is a different physical key and must NOT be treated as SHIFT). Held-state,
    // not a momentary trigger: onShiftPress/Release just flip m_shift and update
    // the two things it gates (CC53 formant range, monitor-fold).
    void onShiftPress(ParamStore& ps);
    void onShiftRelease(ParamStore& ps);
    bool shiftHeld() const { return m_shift; }

    // CC53 formant depth: looper apcKey25Filters.cpp:53-58 -- a deadzone around
    // center with SHIFT roughly doubling the usable range. Intercepted here
    // (not the flat controls.conf remap) because the SHIFT-dependent curve
    // can't be expressed as a static 1:1 binding.
    void onFormantCC(uint8_t data2, ParamStore& ps);

    // --- 3-bank fx control-surface (LOFI feature) ---------------------------
    // dub-fx/lofi-fx are plain tap-to-select bank buttons -- no secondary
    // gesture, unlike guitar-fx below. Switching the active bank re-pushes
    // every one of that bank's 7 stored knob values into the shared Faust
    // zones immediately (see pushBankValuesToZones's impl comment for why
    // this must happen eagerly rather than waiting for the next CC touch on
    // each knob). `now_ms` starts the brief LED flash window (see
    // bankFlashActive/pollHolds).
    void onDubFxPress(unsigned now_ms, ParamStore& ps);
    void onLofiFxPress(unsigned now_ms, ParamStore& ps);
    // guitar-fx is a dual-gesture button: a quick tap (press+release with NO
    // looper pad pressed in between) selects the guitar bank, exactly like
    // dub-fx/lofi-fx above. HOLDING guitar-fx while pressing a looper pad
    // instead toggles that looper's sidechain-pump SOURCE designation (see
    // onSidechainLooperToggle) and suppresses the bank-select tap for this
    // press, mirroring the EXISTING m_looperArmedOnPress press/release-
    // suppression pattern this file already uses for the ARM-vs-tap
    // disambiguation on the main pad grid.
    void onGuitarFxPress(unsigned now_ms, ParamStore& ps);
    void onGuitarFxRelease(ParamStore& ps);
    // Dispatch for the 7 physical fx knobs (CC48/49/50/51/54/55/57 on real
    // APC Key25 hardware, per config/controls.conf's own comment on why these
    // moved out of the flat map) -- `ccNumber` must be one of those 7; writes
    // the normalized (data2/127) value into the CURRENTLY ACTIVE bank's
    // stored slot for that knob position AND the shared Faust zone
    // immediately (so turning a knob has the usual instant effect), matching
    // the confirmed design: each bank stores its own independent value per
    // knob position, only the ACTIVE bank's write reaches the audible chain.
    // No-op if ccNumber isn't one of the 7 known knob CCs.
    void onFxKnobCC(int ccNumber, uint8_t data2, ParamStore& ps);
    FxBank activeBank() const { return m_activeBank; }
    // True only in the brief window right after a dub-fx/guitar-fx/lofi-fx
    // press, for the LED module's flash-on-select feedback (confirmed scope:
    // TRANSIENT indication during the act of selecting, not a persistent
    // always-on "which bank is active" display) -- pollHolds clears this
    // once the flash window elapses.
    bool bankFlashActive() const { return m_bankFlashReleaseAt != 0; }
    FxBank bankFlashWhich() const { return m_bankFlashWhich; }

    // Called from onPadPress when guitar-fx is currently held (see
    // onGuitarFxPress) instead of the normal ARM/FINISH pad dispatch --
    // toggles looper `looper`'s sidechain-source designation (multiple
    // simultaneous sources are allowed, confirmed in the design session).
    // Persists after guitar-fx is released (not cleared on release).
    void onSidechainLooperToggle(int looper, ParamStore& ps);
    bool looperIsSidechainSource(int looper) const { return m_looperIsSidechainSource[looper]; }
    // True while guitar-fx is held AND at least one looper press has already
    // been consumed as a source-toggle this hold -- suppresses the matching
    // bank-select tap on release, same shape as m_looperArmedOnPress.
    bool guitarFxHeldConsumedByLooper() const { return m_guitarFxConsumedByLooperPress; }

    // Read-only state accessors for the LED module (apc_leds.h) — it needs the
    // same per-pad classification ApcGrid already tracks (has this looper
    // recorded anything, is it playing, is a preset slot used) to compute
    // colors, without duplicating that state. Looper's own apcKey25.cpp reads
    // its OWN member fields directly for the same reason (LED code and input
    // dispatch share one class there); this keeps the split we already have
    // between ApcGrid (input+state) and a new ApcLeds (output) while still
    // letting ApcLeds see the state it needs.
    bool looperHasContent(int looper) const { return m_looperHasContent[looper]; }
    bool looperPlaying(int looper) const { return m_looperPlaying[looper]; }
    bool looperRecording(int looper) const { return m_looperRecording[looper]; }
    bool presetUsed(int preset) const { return m_presetUsed[preset]; }
    uint8_t microrepeatDiv() const { return m_microRepeatDiv; }

private:
    bool m_looperHeld[kLooperCount] = {};
    unsigned m_looperHoldStart[kLooperCount] = {};
    bool m_looperErased[kLooperCount] = {};      // true once the hold-erase fired this press
    unsigned m_looperEraseReleaseAt[kLooperCount] = {};  // 0 = not pending; else wall-clock ms to release looperN/erase back to 0 (see pollHolds)
    unsigned m_looperFinishReqReleaseAt[kLooperCount] = {};  // 0 = not pending; else wall-clock ms to release looperN/finishreq back to 0 (see pollHolds, same momentary-pulse pattern as erase)
    bool m_looperArmedOnPress[kLooperCount] = {}; // suppress the release tap (armed on press)
    bool m_looperPlaying[kLooperCount] = {};      // local shadow: last rec/play state we sent
    bool m_looperHasContent[kLooperCount] = {};   // local shadow: has this looper recorded anything
    bool m_looperRecording[kLooperCount] = {};    // true from arm-press until the finish-press (../looper: TRACK_STATE_RECORDING)
    unsigned m_recordStartMs[kLooperCount] = {};  // wall-clock ms at the ARM press, for computing actual recorded duration at finish

    bool m_presetHeld[kPresetCount] = {};
    unsigned m_presetHoldStart[kPresetCount] = {};
    bool m_presetCaptured[kPresetCount] = {};
    bool m_presetUsed[kPresetCount] = {};
    uint32_t m_presetMask[kPresetCount] = {};

    uint8_t m_microRepeatDiv = 0;
    bool m_shift = false;
    bool m_liveEngaged = false;   // master toggle for live pitch (note 64), apcKey25.cpp m_liveEngaged
    bool m_drumRecordMode = false;   // note 66 held (apcKey25.cpp m_drumRecordMode)
    // Local master-phrase length in samples, established from the FIRST
    // looper's own recorded duration (../looper loopClip.cpp:219-244:
    // "ALWAYS defines the local master grid from its own recorded length,
    // Link-synced or not") -- independent of any Ableton Link session, unlike
    // audio_thread.cpp's Link-only length source. Published to ParamStore's
    // "cmd/master_len" target so the audio thread can feed it into microrepeat's
    // MLB zone when Link is not synced (previously: MLB was hard-forced to 0.0
    // -- microrepeat entirely inert -- whenever no Link session was connected,
    // even though looper's real hardware needs no Link at all for glitch/microrepeat).
    // 0 = no master phrase established yet (mirrors looper's masterLoopBlocks==0).
    long m_masterLenSamples = 0;

    // --- 3-bank fx control-surface state (LOFI feature) ---------------------
    // Per-bank stored value for each of the 7 CC-mapped knobs -- [bank][knob].
    // Defaults match the EXISTING dub-bank Faust zone defaults exactly (see
    // midi.cpp's kFxDefaults table) for bank 0 (Dub); guitar/lofi-fx banks
    // default every knob to whatever value makes THAT bank's own effect chain
    // a byte-exact passthrough (0.0 for every new effect's "amount" control,
    // per the confirmed guitar-fx-bank-defaults-passthrough / lofi-fx-bank-
    // defaults-passthrough design requirement) -- set once here, re-verified
    // against the real Faust defaults once effects_runtime.dsp's new stages
    // land (a later integration step reads these same array indices).
    float m_fxBankValues[kFxBankCount][kFxKnobCount] = {
        // Dub bank: fx/reverb, fx/delay, fx/time, fx/hp, fx/lpres, fx/lp, fx/pitch
        {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f},
        // Guitar bank: flange, tremolo, speed, phaser, attack, release, (compress lives outside this 7-knob array, see below)
        {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f},
        // Lofi-fx bank: bitcrush, vinyl, flutter, samplerate, (4 granulator knobs live in sampler.h, not here)
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    };
    FxBank m_activeBank = FxBank::Dub;
    unsigned m_bankFlashReleaseAt = 0;   // 0 = no flash pending; else wall-clock ms to clear bankFlashActive() (see pollHolds)
    FxBank m_bankFlashWhich = FxBank::Dub;
    static constexpr unsigned kBankFlashMs = 150;   // brief, per the confirmed "flash only during selection" scope

    // Sidechain-pump (guitar-fx hold + looper press): multiple simultaneous
    // sources allowed, persists after guitar-fx release, auto-clears on
    // erase/clear (all confirmed in the design session).
    bool m_guitarFxHeld = false;
    bool m_guitarFxConsumedByLooperPress = false;
    bool m_looperIsSidechainSource[kLooperCount] = {};

    void applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps, class LinkBridge* link, class AudioThread* audio = nullptr);
    void capturePreset(int p, ParamStore& ps);
    void applyPreset(int p, ParamStore& ps);
    void forgetLooperFromPresets(int looper);
    // Re-pushes every stored knob value of `bank` into the shared Faust
    // zones (fx/reverb, fx/delay, ...) -- called on every bank switch so the
    // audible effect jumps to the newly-selected bank's last-left values
    // immediately, not only once each knob is next physically touched.
    void pushBankValuesToZones(FxBank bank, ParamStore& ps);
};

} // namespace aloop
#endif // ALOOP_APC_GRID_H
