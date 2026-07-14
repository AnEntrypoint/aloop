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
        // "len" bound separately with dsp/loop.dsp's actual hslider default
        // (48000 = 1 second) -- matches the zero-default bug class fixed
        // elsewhere (ParamStore::bind's own doc comment): any bound target
        // whose Faust zone has a non-zero default must be seeded correctly,
        // or it gets silently forced to 0 (here: a 0-length loop) every
        // block from process start until applyRecPlayCycle's finish-press
        // sets a real value.
        snprintf(name, sizeof name, "looper%d/len", looper);
        ps.bind(name, 48000.0f);
    }
    ps.bind("fx/pitchbend");
    ps.bind("fx/pitchbend_engaged");
    ps.bind("fx/microrepeat_div");
    ps.bind("fx/monitorfold");
    ps.bind("fx/formant");
    ps.bind("cmd/master_len", 0.0f);   // local master-phrase length (samples), 0 = none established yet
}

static void setLooper(ParamStore& ps, int looper, const char* field, float v) {
    char name[32];
    snprintf(name, sizeof name, "looper%d/%s", looper, field);
    ps.setByName(name, v);
}

void ApcGrid::applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps) {
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
        // WITNESSED live + confirmed by cross-codebase research against
        // ../looper (loopClip.cpp:64-66,219,243): looper's masterLoopBlocks
        // (the shared phrase length ALL loopers quantize to) is established
        // from the FIRST recorded clip's own actual duration -- "ALWAYS
        // defines the local master grid from its own recorded length,
        // Link-synced or not." aloop had NO local equivalent: dsp/loop.dsp's
        // de.fdelay ring length (`len`) was previously driven ONLY by
        // audio_thread.cpp's Link-synced branch, meaning any standalone
        // (no Link session) recording left `len` frozen at its Faust-compiled
        // default (48000 = 1 second) regardless of actual record duration --
        // directly causing "loop didn't set phrase" / "didn't play the whole
        // recorded loop" (a 4s take truncated/looped at the stale 1s ring
        // length). Fixed: the FIRST looper to finish recording on a clear rig
        // establishes m_masterLenSamples from its own actual press-to-press
        // duration (mirroring loopClip.cpp's masterLoopBlocks==0 seed check);
        // every subsequent looper (recorded or not yet recorded) is set to
        // that SAME shared length, matching looper's single shared phrase
        // rather than each looper independently deriving its own duration.
        // audio_thread.cpp's Link-synced branch still overrides/retunes this
        // every block once a real Link session connects (mirroring looper's
        // separate anyRecorded-gated Link rescale path) -- this is purely the
        // standalone (no-Link) phrase-establishment fallback.
        // Re-sync from the shared store first: audio_thread.cpp resets
        // cmd/master_len to 0 on CLEAR_ALL (a command this thread doesn't
        // otherwise observe), and a live Link session may also be actively
        // retuning looper*/len independent of this thread -- reading back
        // the authoritative shared value (rather than trusting only this
        // thread's own last-written shadow) keeps both in agreement.
        m_masterLenSamples = (long)ps.get("cmd/master_len", 0.0f);
        if (m_masterLenSamples == 0) {
            unsigned elapsedMs = now_ms - m_recordStartMs[looper];
            long lenSamples = (long)elapsedMs * kSampleRate / 1000;
            if (lenSamples < 64) lenSamples = 64;                     // dsp/loop.dsp's hslider min
            if (lenSamples > kMaxLoopSamples) lenSamples = kMaxLoopSamples;
            m_masterLenSamples = lenSamples;
            ps.setByName("cmd/master_len", (float)m_masterLenSamples);
            // Propagate the newly-established phrase to every looper (this
            // one included) so a second, third, ... looper recorded next
            // joins the SAME shared grid, matching looper's single
            // masterLoopBlocks used by the whole rig, not a per-looper value.
            for (int lp = 0; lp < kLooperCount; lp++) setLooper(ps, lp, "len", (float)m_masterLenSamples);
        } else {
            setLooper(ps, looper, "len", (float)m_masterLenSamples);
        }
    } else if (!m_looperHasContent[looper]) {
        setLooper(ps, looper, "rec", 1.0f);   // ARM: start recording -- next press finishes it
        m_looperRecording[looper] = true;
        m_recordStartMs[looper] = now_ms;
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
            applyRecPlayCycle(looper, now_ms, ps);
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

void ApcGrid::onPadRelease(int note, unsigned now_ms, ParamStore& ps) {
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
            applyRecPlayCycle(looper, now_ms, ps);
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
void ApcGrid::onStopImmediate(ParamStore& ps) {
    // apcKey25Notes.cpp:171's LOOP_COMMAND_STOP_IMMEDIATE: stop every
    // playing looper AND abort any looper still mid-recording (unlike plain
    // cmd/stopall, which only zeroes `play` and leaves an active `rec` alone
    // -- see audio_thread.cpp's cmd/stopall handling).
    for (int lp = 0; lp < kLooperCount; lp++) {
        if (m_looperRecording[lp]) {
            setLooper(ps, lp, "rec", 0.0f);
            m_looperRecording[lp] = false;
            // aborted before any content was captured -- this looper stays
            // empty, matching looper's abort semantics (not "has content").
        }
        setLooper(ps, lp, "play", 0.0f);
        m_looperPlaying[lp] = false;
    }
}
void ApcGrid::onKeybedNoteOn(int note, ParamStore& ps) {
    // apcKey25.cpp:122-123: any keybed key press engages live-pitch at that
    // key's own semitone offset from middle C-ish (note 60), unconditionally.
    m_liveEngaged = true;
    float semis = (float)(note - 60);
    ps.setByName("fx/pitchbend", semis);
    ps.setByName("fx/pitchbend_engaged", 1.0f);
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

// --- CC53 formant depth (apcKey25Filters.cpp:59-71) -------------------------
// WITNESSED via direct cross-codebase comparison against ../looper's real
// values: the prior aloop constants were both off from looper's actual
// numbers -- deadzone was 62-65 here vs looper's real 60-68, and the
// unshifted range was 1.5 here vs looper's real 1.0 (looper: "Default range
// ±1 (musical territory)... Hold SHIFT... to expand to ±3"). Corrected to
// match exactly: deadzone 60-68, range ±1 unshifted / ±3 shifted, and the
// normalization formula itself now matches looper's `((data2-64)/63.0)*range`
// (not aloop's previous /63.5 centered-differently variant).
void ApcGrid::onFormantCC(uint8_t data2, ParamStore& ps) {
    const bool inDeadzone = (data2 >= 60 && data2 <= 68);
    if (inDeadzone) { ps.setByName("fx/formant", 0.0f); return; }
    const float range = m_shift ? 3.0f : 1.0f;
    float v = (((float)(int)data2 - 64.0f) / 63.0f) * range;
    if (v > 3.0f) v = 3.0f; else if (v < -3.0f) v = -3.0f;
    ps.setByName("fx/formant", v);
}

} // namespace aloop
