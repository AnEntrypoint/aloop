// aloop APC Key25 grid engine — ported from looper's apcKey25Notes.cpp state
// machine (grid math, tap-vs-hold, presets) onto aloop's ParamStore.
//
// aloop has NO overdub and NO addressable read head (docs/COMMAND-SURFACE.md),
// so the per-looper state cycle is simplified from looper's 4-state machine
// (empty->record, recording->play, playing->pause, paused->play) to the two
// zones aloop's Faust engine actually exposes: `rec` (button, replaces the
// loop) and `play` (checkbox, gates output). This module tracks a local
// shadow of "has content" / "is playing" (written whenever WE send a command)
// so tap can still cycle rec->play->pause->play without reading back audio
// state cross-thread — the same shape looper's publicTrack read serves, but
// sourced locally since aloop's control thread doesn't have a state channel
// into the audio thread's Faust zones.

#include "apc_grid.h"
#include <cstdio>
#include <cstring>

namespace aloop {

static void setLooper(ParamStore& ps, int looper, const char* field, float v) {
    char name[32];
    snprintf(name, sizeof name, "looper%d/%s", looper, field);
    ps.bind(name);
    ps.setByName(name, v);
}

void ApcGrid::applyRecPlayCycle(int looper, ParamStore& ps) {
    // Mirrors apcKey25Notes.cpp's press-cycle, collapsed onto aloop's two zones
    // (rec = one-shot "replace the loop", play = held gate):
    //   no content            -> rec (arm/replace), mark has-content, playing
    //   has content, playing  -> play=0 (pause)
    //   has content, paused   -> play=1 (resume)
    if (!m_looperHasContent[looper]) {
        setLooper(ps, looper, "rec", 1.0f);
        // rec is a momentary button on real hardware/Faust (button() auto-releases
        // per block); the "held" value only needs to reach the audio thread once.
        m_looperHasContent[looper] = true;
        m_looperPlaying[looper] = true;
        setLooper(ps, looper, "play", 1.0f);
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
        // pad arms record on PRESS (not release) so the take starts on the exact
        // press instant, matching looper's "catch the moment record is pressed"
        // rationale. All other transitions (pause/resume) stay on release.
        if (!m_looperHasContent[looper]) {
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
    bool inDeadzone = (data2 >= 59 && data2 <= 69);
    ps.bind("fx/pitchbend"); ps.bind("fx/pitchbend_engaged");
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
    float semis = (data2 / 127.0f) * 24.0f - 12.0f;
    ps.bind("fx/pitchbend"); ps.bind("fx/pitchbend_engaged");
    ps.setByName("fx/pitchbend", semis);
    ps.setByName("fx/pitchbend_engaged", 1.0f);
}

// --- microrepeat latch (notes 82-86) — apcKey25.cpp -------------------------
void ApcGrid::onMicrorepeatOn(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    m_microRepeatDiv = div[note - 82];
    ps.bind("fx/microrepeat_div");
    ps.setByName("fx/microrepeat_div", (float)m_microRepeatDiv);
}
void ApcGrid::onMicrorepeatOff(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    // Only clear if the released note owns the active division (apcKey25.cpp:
    // a stale earlier note release must not cancel a newer held one).
    if (m_microRepeatDiv == div[note - 82]) {
        m_microRepeatDiv = 0;
        ps.bind("fx/microrepeat_div");
        ps.setByName("fx/microrepeat_div", 0.0f);
    }
}

} // namespace aloop
