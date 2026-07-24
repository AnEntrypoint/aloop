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
// without reading back audio state cross-thread FOR MOST fields — the same
// shape looper's publicTrack read serves. UPDATED (ARM-QUANTIZATION, this
// session): applyRecPlayCycle now DOES read back one piece of audio-thread
// state (writeIdx telemetry, via AudioThread::snapshotTelemetry(), the same
// read-only cross-thread channel midi.cpp already uses for LED refresh) --
// specifically to compensate for ARM-quantization's press-to-grid-tick
// timing gap, which a purely-local wall-clock estimate cannot see.

#include "apc_grid.h"
#include "../dsp/sampler/sampler.h"
#include "../dsp/audio_thread.h"
#include "../host/lv2_host.h"
#include "../link/link_bridge.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace aloop {

void ApcGrid::bindAll(ParamStore& ps) {
    char name[32];
    for (int looper = 0; looper < kLooperCount; looper++) {
        for (const char* field : {"rec", "play", "erase", "finishreq"}) {
            snprintf(name, sizeof name, "looper%d/%s", looper, field);
            ps.bind(name);
        }
        // finishtarget: the quantized sample-count target for
        // finish-quantization (see dsp/loop.dsp's oneLooper comment) --
        // 0 default matches Faust's own hslider("finishtarget", 0, ...)
        // compiled default.
        snprintf(name, sizeof name, "looper%d/finishtarget", looper);
        ps.bind(name, 0.0f);
        // sidechainsrc: per-looper sidechain-pump source designation (LOFI
        // feature) -- matches dsp/loop.dsp's oneLooper isSourceN checkbox,
        // 0 default (not a source), toggled via onSidechainLooperToggle.
        snprintf(name, sizeof name, "looper%d/sidechainsrc", looper);
        ps.bind(name, 0.0f);
    }
    ps.bind("fx/pitchbend");
    ps.bind("fx/pitchbend_engaged");
    ps.bind("fx/microrepeat_div");
    ps.bind("fx/monitorfold");
    ps.bind("fx/formant");
    ps.bind("cmd/master_len", 0.0f);   // local master-phrase length (samples), 0 = none established yet
    ps.bind("cmd/recorded_bpm", 0.0f); // TRUE varispeed: BPM the shared phrase was recorded at, 0 = none established yet

    // --- 3-bank fx control-surface (LOFI feature) ---------------------------
    // fx/reverb, fx/delay, fx/time, fx/hp, fx/lpres, fx/lp, fx/pitch are now
    // bound HERE (moved out of controls.conf's flat map -- see
    // config/controls.conf's own updated comment) with the Dub bank's
    // defaults, since ApcGrid now owns which bank's stored value currently
    // drives each shared Faust zone (see m_fxBankValues/pushBankValuesToZones)
    // -- these 7 targets must never ALSO be bound by the flat map's own
    // ps.bind() loop in midi.cpp, or the two competing default-seed values
    // would race at startup with no defined winner.
    ps.bind("fx/reverb",  0.0f);
    ps.bind("fx/delay",   0.0f);
    ps.bind("fx/time",    0.5f);
    ps.bind("fx/hp",      0.0f);
    ps.bind("fx/lpres",   0.0f);
    ps.bind("fx/lp",      1.0f);
    ps.bind("fx/pitch",   0.0f);
    // NOTE: this used to also ps.bind("fx/bank", ...) -- a selector zone for
    // an in-Faust 3-way crossfade in effects_runtime.dsp. WITNESSED live on a
    // real Pi 4 this session: that crossfade computed all 3 banks' full
    // effect chains every block regardless of which was "selected" (Faust
    // has no runtime branching), a real ~7pp core_busy regression causing
    // continuous audio dropouts. effects_runtime.dsp is restored to its
    // pre-LOFI dub-only chain and has no fx/bank zone anymore -- guitar and
    // lofi-fx's effects moved to their own permanent Core-3 LV2 bundle
    // (guitar_lofi_fx.dsp) instead, always active, never gated by a selector.
}

static void setLooper(ParamStore& ps, int looper, const char* field, float v) {
    char name[32];
    snprintf(name, sizeof name, "looper%d/%s", looper, field);
    ps.setByName(name, v);
}

// Ported from ../looper's real linkDeriveQuant (abletonLink.cpp:884-907),
// confirmed via cross-codebase research this session. User's explicit
// standing requirement: "all loopers must absolutely and permanently stick
// and track to ableton links phrasing, they must all run on a multiple or
// division of it... the ableton link phrase must be as close to 4 bars 120
// as we can get it adjusting the tempo or division or multiple of 4 bars
// accordingly, just like ../looper." The fixed base quantum is 4 bars = 16
// beats (LinkBridge's own quantum, see link_bridge.h/.cpp); candidates are
// multiples/divisions OF that 16-beat base (1/16, 1/8, 1/4, 1/2, 1x, 2x, 4x,
// 8x -- i.e. 1,2,4,8,16,32,64,128 beats), mirroring looper's own candidate
// SET SHAPE (its own {0.25,0.5,1,2,4,8,16} beats, rebased here onto a 4-bar
// rather than looper's implicit 1-bar unit, per the user's explicit "4
// bars" framing). For each candidate, the implied tempo is beats*60/seconds;
// whichever candidate's implied tempo lands NEAREST 120 BPM wins (preferring
// the [80,160] window if any candidate lands inside it, exactly mirroring
// looper's own fallback-outside-window behavior) -- this is what lets a
// short "one hit" loop resolve to a small beat-count instead of being
// force-fit to a full 4-bar phrase at an absurd tempo.
struct TempoSolveResult {
    double bpm;
    double beats;   // the winning candidate's beat-count (this loop's phrase length in beats)
};
static TempoSolveResult deriveTempoQuant(double seconds) {
    if (seconds <= 0.0) return {120.0, 16.0};
    static const double kCandidates[] = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0};
    TempoSolveResult best = {120.0, 16.0};
    double bestDist = 1e18;
    bool bestInWindow = false;
    for (double beats : kCandidates) {
        double bpm = 60.0 * beats / seconds;
        bool inWindow = (bpm >= 80.0 && bpm <= 160.0);
        double dist = std::fabs(bpm - 120.0);
        // Prefer any in-window candidate over any out-of-window one, then
        // nearest-120 within whichever preference tier wins -- matches
        // looper's own "within [80,160] window, else nearest-120 outside it".
        bool better = inWindow && !bestInWindow;
        bool tieBreak = (inWindow == bestInWindow) && (dist < bestDist);
        if (better || tieBreak) {
            best = {bpm, beats};
            bestDist = dist;
            bestInWindow = inWindow;
        }
    }
    return best;
}
// Backward-compatible BPM-only accessor for call sites that don't yet need
// the beat-count (the phrase-length-in-beats result feeds the masterPhase
// rearchitecture, tracked separately).
static double deriveTempoBpm(double seconds) { return deriveTempoQuant(seconds).bpm; }

void ApcGrid::applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps, LinkBridge* link, AudioThread* audio) {
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
            // WITNESSED + CORRECTED (user's explicit spec this session):
            // "loop 1 is supposed to immediately and precisely repeat its
            // exact start/stop of the recording, exactly like a commercial
            // looper" -- a wall-clock (now_ms) estimate can never satisfy
            // this: it was never sample-accurate relative to the audio
            // thread's real per-block timeline (real, if small,
            // MIDI-event-to-audio-thread latency from control-thread
            // scheduling/block-buffering), which is exactly why the OLD code
            // here subtracted a hand-tuned 10ms "safety margin" to avoid
            // reading into pre-clear silence -- a permanent, deliberate
            // inaccuracy that itself violates "exact start/stop". Fixed by
            // using the SAME sample-accurate mechanism the subsequent-loop
            // branch below already uses: dsp/loop.dsp's writeIdx telemetry,
            // the DSP's own true elapsed-sample count since the real
            // (grid-aligned) arm instant -- no wall-clock skew, no margin
            // needed at all, since this value can never be longer than what
            // was genuinely written.
            long lenSamples;
            if (audio) {
                auto t = audio->snapshotTelemetry();
                lenSamples = (long)t.looperWriteIdx[looper];
            } else {
                // Defensive fallback only (should not happen in practice --
                // audio is always non-null once the audio thread has
                // started): wall-clock estimate, matching the subsequent-
                // loop branch's own fallback for the same reason.
                unsigned elapsedMs = now_ms - m_recordStartMs[looper];
                lenSamples = (long)elapsedMs * kSampleRate / 1000;
            }
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
            // WITNESSED/CORRECTED (user's explicit, direct correction this
            // session): "loop 1 is supposed to immediately and precisely
            // repeat its exact start/stop of the recording, exactly like a
            // commercial looper... from that point onwards, [Link] controls
            // the loop playback so that other devices can change the tempo
            // from that point on once we've locally set it with the first
            // loop." This REVERSES the previous design (which re-derived
            // m_masterLenSamples from the solver's own beats-at-chosen-BPM
            // reconstruction, quietly resizing the loop to the nearest
            // musical-tempo-implying duration) -- m_masterLenSamples/
            // cmd/master_len must stay EXACTLY the raw recorded length set
            // above (safety-margin already applied), never overwritten by
            // any solver output. deriveTempoQuant is used ONLY to pick a BPM
            // to propose to Link -- the loop's own length is the source of
            // truth Link is phrased to fit, not the other way around.
            double recordedSeconds = (double)m_masterLenSamples / (double)kSampleRate;
            TempoSolveResult solved = deriveTempoQuant(recordedSeconds);
            ps.setByName("cmd/master_len", (float)m_masterLenSamples);
            ps.setByName("cmd/recorded_bpm", (float)solved.bpm);
            // NOTE: no longer writes looperN/len here -- dsp/loop.dsp's oneLooper
            // no longer has a "len" zone at all (removed, dead code: wrapLen is
            // derived purely from writeIdx/finishTargetN at finishEdge, not from
            // any lenN control -- see loop.dsp's own "ROOT CAUSE (silence ever
            // since TRUE varispeed's rwtable redesign)" comment). cmd/master_len
            // above is the one shared value every looper's gridStep calculation
            // actually reads (masterLenBuf, audio_thread.cpp's 6th process()
            // input) -- nothing per-looper is needed for phrase propagation.
            // TWO-WAY LINK INTEGRATION: the first recorded loop ACTIVELY
            // STEERS the Link session's tempo toward the solver's chosen BPM
            // (nearest 120, at a clean multiple/division of the 4-bar
            // quantum) -- not merely reading whatever tempo already exists.
            // Mirrors looper's real linkDeriveQuant+linkEnd behavior
            // (abletonLink.cpp:884-932, confirmed via cross-codebase
            // research this session): propose tempo AND implicitly establish
            // phrase origin together, so peers (and this session's own
            // Link-synced branch) adopt both as one clean restart.
            if (link) {
                link->proposeTempo(solved.bpm);
            }
            // FINISH-QUANTIZATION (see dsp/loop.dsp's oneLooper comment):
            // this looper's own write boundary must land exactly on
            // m_masterLenSamples (the just-solved quantized target), not
            // wherever its raw elapsed recording happened to be -- push the
            // target and pulse finishreq so the DSP extends/trims to it
            // sample-accurately, mirroring every other looper's own
            // finish-quantization path below.
            setLooper(ps, looper, "finishtarget", (float)m_masterLenSamples);
            setLooper(ps, looper, "finishreq", 1.0f);
            m_looperFinishReqReleaseAt[looper] = now_ms + 50;
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
            // ARM-QUANTIZATION compensation: this branch's recordings go
            // through the grid-tick-quantized ARM (masterLen already
            // established, see dsp/loop.dsp's armEdge/gridTickCrossed) --
            // the true recording start can land up to ~1/16 phrase AFTER
            // the press, which a wall-clock press-to-press estimate cannot
            // see. Prefer reading writeIdx's own telemetry (the TRUE
            // elapsed sample count since the real, grid-aligned arm
            // instant, confirmed via cross-codebase research this session
            // that ../looper itself measures duration this same way --
            // from the post-latch start, never the press) when available;
            // fall back to the wall-clock estimate only if audio/telemetry
            // isn't reachable (defensive, should not happen in practice).
            long rawSamples;
            if (audio) {
                auto t = audio->snapshotTelemetry();
                rawSamples = (long)t.looperWriteIdx[looper];
            } else {
                unsigned elapsedMs = now_ms - m_recordStartMs[looper];
                rawSamples = (long)elapsedMs * kSampleRate / 1000;
            }
            // WITNESSED live, two rounds of feedback this session:
            // (1) "it works just fine unless the division loop is short, we
            //     must just support some shorter divisions" -- the smallest
            //     available candidate was 0.25 (M/4), fixed by extending
            //     down to M/16.
            // (2) "recording multiples didnt quite work it didnt take the
            //     whole length just took a piece of the end" / clarified:
            //     "we want to be able to take any multiple of the loop if
            //     the start loop is 1/4 bar and the next loop is 64 bars it
            //     must also work" / "when we press stop of a loop just past
            //     2 bars we dont want to keep playing till 4 bars, it must
            //     cut to whatevers closer to where we pressed, but if we
            //     press past the 3/4 mark [confirmed: 68%], it must keep
            //     recording to the multiple and not cut back."
            // ROOT CAUSE of (2): the old design picked whichever candidate
            // in a SMALL FIXED SET {..., 2.0, 4.0} was closest by raw
            // distance -- with a big gap between 2M and 4M, anything past
            // ~3M (raw distance nearest-neighbor) would jump all the way to
            // 4M, extending recording for a potentially very long time past
            // where the user actually stopped playing, capturing mostly
            // silence/nothing -- "took a piece of the end" exactly matches
            // extending far past the performed content. Also capped at 4M,
            // so a genuinely-intended 8M/16M/64M take had no candidate to
            // reach at all.
            // FIX: an UNBOUNDED geometric candidate sequence (powers of 2,
            // M/16 up through as many multiples as kMaxLoopSamples allows),
            // and instead of "nearest by raw distance across the whole set",
            // find the bracketing PAIR immediately below/above rawSamples,
            // then apply the user's own confirmed 68% threshold: extend to
            // the UPPER candidate only if rawSamples is past 68% of the way
            // from the lower to the upper candidate; otherwise trim to the
            // LOWER one. This bounds the maximum possible "wait" extension
            // to at most 32% of one octave's span from wherever the user
            // actually released -- never a multi-bar jump like the old
            // fixed-set-nearest design could produce.
            // Bracket rawSamples between consecutive powers of 2 (times M)
            // directly via log2 of the ratio -- correct regardless of
            // whether rawSamples is above or below M, no directional walk
            // to get backwards (an earlier draft of this fix had exactly
            // that bug: a lower-only walk starting at M could never find a
            // lower bracket ABOVE M for a raw recording already past M,
            // caught and fixed before this ever reached CI).
            double ratio = (double)rawSamples / (double)m_masterLenSamples;
            if (ratio < 1.0 / 16.0) ratio = 1.0 / 16.0;   // floor: never propose below M/16
            double log2Ratio = std::log2(ratio);
            double lowerExp = std::floor(log2Ratio);
            double lowerCand = (double)m_masterLenSamples * std::pow(2.0, lowerExp);
            double upperCand = (double)m_masterLenSamples * std::pow(2.0, lowerExp + 1.0);
            if (upperCand > (double)kMaxLoopSamples) upperCand = (double)kMaxLoopSamples;
            if (lowerCand > upperCand) lowerCand = upperCand;   // degenerate guard at the ceiling
            double span = upperCand - lowerCand;
            double bestLen;
            if (span <= 0.0) {
                bestLen = lowerCand;   // rawSamples exactly on a candidate (or span collapsed)
            } else {
                double frac = ((double)rawSamples - lowerCand) / span;
                bestLen = (frac >= 0.68) ? upperCand : lowerCand;
            }
            long quantized = (long)(bestLen + 0.5);
            if (quantized < 64) quantized = 64;
            if (quantized > kMaxLoopSamples) quantized = kMaxLoopSamples;
            // FINISH-QUANTIZATION (WITNESSED live: "when our second loop is
            // short, it doesnt take the start and stop timing it its making
            // it longer and offsetting the position instead of matching
            // it"): setLooper(..., "len", ...) alone used to just relabel
            // the Faust len ZONE, which dsp/loop.dsp's wrapLen never reads
            // (it latches from the RAW writeIdx count at finishEdge) -- so
            // this quantization never actually reached the DSP's real write
            // boundary. Fixed: push the quantized target and pulse
            // finishreq, exactly like the FIRST-establish branch above, so
            // Faust itself extends/trims writing to hit this exact target
            // sample-accurately ("wait when waiting is closer, and backdate
            // if pressed just too late").
            setLooper(ps, looper, "finishtarget", (float)quantized);
            setLooper(ps, looper, "finishreq", 1.0f);
            m_looperFinishReqReleaseAt[looper] = now_ms + 50;
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

void ApcGrid::onPadPress(int note, unsigned now_ms, ParamStore& ps, LinkBridge* link, AudioThread* audio) {
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
        // Sidechain-pump gesture (LOFI feature): while guitar-fx is held, a
        // looper press is REDIRECTED entirely to toggling that looper's
        // sidechain-source designation -- it never reaches the normal ARM/
        // FINISH dispatch below at all, and never touches m_looperHeld/
        // m_looperHoldStart (this press is not a "hold this pad" gesture,
        // it's a one-shot toggle consumed by guitar-fx being held instead).
        if (m_guitarFxHeld) {
            onSidechainLooperToggle(looper, ps);
            return;
        }
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
            applyRecPlayCycle(looper, now_ms, ps, link, audio);
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

void ApcGrid::onPadRelease(int note, unsigned now_ms, ParamStore& ps, LinkBridge* link, AudioThread* audio) {
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
            applyRecPlayCycle(looper, now_ms, ps, link, audio);
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
    // Clear the bank-select LED flash once its brief window elapses (see
    // onDubFxPress/onGuitarFxPress/onLofiFxPress and kBankFlashMs) -- same
    // momentary-pulse pattern as the erase/finishreq releases below, just
    // for a UI indicator rather than a DSP gate.
    if (m_bankFlashReleaseAt != 0 && now_ms >= m_bankFlashReleaseAt) {
        m_bankFlashReleaseAt = 0;
    }
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
    // Release any pending finishreq pulse the same momentary-pulse pattern
    // as erase above -- setLooper(..., "finishreq", 1.0f) alone (see
    // applyRecPlayCycle) would stay stuck at 1 forever with nothing else
    // ever writing it back to 0, and dsp/loop.dsp's finishRequestedStep
    // only cares that finishreq was seen >0.5 for at least one sample (it
    // latches into finishRequested until the next armEdge), so holding it
    // for ~50ms (many DSP blocks) then releasing is correct and harmless --
    // it does NOT need to still be 1 by the time the DSP-side target is
    // actually reached (that's what finishRequested's own latch is for).
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (m_looperFinishReqReleaseAt[looper] != 0 && now_ms >= m_looperFinishReqReleaseAt[looper]) {
            setLooper(ps, looper, "finishreq", 0.0f);
            m_looperFinishReqReleaseAt[looper] = 0;
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
        // Sidechain-pump source designation auto-clears on erase (confirmed
        // in the design session): a source tied to specific recorded content
        // shouldn't silently survive that content being wiped -- a fresh
        // recording into this same slot later starts clean, not silently
        // excluded from ducking as a leftover designation.
        m_looperIsSidechainSource[looper] = false;
        setLooper(ps, looper, "sidechainsrc", 0.0f);
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
        // Sidechain-pump source designation auto-clears on CLEAR_ALL too,
        // same reasoning as the per-looper long-hold erase path above.
        m_looperIsSidechainSource[lp] = false;
        setLooper(ps, lp, "sidechainsrc", 0.0f);
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
        // ROOT CAUSE FOUND LIVE (this session, real hardware): CLEAR_ALL
        // pressed WHILE a looper was still mid-recording (rec=1) left that
        // looper's "rec" Faust zone stuck at 1 FOREVER -- this loop resets
        // m_looperRecording[lp]=false (the C++ shadow only) but, exactly the
        // same class of bug the play-gate fix above already documents, never
        // told the Faust DSP itself to stop recording. dsp/loop.dsp's
        // `hold = delayed * (1.0 - recN) * (1.0 - wipe)` is gated to ZERO
        // for as long as recN stays 1 -- so a looper whose rec zone got stuck
        // this way can never play back ANY recirculated loop content again,
        // even after a fresh, otherwise-correct ARM/FINISH cycle re-records
        // it (the NEXT armEdge does reset writeIdx/readPos/wrapLen, but nothing
        // in this clear path ever unstuck the STALE rec=1 zone if the clear
        // happened to land while recording was in progress) -- matching
        // exactly the reported "loops don't play, only passthrough" symptom
        // whenever a clear/interrupt lands mid-recording. Explicitly stop
        // every looper's rec gate here too, matching play's fix above.
        setLooper(ps, lp, "rec", 0.0f);
        // Also release any pending finishreq pulse/target -- a clear landing
        // mid-finish-quantization-extension shouldn't leave a stale
        // finishreq=1 or finishtarget from the interrupted take lingering
        // (harmless either way once the DSP's own armEdge resets
        // finishRequested/writeIdx on the NEXT genuine recording, but
        // explicit hygiene here matches rec/play's own explicit resets).
        setLooper(ps, lp, "finishreq", 0.0f);
        m_looperFinishReqReleaseAt[lp] = 0;
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

// --- 3-bank fx control-surface (LOFI feature) -------------------------------
// The real APC Key25 CC number for each of the 7 knob positions, index-
// matched to m_fxBankValues' second dimension and to every kXxxTargets table
// below -- must stay in sync with config/controls.conf's own documented
// cc48/49/50/51/54/55/57 assignment.
static const int kFxKnobCcNumbers[kFxKnobCount] = { 48, 49, 50, 51, 54, 55, 57 };

// Per-bank permanent target for each of the 7 knob positions (see
// FxKnobTarget's declaration in apc_grid.h for the full redesign rationale:
// unlike the old shared-7-zones model, each bank's knobs now have their OWN
// permanent target, always live regardless of which bank is "selected").
static const FxKnobTarget kDubTargets[kFxKnobCount] = {
    { FxKnobKind::FaustZone, "fx/reverb" },
    { FxKnobKind::FaustZone, "fx/delay"  },
    { FxKnobKind::FaustZone, "fx/time"   },
    { FxKnobKind::FaustZone, "fx/hp"     },
    { FxKnobKind::FaustZone, "fx/lpres"  },
    { FxKnobKind::FaustZone, "fx/lp"     },
    { FxKnobKind::FaustZone, "fx/pitch"  },
};
static const FxKnobTarget kGuitarTargets[kFxKnobCount] = {
    { FxKnobKind::Lv2Control, "fx2/FLANGEAMT"   },
    { FxKnobKind::Lv2Control, "fx2/TREMOLOAMT"  },
    { FxKnobKind::Lv2Control, "fx2/BANKSPEED"   },
    { FxKnobKind::Lv2Control, "fx2/PHASERAMT"   },
    { FxKnobKind::SamplerAttackMs,  nullptr },
    { FxKnobKind::SamplerReleaseMs, nullptr },
    { FxKnobKind::Lv2Control, "fx2/COMPRESSAMT" },
};
static const FxKnobTarget kLofiFxTargets[kFxKnobCount] = {
    { FxKnobKind::Lv2Control, "fx2/BITCRUSHAMT" },
    { FxKnobKind::Lv2Control, "fx2/VINYLAMT"    },
    { FxKnobKind::Lv2Control, "fx2/FLUTTERAMT"  },
    { FxKnobKind::Lv2Control, "fx2/SRRAMT"      },
    { FxKnobKind::SamplerGrainSizeMs,    nullptr },
    { FxKnobKind::SamplerGrainDensityHz, nullptr },
    { FxKnobKind::SamplerScanRate,       nullptr },
};

// Attack/release/grain-size/grain-density/scan-rate are Sampler-native
// controls whose real ranges (ms/Hz/multiplier) are NOT 0..1 like every
// other knob here -- each knob's raw 0..1 CC value is mapped to that
// control's own natural range before calling into Sampler, matching the
// range each setter already documents/clamps to in sampler.h.
static void applySamplerFxKnob(FxKnobKind kind, float v01, Sampler* sampler) {
    if (!sampler) return;
    switch (kind) {
        case FxKnobKind::SamplerAttackMs:       sampler->setAttackMs(v01 * 2000.0f); break;
        case FxKnobKind::SamplerReleaseMs:      sampler->setReleaseMs(v01 * 2000.0f); break;
        // Touching any granulator knob turns the granulator on -- matching
        // setAttackMs/setReleaseMs's own "calling this at all is a one-way
        // switch off the legacy default" pattern in sampler.h. Without this,
        // dialing grain size/density/scan rate would silently do nothing
        // audible (m_granOn defaults off and nothing else ever enables it).
        case FxKnobKind::SamplerGrainSizeMs:
            sampler->setGranulatorEnabled(true);
            sampler->setGrainSizeMs(5.0f + v01 * 495.0f);
            break;
        case FxKnobKind::SamplerGrainDensityHz:
            sampler->setGranulatorEnabled(true);
            sampler->setGrainDensityHz(0.5f + v01 * 199.5f);
            break;
        case FxKnobKind::SamplerScanRate:
            sampler->setGranulatorEnabled(true);
            sampler->setScanRate(v01 * 8.0f);
            break;
        default: break;
    }
}

static void applyFxKnobTarget(const FxKnobTarget& t, float v01, ParamStore& ps, Sampler* sampler, Lv2Host* homeFx) {
    switch (t.kind) {
        case FxKnobKind::FaustZone:  ps.setByName(t.name, v01); break;
        case FxKnobKind::Lv2Control: if (homeFx) homeFx->setControl(t.name, v01); break;
        default: applySamplerFxKnob(t.kind, v01, sampler); break;
    }
}

void ApcGrid::onFxKnobCC(int ccNumber, uint8_t data2, ParamStore& ps, Sampler* sampler, Lv2Host* homeFx) {
    int knobIdx = -1;
    for (int k = 0; k < kFxKnobCount; k++) {
        if (kFxKnobCcNumbers[k] == ccNumber) { knobIdx = k; break; }
    }
    if (knobIdx < 0) return;   // not one of the 7 known knob CCs
    float v = (float)data2 / 127.0f;
    m_fxBankValues[(int)m_activeBank][knobIdx] = v;
    const FxKnobTarget* targets =
        m_activeBank == FxBank::Dub ? kDubTargets :
        m_activeBank == FxBank::Guitar ? kGuitarTargets : kLofiFxTargets;
    applyFxKnobTarget(targets[knobIdx], v, ps, sampler, homeFx);
}

// now_ms==0 is a valid real timestamp only in theory (process start) -- to
// keep bankFlashActive()'s "!= 0 means active" check unambiguous even in
// that edge instant, treat a would-be-zero deadline as 1ms instead. This
// mirrors ApcLeds::refresh's own bootMs_ zero-is-sentinel handling elsewhere
// in this control surface for the identical reason.
static unsigned nonZeroDeadline(unsigned now_ms, unsigned windowMs) {
    unsigned d = now_ms + windowMs;
    return d != 0 ? d : 1;
}

// REDESIGN (Core-3 move): bank switches are now a PURE UI/state change --
// see apc_grid.h's updated comment on why there is nothing left to re-push.
// Each press only flips m_activeBank (which knob targets the next CC touch
// reaches) and starts the LED flash window.
void ApcGrid::onDubFxPress(unsigned now_ms, ParamStore& /*ps*/) {
    m_activeBank = FxBank::Dub;
    m_bankFlashWhich = FxBank::Dub;
    m_bankFlashReleaseAt = nonZeroDeadline(now_ms, kBankFlashMs);
}
void ApcGrid::onLofiFxPress(unsigned now_ms, ParamStore& /*ps*/) {
    m_activeBank = FxBank::LofiFx;
    m_bankFlashWhich = FxBank::LofiFx;
    m_bankFlashReleaseAt = nonZeroDeadline(now_ms, kBankFlashMs);
}
void ApcGrid::onGuitarFxPress(unsigned now_ms, ParamStore& /*ps*/) {
    // Press-time bank-select fires immediately (matching dub-fx/lofi-fx's own
    // press-time switch) -- the hold-for-sidechain gesture is layered on TOP
    // of this, not instead of it, exactly mirroring how the main pad grid's
    // ARM/FINISH already fires on press while a SEPARATE flag
    // (m_looperArmedOnPress) suppresses only the matching RELEASE's tap.
    // Selecting the bank on every guitar-fx press (even ones that turn out to
    // be sidechain-holds) is deliberately harmless: re-selecting the SAME
    // bank a press already had active is now simply a no-op (nothing to
    // re-push under the Core-3 redesign).
    m_activeBank = FxBank::Guitar;
    m_bankFlashWhich = FxBank::Guitar;
    m_bankFlashReleaseAt = nonZeroDeadline(now_ms, kBankFlashMs);
    m_guitarFxHeld = true;
    m_guitarFxConsumedByLooperPress = false;
}
void ApcGrid::onGuitarFxRelease(ParamStore& /*ps*/) {
    m_guitarFxHeld = false;
    m_guitarFxConsumedByLooperPress = false;
}

void ApcGrid::onSidechainLooperToggle(int looper, ParamStore& ps) {
    // Toggle (not just set) per the confirmed design: press once while
    // guitar-fx is held to ADD this looper as a source, press again (still
    // held) to REMOVE it. Persists after guitar-fx is released. Multiple
    // simultaneous sources are allowed -- their envelopes combine via max/peak
    // in audio_thread.cpp's sidechain-envelope computation, reading this same
    // looperN/sidechainsrc zone (pushed generically via targetToZone/forEach,
    // no special-cased per-block C++ needed) alongside looperLevel[] telemetry.
    if (looper < 0 || looper >= kLooperCount) return;
    m_looperIsSidechainSource[looper] = !m_looperIsSidechainSource[looper];
    setLooper(ps, looper, "sidechainsrc", m_looperIsSidechainSource[looper] ? 1.0f : 0.0f);
    m_guitarFxConsumedByLooperPress = true;   // suppress guitar-fx's own release-tap bank-reselect noise, matching m_looperArmedOnPress's shape
}

} // namespace aloop
