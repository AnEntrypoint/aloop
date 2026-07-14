// aloop APC Key25 grid engine — ported from looper's apcKey25Notes.cpp state
// machine (grid math, tap-vs-hold, presets) onto aloop's ParamStore.
//
// Per-looper press cycle mirrors looper's real state machine exactly
// (apcKey25Notes.cpp:43-84 _onPadPress): empty -> ARM (rec=1, held for the
// whole recording pass) -> FINISH (rec=0, play=1, starts playback of what was
// just captured) -> pause (play=0) -> resume (play=1) -> ... WITNESSED BUG
// this replaced: an earlier version set rec=1 AND play=1 in the SAME press,
// with nothing ever resetting rec back to 0 anywhere in the codebase — since
// `rec` is a persistent ParamStore value (not a momentary Faust-native
// button() the widget itself releases), it stayed 1 forever, meaning the
// Faust looper (dsp/loop.dsp: record = in*recN) NEVER stopped re-recording
// live input over the loop on every single block, so nothing ever actually
// played back a fixed take — the user's reported "loops don't play, they
// just stay paused" (recording forever look identical to "not playing" from
// the outside). This module tracks a local shadow of record/content/playing
// state (written whenever WE send a command) so tap can cycle correctly
// without reading back audio state cross-thread — the same shape looper's
// publicTrack read serves, but sourced locally since aloop's control thread
// doesn't have a state channel into the audio thread's Faust zones.

#include "apc_grid.h"
#include <cstdio>
#include <cstring>

namespace aloop {

void ApcGrid::bindAll(ParamStore& ps) {
    char name[32];
    for (int looper = 0; looper < kLooperCount; looper++) {
        for (const char* field : {"rec", "play", "erase"}) {
            snprintf(name, sizeof name, "looper%d/%s", looper, field);
            ps.bind(name);
        }
    }
    ps.bind("fx/pitchbend");
    ps.bind("fx/pitchbend_engaged");
    ps.bind("fx/microrepeat_div");
    ps.bind("fx/monitorfold");
    ps.bind("fx/formant");
}

static void setLooper(ParamStore& ps, int looper, const char* field, float v) {
    char name[32];
    snprintf(name, sizeof name, "looper%d/%s", looper, field);
    ps.setByName(name, v);
}

void ApcGrid::applyRecPlayCycle(int looper, ParamStore& ps) {
    // Real 3-state cycle (apcKey25Notes.cpp:61-84):
    //   empty                 -> ARM: rec=1 (HELD -- stays 1 for the whole
    //                             recording pass, this press only starts it)
    //   currently recording    -> FINISH: rec=0 (stop overwriting the loop,
    //                             matching looper's TRACK cmd), play=1 (start
    //                             playback of what was just captured)
    //   has content, playing  -> play=0 (pause)
    //   has content, paused   -> play=1 (resume)
    if (m_looperRecording[looper]) {
        setLooper(ps, looper, "rec", 0.0f);   // FINISH: stop recording (was stuck at 1 forever pre-fix)
        m_looperRecording[looper] = false;
        m_looperHasContent[looper] = true;
        m_looperPlaying[looper] = true;
        setLooper(ps, looper, "play", 1.0f);
    } else if (!m_looperHasContent[looper]) {
        setLooper(ps, looper, "rec", 1.0f);   // ARM: start recording -- next press finishes it
        m_looperRecording[looper] = true;
    } else if (m_looperPlaying[looper]) {
        setLooper(ps, looper, "play", 0.0f);
        m_looperPlaying[looper] = false;
    } else {
        setLooper(ps, looper, "play", 1.0f);
        m_looperPlaying[looper] = true;
    }
}

void ApcGrid::forgetLooperFromPresets(int looper) {
    // apcKey25Notes.cpp _forgetLooperFromPresets: drop this looper's bit from
    // every stored preset; a preset that becomes empty is deleted.
    uint32_t bit = (1u << looper);
    for (int p = 0; p < kPresetCount; p++) {
        if (!m_presetUsed[p]) continue;
        if (!(m_presetMask[p] & bit)) continue;
        m_presetMask[p] &= ~bit;
        if (m_presetMask[p] == 0) m_presetUsed[p] = false;
    }
}

void ApcGrid::onPadPress(int note, unsigned now_ms, ParamStore& ps) {
    int row = note / kApcCols, col = note % kApcCols;

    int looper = gridLooperIndex(row, col);
    if (looper >= 0) {
        m_looperHeld[looper] = true;
        m_looperErased[looper] = false;
        m_looperHoldStart[looper] = now_ms;
        // Press-time-critical arm/finish, mirroring apcKey25Notes.cpp: an empty
        // pad arms record on PRESS (not release), and a CURRENTLY RECORDING pad
        // finishes recording on PRESS too (both are "the exact press instant
        // must land precisely" cases per looper's comments -- release-triggered
        // dispatch would add the press-hold duration as timing jitter to either
        // the start or the end of the take). All other transitions (pause/
        // resume) stay on release.
        if (!m_looperHasContent[looper] || m_looperRecording[looper]) {
            applyRecPlayCycle(looper, ps);
            m_looperArmedOnPress[looper] = true;
        } else {
            m_looperArmedOnPress[looper] = false;
        }
        return;
    }
    int preset = gridPresetIndex(row, col);
    if (preset >= 0) {
        m_presetHeld[preset] = true;
        m_presetCaptured[preset] = false;
        m_presetHoldStart[preset] = now_ms;
        return;
    }
}

void ApcGrid::onPadRelease(int note, unsigned /*now_ms*/, ParamStore& ps) {
    int row = note / kApcCols, col = note % kApcCols;

    int looper = gridLooperIndex(row, col);
    if (looper >= 0) {
        if (m_looperArmedOnPress[looper]) {
            // Already armed on press for an empty pad — don't double-fire the tap.
            m_looperArmedOnPress[looper] = false;
            m_looperHeld[looper] = false;
            return;
        }
        if (m_looperHeld[looper] && !m_looperErased[looper]) {
            applyRecPlayCycle(looper, ps);
        }
        m_looperHeld[looper] = false;
        return;
    }
    int preset = gridPresetIndex(row, col);
    if (preset >= 0) {
        if (m_presetHeld[preset] && !m_presetCaptured[preset]) {
            // Short-tap -> restore if this preset has been captured (apcKey25Notes.cpp).
            if (m_presetUsed[preset]) applyPreset(preset, ps);
        }
        m_presetHeld[preset] = false;
        return;
    }
}

void ApcGrid::pollHolds(unsigned now_ms, ParamStore& ps) {
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (!m_looperHeld[looper] || m_looperErased[looper]) continue;
        if (now_ms - m_looperHoldStart[looper] < kHoldEraseMs) continue;
        // Long-hold -> erase (apcKey25Notes.cpp: a >=1s hold clears the looper
        // regardless of state; also cancels a just-armed press-record).
        setLooper(ps, looper, "erase", 1.0f);
        if (m_looperRecording[looper]) {
            setLooper(ps, looper, "rec", 0.0f);   // cancel the in-progress take, don't leave rec stuck at 1
            m_looperRecording[looper] = false;
        }
        m_looperErased[looper] = true;
        m_looperArmedOnPress[looper] = false;
        m_looperHasContent[looper] = false;
        m_looperPlaying[looper] = false;
        forgetLooperFromPresets(looper);
    }
    for (int p = 0; p < kPresetCount; p++) {
        if (!m_presetHeld[p] || m_presetCaptured[p]) continue;
        if (now_ms - m_presetHoldStart[p] < kHoldEraseMs) continue;
        // Long-hold -> capture (apcKey25Notes.cpp: snapshot the currently-playing
        // set into this preset slot).
        capturePreset(p, ps);
        m_presetCaptured[p] = true;
    }
}

void ApcGrid::capturePreset(int p, ParamStore& /*ps*/) {
    if (p < 0 || p >= kPresetCount) return;
    uint32_t mask = 0;
    for (int n = 0; n < kLooperCount; n++)
        if (m_looperHasContent[n] && m_looperPlaying[n]) mask |= (1u << n);
    m_presetMask[p] = mask;
    m_presetUsed[p] = true;
}

void ApcGrid::applyPreset(int p, ParamStore& ps) {
    if (p < 0 || p >= kPresetCount || !m_presetUsed[p]) return;
    uint32_t mask = m_presetMask[p];
    for (int n = 0; n < kLooperCount; n++) {
        if (!m_looperHasContent[n]) continue;   // empty looper, skip (apcKey25Notes.cpp)
        bool shouldPlay = (mask & (1u << n)) != 0;
        if (shouldPlay != m_looperPlaying[n]) {
            setLooper(ps, n, "play", shouldPlay ? 1.0f : 0.0f);
            m_looperPlaying[n] = shouldPlay;
        }
    }
}

// --- live pitch (CC1 mod-wheel deadzone, CC52 absolute) — apcKey25.cpp -----
// Targets `fx/pitchbend` (semitones, -12..12) distinct from the static
// `fx/pitch` knob (config/controls.conf's SEMIS zone) since this is a
// continuous performance control, not a settable effect param. Wire into
// dsp/aloop.dsp's SEMIS/ENGAGED zones via the control map (fx/pitchbend ->
// pitchStage's live-offset input) — see PRD row wiring notes.
void ApcGrid::onModWheel(uint8_t data2, ParamStore& ps) {
    if (!m_liveEngaged) { ps.setByName("fx/pitchbend_engaged", 0.0f); ps.setByName("fx/pitchbend", 0.0f); return; }
    bool inDeadzone = (data2 >= 59 && data2 <= 69);
    if (inDeadzone) {
        ps.setByName("fx/pitchbend_engaged", 0.0f);
        ps.setByName("fx/pitchbend", 0.0f);
    } else {
        float semis = ((float)((int)data2 - 64)) * 12.0f / 63.0f;
        ps.setByName("fx/pitchbend", semis);
        ps.setByName("fx/pitchbend_engaged", 1.0f);
    }
}
void ApcGrid::onAbsolutePitch(uint8_t data2, ParamStore& ps) {
    if (!m_liveEngaged) { ps.setByName("fx/pitchbend_engaged", 0.0f); ps.setByName("fx/pitchbend", 0.0f); return; }
    float semis = (data2 / 127.0f) * 24.0f - 12.0f;
    ps.setByName("fx/pitchbend", semis);
    ps.setByName("fx/pitchbend_engaged", 1.0f);
}
void ApcGrid::onLiveEngageToggle(ParamStore& ps) {
    // apcKey25.cpp:97-102: toggle, and disengaging immediately zeros the
    // pitch offset rather than waiting for the next mod-wheel/CC52 event.
    m_liveEngaged = !m_liveEngaged;
    if (!m_liveEngaged) {
        ps.setByName("fx/pitchbend", 0.0f);
        ps.setByName("fx/pitchbend_engaged", 0.0f);
    }
}

// --- microrepeat latch (notes 82-86) — apcKey25.cpp -------------------------
void ApcGrid::onMicrorepeatOn(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    m_microRepeatDiv = div[note - 82];
    ps.setByName("fx/microrepeat_div", (float)m_microRepeatDiv);
}
void ApcGrid::onMicrorepeatOff(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    // Only clear if the released note owns the active division (apcKey25.cpp:
    // a stale earlier note release must not cancel a newer held one).
    if (m_microRepeatDiv == div[note - 82]) {
        m_microRepeatDiv = 0;
        ps.setByName("fx/microrepeat_div", 0.0f);
    }
}

// --- SHIFT (apcKey25.cpp:96,185) --------------------------------------------
// Held state, not a trigger: gates CC53 formant range (onFormantCC) and the
// monitor-fold routing (fx/monitorfold, applied immediately on press/release
// exactly as looper's p.monitorMode = m_shift is read every block).
void ApcGrid::onShiftPress(ParamStore& ps) {
    m_shift = true;
    ps.setByName("fx/monitorfold", 1.0f);
}
void ApcGrid::onShiftRelease(ParamStore& ps) {
    m_shift = false;
    ps.setByName("fx/monitorfold", 0.0f);
}

// --- CC53 formant depth (apcKey25Filters.cpp:53-58) -------------------------
// Deadzone around center (63/64, matching the mod-wheel deadzone convention
// used elsewhere on this controller). effects_runtime.dsp's FORMANT hslider is
// declared -3..3 (audio_thread.cpp targetToZone maps fx/formant straight onto
// that zone, no rescale) so the achievable range IS ±3, not the raw CC's
// theoretical ±1 normalized value scaled arbitrarily: unshifted reaches half
// that (±1.5), SHIFT held reaches the full ±3 -- doubling the usable range
// within the zone's own declared bounds, matching looper's SHIFT-expands
// behavior without exceeding what FORMANT can actually represent.
void ApcGrid::onFormantCC(uint8_t data2, ParamStore& ps) {
    const bool inDeadzone = (data2 >= 62 && data2 <= 65);
    if (inDeadzone) { ps.setByName("fx/formant", 0.0f); return; }
    const float norm = ((float)(int)data2 - 63.5f) / 63.5f;   // -1..1
    const float maxDepth = m_shift ? 3.0f : 1.5f;              // SHIFT doubles the usable range within FORMANT's -3..3
    float v = norm * maxDepth;
    if (v > 3.0f) v = 3.0f; else if (v < -3.0f) v = -3.0f;
    ps.setByName("fx/formant", v);
}

} // namespace aloop
