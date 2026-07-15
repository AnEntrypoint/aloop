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
#include "../dsp/sampler/sampler.h"
#include "../link/link_bridge.h"
#include <cstdio>
#include <cstring>
#include <cmath>

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
    ps.bind("cmd/recorded_bpm", 0.0f); // TRUE varispeed: BPM the shared phrase was recorded at, 0 = none established yet
}

static void setLooper(ParamStore& ps, int looper, const char* field, float v) {
    char name[32];
    snprintf(name, sizeof name, "looper%d/%s", looper, field);
    ps.setByName(name, v);
}

// Derive a tempo (BPM) from a recorded phrase's actual duration, choosing the
// number of beats-per-phrase that lands the result NEAREST 120 BPM -- mirrors
// looper's own (never-fully-wired) intended design, documented in its PRD
// history: "bpm = 60*beats_in_clip / clip_seconds where beats chosen to land
// tempo nearest 120." A 4-beat bar is tried first (the common case, matching
// audio_thread.cpp's own beatsPerBar=4.0 assumption for Link-driven length),
// but other small beat-counts (1,2,8,16) are also tried and whichever gets
// closest to 120 BPM wins -- so a very short "one hit" loop doesn't get
// forced into an absurd 4-beat interpretation (e.g. 500ms/4beats = 480bpm)
// when interpreting it as a single beat (500ms/1beat = 120bpm exactly) is
// obviously the musically-sensible reading.
static double deriveTempoBpm(double seconds) {
    if (seconds <= 0.0) return 120.0;
    static const int kCandidates[] = {1, 2, 4, 8, 16};
    double bestBpm = 120.0;
    double bestDist = 1e18;
    for (int beats : kCandidates) {
        double bpm = 60.0 * beats / seconds;
        double dist = std::fabs(bpm - 120.0);
        if (dist < bestDist) { bestDist = dist; bestBpm = bpm; }
    }
    return bestBpm;
}

void ApcGrid::applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps, LinkBridge* link) {
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
        // TEMPORARY diagnostic (tracked for removal): see ARM's matching diag7.
        fprintf(stderr, "[diag7] FINISH looper=%d now_ms=%u recordStart=%u elapsedMs=%u masterLen(before)=%ld erase_zone=%.2f\n",
                looper, now_ms, m_recordStartMs[looper], now_ms - m_recordStartMs[looper], m_masterLenSamples,
                ps.get("looper" + std::to_string(looper) + "/erase", -1.0f));
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
            // WITNESSED live: "the new recording doesn't appear to have
            // audio" / "recording the wrong part of the input buffer" on
            // the FIRST post-clear recording specifically, while a SECOND
            // recording (which reuses the already-established
            // m_masterLenSamples unchanged, see the `else` branch below)
            // works fine. Root cause: `now_ms` (this control thread's wall
            // clock, captured at the ARM press and again at the FINISH
            // press) is NOT sample-accurate relative to the audio thread's
            // actual per-block timeline -- there is real, if small,
            // MIDI-event-to-audio-thread latency (control-thread scheduling,
            // block-buffering) that the FIRST recording's freshly-computed
            // length has no margin against. dsp/loop.dsp's de.fdelay ring
            // taps `effLen` samples BACK from "now" once play starts; if the
            // computed length is even slightly LONGER than what was actually
            // written since the ring was zeroed by the preceding clear, that
            // tap reads into the pre-clear silence for part or all of the
            // loop -- exactly matching "recording the wrong part of the
            // input buffer". A SECOND recording never hits this because it
            // reuses the SAME already-verified-working length rather than
            // computing a fresh one with its own fresh skew. Fix: shrink the
            // freshly-computed length by a safety margin (10ms) so the tap
            // can never reach further back than genuinely-recorded content,
            // erring toward a very slightly short loop rather than one that
            // reads stale/silent content at its start.
            const long kSafetyMarginSamples = kSampleRate / 100;   // 10ms
            lenSamples -= kSafetyMarginSamples;
            if (lenSamples < 64) lenSamples = 64;                     // dsp/loop.dsp's hslider min
            if (lenSamples > kMaxLoopSamples) lenSamples = kMaxLoopSamples;
            m_masterLenSamples = lenSamples;
            ps.setByName("cmd/master_len", (float)m_masterLenSamples);
            // TRUE varispeed: lock in the BPM this shared phrase was
            // established at, exactly at the same moment cmd/master_len
            // itself is first established -- mirrors looper's
            // m_nativeBlocks being set ONCE at _finishRecording and never
            // changing while the clip lives (Looper.h:317's field comment,
            // loopClip.cpp:268-271). audio_thread.cpp's "Varispeed Link
            // sync" block reads this back as cmd/recorded_bpm to compute
            // linkSpeedRatio = recordedBpm/currentLinkBpm every block a
            // Link session is actively driving length, so an
            // already-recorded loop stays pitch-locked to the tempo it was
            // captured at even as Link's tempo moves. aloop shares ONE
            // master phrase across the whole rig (unlike looper's per-clip
            // m_nativeBlocks), so this is a single shared value, consistent
            // with cmd/master_len's own existing shared-rig design.
            double recordedSeconds = (double)m_masterLenSamples / (double)kSampleRate;
            ps.setByName("cmd/recorded_bpm", (float)deriveTempoBpm(recordedSeconds));
            // Propagate the newly-established phrase to every looper (this
            // one included) so a second, third, ... looper recorded next
            // joins the SAME shared grid, matching looper's single
            // masterLoopBlocks used by the whole rig, not a per-looper value.
            for (int lp = 0; lp < kLooperCount; lp++) setLooper(ps, lp, "len", (float)m_masterLenSamples);
            // TWO-WAY LINK INTEGRATION: the first recorded loop PROPOSES its
            // own tempo to the Link session, so Link becomes the shared
            // tempo authority for the whole group from this point on --
            // previously aloop only ever READ Link's tempo (LinkBridge::
            // proposeTempo existed but was never called from anywhere,
            // genuinely dead code). Mirrors looper's own intended-but-
            // never-fully-wired design (linkSetBPM, PRD history: "bpm =
            // 60*beats_in_clip/clip_seconds"). Once proposed, Link's own
            // tempo becomes authoritative going forward: audio_thread.cpp's
            // Link-synced branch will pick up this exact BPM on its very
            // next read and start retiming everything (including THIS
            // looper) from Link rather than the local cmd/master_len
            // fallback -- a real closed loop, not a one-shot announcement.
            if (link) {
                double seconds = (double)m_masterLenSamples / (double)kSampleRate;
                link->proposeTempo(deriveTempoBpm(seconds));
            }
        } else {
            // WITNESSED live: "consecutive loops were all the same lengths
            // not multiples and divisions like ../looper" -- this branch
            // previously forced EVERY subsequent looper to the exact master
            // length, ignoring how long the user actually held record for.
            // looper's real design (loopClipState.cpp's _calcQuantizeTarget)
            // rounds a subsequent recording's raw duration to the NEAREST
            // musical subdivision/multiple of the established phrase (M/8,
            // M/4, M/2, M, 2M, 4M, ...), so a deliberately-short or
            // deliberately-long take becomes a clean fraction/multiple
            // rather than always collapsing to exactly M. Mirror that here:
            // measure this looper's own elapsed record duration, then snap
            // it to the closest of {M/4, M/2, M, 2M, 4M} (an 8-value set
            // would need M/8 to make sense at very short M; this covers the
            // common musical cases without over-fitting a formula looper's
            // own C++ never fully specified in a way this session could
            // verify exactly).
            unsigned elapsedMs = now_ms - m_recordStartMs[looper];
            long rawSamples = (long)elapsedMs * kSampleRate / 1000;
            static const double kMultiples[] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
            double bestLen = (double)m_masterLenSamples;
            double bestDist = 1e18;
            for (double mult : kMultiples) {
                double candidate = (double)m_masterLenSamples * mult;
                double dist = std::fabs((double)rawSamples - candidate);
                if (dist < bestDist) { bestDist = dist; bestLen = candidate; }
            }
            long quantized = (long)(bestLen + 0.5);
            if (quantized < 64) quantized = 64;
            if (quantized > kMaxLoopSamples) quantized = kMaxLoopSamples;
            setLooper(ps, looper, "len", (float)quantized);
        }
    } else if (!m_looperHasContent[looper]) {
        setLooper(ps, looper, "rec", 1.0f);   // ARM: start recording -- next press finishes it
        m_looperRecording[looper] = true;
        m_recordStartMs[looper] = now_ms;
        // TEMPORARY diagnostic (tracked for removal): re-investigating "the
        // second time we try its still empty" after the master-phrase-reset
        // fix (67c10a8) -- checking whether a pending per-looper erase
        // release (m_looperEraseReleaseAt) is still active when a fresh ARM
        // fires, which could mean the erase gate (wipe=1) is still held
        // during part of this new recording's window.
        fprintf(stderr, "[diag7] ARM looper=%d now_ms=%u eraseReleaseAt=%u (pending=%d) masterLen=%ld cmd_masterlen=%.0f erase_zone=%.2f\n",
                looper, now_ms, m_looperEraseReleaseAt[looper], (int)(m_looperEraseReleaseAt[looper] != 0),
                m_masterLenSamples, ps.get("cmd/master_len", -1.0f), ps.get("looper" + std::to_string(looper) + "/erase", -1.0f));
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

void ApcGrid::onPadPress(int note, unsigned now_ms, ParamStore& ps, LinkBridge* link) {
    int row = note / kApcCols, col = note % kApcCols;

    int looper = gridLooperIndex(row, col);
    if (looper >= 0) {
        // REPEATED-NOTE-ON GUARD: real APC Key25 hardware (unlike the
        // synthetic midi-inject.js press/release test path used earlier this
        // session) re-sends note-on for a pad that is physically still held
        // down -- WITNESSED as a real, distinct MIDI byte sequence class this
        // session was asked to check for but had not yet verified live
        // (device unreachable at investigation time; this is a structural
        // fix applied defensively from first-principles MIDI behavior, to be
        // confirmed against a live raw-byte capture once the device is back
        // on the network). Before this guard, EVERY repeated note-on for an
        // already-held pad unconditionally reset m_looperHoldStart[looper]
        // to "now" (below) -- so a >=1s continuous physical hold whose
        // hardware re-sends note-on faster than kHoldEraseMs (1000ms) apart
        // could NEVER accumulate enough elapsed time to fire the erase
        // long-hold, and, far worse, a repeat arriving while
        // m_looperRecording[looper] is true re-entered the
        // `!hasContent || recording` press-dispatch condition below and
        // fired applyRecPlayCycle a SECOND time mid-recording -- prematurely
        // finishing the take after only the repeat interval's worth of
        // audio (a fraction of a second) instead of the user's actual
        // multi-second hold, then re-arming ANOTHER recording on the same
        // physical press the user believes is still their first one. This
        // is a direct structural mechanism for "recording came out blank/
        // near-silent" independent of anything in the DSP or erase-release
        // timing. Fix: only treat this as a genuinely NEW press (reset hold
        // start, run the press-time dispatch) when the pad was not already
        // marked held; a repeat note-on for an already-held pad is a no-op
        // here, exactly as a real key-repeat/aftertouch event should be.
        bool alreadyHeld = m_looperHeld[looper];
        m_looperHeld[looper] = true;
        if (alreadyHeld) {
            // TEMPORARY diagnostic (tracked for removal): confirms live
            // whether real APC hardware actually sends repeated note-on
            // while a pad is physically held (the guard above exists
            // specifically to neutralize this) -- distinguishes this
            // dedup path actually firing from the guard being dead code.
            fprintf(stderr, "[diag8] onPadPress REPEAT-SUPPRESSED looper=%d now_ms=%u recording=%d\n",
                    looper, now_ms, (int)m_looperRecording[looper]);
            return;
        }
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
            applyRecPlayCycle(looper, now_ms, ps, link);
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

void ApcGrid::onPadRelease(int note, unsigned now_ms, ParamStore& ps, LinkBridge* link) {
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
            applyRecPlayCycle(looper, now_ms, ps, link);
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
    // Release any pending erase gate whose delay has elapsed (see the
    // erase-fire branch below for why this can't be an immediate set-then-
    // clear in the same call).
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (m_looperEraseReleaseAt[looper] != 0 && now_ms >= m_looperEraseReleaseAt[looper]) {
            setLooper(ps, looper, "erase", 0.0f);
            fprintf(stderr, "[diag6b] pollHolds ERASE-RELEASE looper=%d now_ms=%u releaseAt=%u\n",
                    looper, now_ms, m_looperEraseReleaseAt[looper]);
            m_looperEraseReleaseAt[looper] = 0;
        }
    }
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (!m_looperHeld[looper] || m_looperErased[looper]) continue;
        if (now_ms - m_looperHoldStart[looper] < kHoldEraseMs) continue;
        // Long-hold -> erase (apcKey25Notes.cpp: a >=1s hold clears the looper
        // regardless of state; also cancels a just-armed press-record).
        //
        // ROOT CAUSE FOUND (via the new MIDI-injection harness, test/hardware/
        // midi-inject.js, scripting this exact sequence with no human at the
        // hardware): this call set "erase" to 1.0 and NEVER released it back
        // to 0 -- the only other writer of looperN/erase is this same line,
        // fired once. dsp/loop.dsp's `wipe = max(clearAll, eraseN)` gates
        // `hold` EVERY block (`hold = delayed * (1-recN) * (1-wipe)`), so once
        // eraseN stuck at 1 forever, this looper's delay-ring recirculation
        // was permanently zeroed -- recording still worked (`record = in*recN`
        // is unaffected by wipe), but playback was silently wiped every
        // single block from then on, matching exactly what was reported:
        // "after clearing it, the second round didn't play after recording"
        // (and, earlier this session, the "blank recording" reports -- not
        // blank at record time, wiped in every subsequent block afterward).
        // Fix: release erase back to 0 after a short real delay, the same
        // momentary held/release shape onClearAll already uses for
        // cmd/clearall (there, note-on sets it, a LATER note-off from the
        // user's own release releases it -- real wall-clock time passes in
        // between, guaranteeing the audio thread's block-rate reads actually
        // observe erase=1 for a while first). Setting it to 1 then
        // immediately back to 0 in the same call would race the audio
        // thread's plain-atomic read with no ordering guarantee -- it could
        // read 0 and never see the wipe at all. Instead: set it now, and
        // record a release deadline pollHolds itself checks on a later tick
        // (it already runs every ~100ms regardless of MIDI traffic) so at
        // least one real control-thread tick's worth of DSP blocks
        // (thousands, at 48kHz/64-sample blocks) genuinely see wipe=1 before
        // it's released.
        setLooper(ps, looper, "erase", 1.0f);
        m_looperEraseReleaseAt[looper] = now_ms + 50;   // ~50ms is many DSP blocks; short enough no user notices a delay
        // TEMPORARY diagnostic (tracked for removal, see diag7 in
        // applyRecPlayCycle): real erase-fire timestamp, to compare directly
        // against the next ARM/FINISH diag7 lines in a live reproduction and
        // confirm precisely how long before the next ARM this fired, and
        // whether the release (diag6b below) genuinely completes first.
        fprintf(stderr, "[diag6] pollHolds ERASE-FIRE looper=%d now_ms=%u holdStart=%u heldMs=%u releaseAt=%u\n",
                looper, now_ms, m_looperHoldStart[looper], now_ms - m_looperHoldStart[looper], m_looperEraseReleaseAt[looper]);
        if (m_looperRecording[looper]) {
            setLooper(ps, looper, "rec", 0.0f);   // cancel the in-progress take, don't leave rec stuck at 1
            m_looperRecording[looper] = false;
        }
        m_looperErased[looper] = true;
        m_looperArmedOnPress[looper] = false;
        m_looperHasContent[looper] = false;
        m_looperPlaying[looper] = false;
        setLooper(ps, looper, "play", 0.0f);   // stop the Faust play gate too, not just the shadow (see onClearAll's matching fix)
        forgetLooperFromPresets(looper);
    }
    // WITNESSED live: clearing via per-looper long-hold erase (not the PLAY
    // button) left m_masterLenSamples/cmd/master_len UNCHANGED -- only
    // onClearAll ever reset those. So erasing the rig's last remaining
    // looper this way, then recording again, reused the STALE phrase length
    // from before the clear (the `else` quantize branch in
    // applyRecPlayCycle, not the FIRST-establish branch) -- reported as "the
    // second loop became a continuation of what the first loop was set up
    // to do instead of starting a new song... cut the loop short and the
    // start of the loop was not where we started recording, the end was
    // correct" (a too-long stale length, tapped from the wrong point,
    // truncated to what was actually just recorded). Mirror onClearAll's
    // reset here too: once erasing this way leaves NO looper with content
    // anywhere in the rig, the master phrase is genuinely gone (matching
    // looper's masterLoopBlocks==0 empty-rig case) and must reset so the
    // next recording re-establishes a fresh phrase from scratch.
    bool anyHasContent = false;
    for (int lp = 0; lp < kLooperCount; lp++) if (m_looperHasContent[lp]) { anyHasContent = true; break; }
    if (!anyHasContent && m_masterLenSamples != 0) {
        m_masterLenSamples = 0;
        ps.setByName("cmd/master_len", 0.0f);
        // cmd/recorded_bpm rides with cmd/master_len (see the "TRUE
        // varispeed" comment where it's established) -- an emptied rig must
        // not leave a stale recorded-tempo reference for the NEXT phrase's
        // Link-ratio computation.
        ps.setByName("cmd/recorded_bpm", 0.0f);
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
void ApcGrid::onClearAll(bool held, ParamStore& ps) {
    // LOOP_COMMAND_CLEAR_ALL (apcKey25Notes.cpp:175). `cmd/clearall` is a
    // HELD momentary value (audio_thread.cpp's `wipe = max(clearAll, eraseN)`
    // gate zeroes the loop ring every block clear is held) -- it must be
    // released on note-off, or every subsequent recording attempt gets wiped
    // every block forever. The release call (held=false) only clears the
    // Faust-side hold; local shadow-state reset happens once, on press.
    ps.setByName("cmd/clearall", held ? 1.0f : 0.0f);
    if (!held) return;
    // Wipe the DSP-side content of every looper AND reset every bit of local
    // shadow state this thread tracks, so a subsequent press correctly
    // re-arms record on an empty looper instead of treating it as still
    // having content from before the clear.
    for (int lp = 0; lp < kLooperCount; lp++) {
        m_looperHeld[lp] = false;
        m_looperErased[lp] = false;
        m_looperArmedOnPress[lp] = false;
        m_looperPlaying[lp] = false;
        m_looperHasContent[lp] = false;
        m_looperRecording[lp] = false;
        m_recordStartMs[lp] = 0;
        // WITNESSED live (real-audio test, after the erase-gate release fix
        // landed): "clearing doesn't stop them" -- this loop only ever reset
        // the C++ SHADOW state (m_looperPlaying=false) and never told the
        // Faust DSP to actually stop: dsp/loop.dsp's `out = loopSig * playN *
        // volN` keeps outputting whatever loopSig currently holds gated by
        // playN, and playN (the "play" checkbox control) was never zeroed
        // here. clearAll wipes the RING CONTENT (hold *= (1-wipe)), but that
        // only silences the ring's own recirculated content -- it does
        // nothing to stop the looper's play gate itself, so anything already
        // mid-recirculation (or a play=1 looper about to resume once wipe
        // releases) kept audibly playing through the clear. Explicitly stop
        // every looper's play gate here, matching what m_looperPlaying=false
        // claims is already true.
        setLooper(ps, lp, "play", 0.0f);
        // ROOT CAUSE FOUND + FIXED at the DSP level (dsp/loop.dsp), 2nd
        // generation fix: the "clear"/"speed" engine-global controls were
        // originally referenced by bare name INSIDE oneLooper, which par(i,
        // NLOOPERS, vgroup(...)) instantiates 20 times, producing 20
        // SEPARATE zones. A first fix attempt (382e775) hoisted the
        // button()/hslider() DECLARATIONS outside the par/vgroup and passed
        // them into oneLooper as ordinary parameters -- but this did NOT
        // actually collapse them to one zone: Faust's par() combinator
        // re-elaborates whatever UI primitives sit inside an argument
        // expression at EACH of its 20 instantiation sites (WITNESSED via
        // generated C++: still 20 "speed"/"clear" zones, each inside its own
        // "looper N" vgroup, even after that fix landed). The REAL fix
        // removes clear/speed as Faust UI zones entirely and threads them in
        // as plain process() SIGNAL inputs instead (like prevFiltIn) --
        // audio_thread.cpp now writes them into fins[2]/fins[3] every block,
        // which par() cannot duplicate since there's no UI primitive to
        // re-elaborate. cmd/clearall now correctly wipes every looper's ring
        // in one write, with no C++-side per-looper compensation needed here.
    }
    for (int p = 0; p < kPresetCount; p++) {
        m_presetHeld[p] = false;
        m_presetCaptured[p] = false;
        m_presetUsed[p] = false;
        m_presetMask[p] = 0;
    }
    // Reset the shared phrase length too (audio_thread.cpp also resets its
    // own copy of cmd/master_len when cmd/clearall is held, but resetting it
    // here as well means a subsequent press in THIS thread sees 0 immediately
    // rather than depending on a race with the audio thread's next block).
    m_masterLenSamples = 0;
    ps.setByName("cmd/master_len", 0.0f);
    ps.setByName("cmd/recorded_bpm", 0.0f);   // see the "TRUE varispeed" comment above
}
void ApcGrid::onKeybedNoteOn(int note, ParamStore& ps, Sampler* sampler) {
    // apcKey25.cpp:103-125: the sampler takes the keys when it has content.
    // In drum-record mode (button 66 held) a key press records into THAT
    // key's drum slot. Otherwise, if a chromatic sample is loaded OR this key
    // has its own drum slot, the key triggers sampler playback (live-pitch
    // keyboard transpose is suppressed for that key; live pitch stays
    // reachable via mod-wheel/CC52/note-64). With no sampler content, the key
    // falls through to live-pitch exactly as before this feature was built.
    if (sampler) {
        int keyIdx = Sampler::keyIndex(note);
        if (m_drumRecordMode) {
            if (keyIdx >= 0) sampler->pushEvent(Sampler::EV_REC_START, keyIdx, 0);
            return;
        }
        if (sampler->chromaticLoaded() || sampler->drumLoaded(keyIdx)) {
            sampler->pushEvent(Sampler::EV_NOTE_ON, note, 127);
            return;
        }
    }
    // apcKey25.cpp:122-123: any keybed key press engages live-pitch at that
    // key's own semitone offset from middle C-ish (note 60), unconditionally.
    m_liveEngaged = true;
    float semis = (float)(note - 60);
    ps.setByName("fx/pitchbend", semis);
    ps.setByName("fx/pitchbend_engaged", 1.0f);
}
void ApcGrid::onKeybedNoteOff(int note, Sampler* sampler) {
    // apcKey25.cpp:211-228: mirror the note-on routing on release. The
    // sampler NOTE_OFF is forwarded UNCONDITIONALLY (not gated on
    // chromaticLoaded/drumLoaded) -- gating it could suppress the release if
    // content changed between press and release, stranding a sustaining
    // voice (the "auto-sustain" bug looper's own comment names explicitly).
    if (!sampler) return;
    if (m_drumRecordMode) {
        int keyIdx = Sampler::keyIndex(note);
        if (keyIdx >= 0) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
        return;
    }
    sampler->pushEvent(Sampler::EV_NOTE_OFF, note, 0);
}
void ApcGrid::onSamplerBtn65Press(Sampler* sampler) {
    // apcKey25.cpp:160-162: 65 HELD records the shared chromatic sample.
    if (sampler) sampler->pushEvent(Sampler::EV_REC_START, -1, 0);
}
void ApcGrid::onSamplerBtn65Release(Sampler* sampler) {
    // apcKey25.cpp:198-200: release stops + auto-trims.
    if (sampler) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
}
void ApcGrid::onSamplerBtn66Press() {
    // apcKey25.cpp:164-167: arm drum-record-mode (gates channel-1 key routing
    // in onKeybedNoteOn/Off above); does not itself start any capture.
    m_drumRecordMode = true;
}
void ApcGrid::onSamplerBtn66Release(Sampler* sampler) {
    // apcKey25.cpp:202-208: disarm, AND stop any in-progress drum capture
    // (idempotent if none) so releasing 66 before the key never leaves a
    // record armed.
    m_drumRecordMode = false;
    if (sampler) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
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
