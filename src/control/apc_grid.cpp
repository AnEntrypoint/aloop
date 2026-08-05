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
        snprintf(name, sizeof name, "looper%d/finishtarget", looper);
        ps.bind(name, 0.0f);
        snprintf(name, sizeof name, "looper%d/latencybias", looper);
        ps.bind(name, 0.0f);
        snprintf(name, sizeof name, "looper%d/sidechainsrc", looper);
        ps.bind(name, 0.0f);
    }
    ps.bind("fx/pitchbend");
    ps.bind("fx/pitchbend_engaged");
    char xposeName[24];
    for (int v = 0; v < kTransposeVoices; v++) {
        snprintf(xposeName, sizeof xposeName, "fx/xpose%d/note", v);
        ps.bind(xposeName, 0.0f);
        snprintf(xposeName, sizeof xposeName, "fx/xpose%d/gate", v);
        ps.bind(xposeName, 0.0f);
    }
    ps.bind("fx/microrepeat_div");
    ps.bind("fx/monitorfold");
    ps.bind("fx/formant");
    ps.bind("cmd/master_len", 0.0f);
    ps.bind("cmd/recorded_bpm", 0.0f);

    ps.bind("fx/reverb",  0.0f);
    ps.bind("fx/delay",   0.0f);
    ps.bind("fx/time",    0.5f);
    ps.bind("fx/hp",      0.0f);
    ps.bind("fx/lpres",   0.0f);
    ps.bind("fx/lp",      1.0f);
    ps.bind("fx/pitch",   0.0f);
}

static void setLooper(ParamStore& ps, int looper, const char* field, float v) {
    char name[32];
    snprintf(name, sizeof name, "looper%d/%s", looper, field);
    ps.setByName(name, v);
}

struct TempoSolveResult {
    double bpm;
    double beats;
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
static double deriveTempoBpm(double seconds) { return deriveTempoQuant(seconds).bpm; }

constexpr long kShiftFoldBlockLatencySamples = 64;

// Publish OUR transport to the Link session whenever it changes. Start-stop-sync
// is enabled on both this project and ../esp-idf-link, but a peer that only ever
// READS isPlaying is half-wired -- the ESP acts on a peer's transport (MIDI
// Start/Stop + all-notes-off) and would never hear from us otherwise.
// "Playing" for aloop = at least one looper is playing.
// Ableton's own Test Plan, STARTSTOPSTATE-1: with Link + Start Stop Sync on, a
// peer pressing Play must START this app "according to its quantization", and a
// peer stopping must stop it. aloop previously only READ the session transport
// into telemetry and acted on none of it, so it satisfied STARTSTOPSTATE-2
// (sending) but failed -1 (listening).
//
// Precedence: a remote transport edge drives loopers that already have content;
// it never arms a recording and never erases. A local pad press still wins for
// the looper it touches, because publishTransport() immediately re-publishes our
// resulting state to the session.
//
// Start is deferred to the next quantum boundary rather than applied instantly
// -- that is what "according to its quantization" means, and it mirrors
// ../esp-idf-link's own pending-realign-then-honor-at-phrase-boundary shape.
// Stop is immediate: stopping late is audible, and no spec wants a stop to wait.
void ApcGrid::applyRemoteTransport(ParamStore& ps, LinkBridge* link) {
    if (!link) return;
    LinkSnapshot ls = link->audioRead();
    if (!ls.synced) return;              // no session, nothing to follow

    if (ls.isPlaying != m_lastSeenRemotePlaying) {
        m_lastSeenRemotePlaying = ls.isPlaying;
        if (ls.isPlaying) {
            m_remoteStartPending = true;   // honored at the next quantum boundary
        } else {
            for (int lp = 0; lp < kLooperCount; lp++) {
                if (!m_looperPlaying[lp]) continue;
                setLooper(ps, lp, "play", 0.0f);
                m_looperPlaying[lp] = false;
            }
            m_remoteStartPending = false;
            m_lastPublishedPlaying = false;   // matches the session; don't re-emit
        }
        return;
    }

    if (!m_remoteStartPending) return;
    // Honor the pending start once the shared phase wraps past the quantum start.
    // beatPhaseMicroBeats counts 0..quantumMicroBeats; a wrap means a boundary.
    if (ls.quantumMicroBeats <= 0) return;
    bool wrapped = (ls.beatPhaseMicroBeats < m_lastRemotePhaseMicroBeats);
    m_lastRemotePhaseMicroBeats = ls.beatPhaseMicroBeats;
    if (!wrapped) return;

    m_remoteStartPending = false;
    for (int lp = 0; lp < kLooperCount; lp++) {
        if (!m_looperHasContent[lp] || m_looperPlaying[lp]) continue;
        setLooper(ps, lp, "play", 1.0f);
        m_looperPlaying[lp] = true;
    }
    m_lastPublishedPlaying = true;        // matches the session; don't re-emit
}

void ApcGrid::publishTransport(LinkBridge* link) {
    if (!link) return;
    bool anyPlaying = false;
    for (int lp = 0; lp < kLooperCount; lp++) {
        if (m_looperPlaying[lp]) { anyPlaying = true; break; }
    }
    if (anyPlaying == m_lastPublishedPlaying) return;   // edge only
    m_lastPublishedPlaying = anyPlaying;
    link->setTransportPlaying(anyPlaying);
}

void ApcGrid::applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps, LinkBridge* link, AudioThread* audio) {
    if (m_looperRecording[looper]) {
        setLooper(ps, looper, "rec", 0.0f);
        m_looperRecording[looper] = false;
        m_looperHasContent[looper] = true;
        m_looperPlaying[looper] = true;
        setLooper(ps, looper, "play", 1.0f);
        fprintf(stderr, "[diag7] FINISH looper=%d now_ms=%u recordStart=%u elapsedMs=%u masterLen(before)=%ld erase_zone=%.2f\n",
                looper, now_ms, m_recordStartMs[looper], now_ms - m_recordStartMs[looper], m_masterLenSamples,
                ps.get("looper" + std::to_string(looper) + "/erase", -1.0f));
        // Recording always taps prevFiltOut (audio_thread.cpp: prevFiltOut =
        // rawFiltTap, assigned at the END of each block, so renderInto()'s
        // NEXT block reads the PREVIOUS block's fully-effected output) --
        // this is a genuine, fixed, ALWAYS-PRESENT one-block recording delay
        // (kBlockSize samples), not a SHIFT-specific artifact. Previously
        // only the SHIFT-fold's own ADDITIONAL block of lag
        // (kShiftFoldBlockLatencySamples) was compensated, leaving every
        // plain (no-SHIFT) recording playing back kBlockSize samples later
        // than it was actually performed -- "a tiny fraction late". Every
        // recording now gets the baseline block compensated; a SHIFT-held
        // take gets the fold's extra block ON TOP of that baseline.
        long latencyBias = kBlockSize + (m_looperShiftHeldDuringTake[looper] ? kShiftFoldBlockLatencySamples : 0);
        setLooper(ps, looper, "latencybias", (float)latencyBias);
        m_masterLenSamples = (long)ps.get("cmd/master_len", 0.0f);
        if (m_masterLenSamples == 0) {
            long lenSamples;
            if (audio) {
                auto t = audio->snapshotTelemetry();
                lenSamples = (long)t.looperWriteIdx[looper];
            } else {
                unsigned elapsedMs = now_ms - m_recordStartMs[looper];
                lenSamples = (long)elapsedMs * kSampleRate / 1000;
            }
            if (lenSamples < 64) lenSamples = 64;
            if (lenSamples > kMaxLoopSamples) lenSamples = kMaxLoopSamples;
            m_masterLenSamples = lenSamples;
            ps.setByName("cmd/master_len", (float)m_masterLenSamples);
            double recordedSeconds = (double)m_masterLenSamples / (double)kSampleRate;
            TempoSolveResult solved = deriveTempoQuant(recordedSeconds);
            ps.setByName("cmd/master_len", (float)m_masterLenSamples);
            ps.setByName("cmd/recorded_bpm", (float)solved.bpm);
            if (link) {
                link->proposeTempo(solved.bpm);
            }
            setLooper(ps, looper, "finishtarget", (float)m_masterLenSamples);
            setLooper(ps, looper, "finishreq", 1.0f);
            m_looperFinishReqReleaseAt[looper] = now_ms + 50;
        } else {
            long rawSamples;
            if (audio) {
                auto t = audio->snapshotTelemetry();
                rawSamples = (long)t.looperWriteIdx[looper];
            } else {
                unsigned elapsedMs = now_ms - m_recordStartMs[looper];
                rawSamples = (long)elapsedMs * kSampleRate / 1000;
            }
            // rawSamples is writeIdx's real elapsed sample count -- always
            // real-time, never varispeed-corrected (see loop.dsp's writeIdx,
            // a plain +1/sample counter with no effSpeed dependency), and
            // wrapLen/finishtarget MUST stay in that same real-sample space:
            // the ring (`rwtable(MAXLEN,...,writeIdx,writeVal,readIdx0)`) is
            // indexed at write time by real writeIdx, so readIdx0/readIdx1
            // must land in the identical real-sample space or playback reads
            // the wrong ring position entirely, not just the wrong speed.
            //
            // But m_masterLenSamples was sized in real samples AT THE TIME
            // THE MASTER WAS RECORDED, and playback applies ONE shared
            // effSpeed = recordedBpm/currentLinkBpm to every looper's read
            // position (see AGENTS.md close-tempo phasing / audio_thread.cpp's
            // linkSpeedRatio) to compensate for Link's CURRENT tempo having
            // drifted from that original recorded tempo. If Link's tempo has
            // already changed by the time THIS recording happens, rawSamples
            // reflects the CURRENT (already-drifted) tempo -- comparing it
            // directly against m_masterLenSamples (sized at the ORIGINAL
            // tempo) picks the wrong quantize candidate, and this looper then
            // also inherits the SAME playback-time effSpeed correction meant
            // to compensate for a drift that doesn't apply to content already
            // recorded at the current tempo. Fix: only the QUANTIZE DECISION
            // (which power-of-2 candidate of m_masterLenSamples to snap to)
            // is made in original-tempo-equivalent space; the stored
            // wrapLen/finishtarget stays in real-sample space by converting
            // the chosen candidate back.
            double tempoScale = 1.0;
            if (link) {
                float recordedBpm = ps.get("cmd/recorded_bpm", 0.0f);
                double curBpm = link->audioRead().bpm;
                if (recordedBpm > 1.0f && curBpm > 1.0) {
                    tempoScale = (double)recordedBpm / curBpm;
                }
            }
            double effectiveSamples = (double)rawSamples * tempoScale;
            double log2Ratio = std::log2(effectiveSamples / (double)m_masterLenSamples);
            double lowerExp = std::floor(log2Ratio);
            if (lowerExp < -4.0) lowerExp = -4.0;
            double lowerCand = (double)m_masterLenSamples * std::pow(2.0, lowerExp);
            double upperCand = (double)m_masterLenSamples * std::pow(2.0, lowerExp + 1.0);
            if (upperCand > (double)kMaxLoopSamples) upperCand = (double)kMaxLoopSamples;
            if (lowerCand > upperCand) lowerCand = upperCand;
            double bestLen;
            if (upperCand <= lowerCand) {
                bestLen = lowerCand;
            } else {
                double midpoint = std::sqrt(lowerCand * upperCand);
                bestLen = (effectiveSamples >= midpoint) ? upperCand : lowerCand;
            }
            // bestLen is in original-tempo-equivalent space (derived from
            // m_masterLenSamples by powers of 2) -- convert back to real
            // elapsed-sample space (matching writeIdx's own indexing) before
            // storing as finishtarget/wrapLen.
            long quantized = (long)(bestLen / tempoScale + 0.5);
            if (quantized < 64) quantized = 64;
            if (quantized > kMaxLoopSamples) quantized = kMaxLoopSamples;
            fprintf(stderr, "[diag9] QUANT looper=%d rawSamples=%ld effectiveSamples=%.0f lowerCand=%.0f upperCand=%.0f tempoScale=%.5f quantized=%ld ratio=%.4f\n",
                    looper, rawSamples, effectiveSamples, lowerCand, upperCand, tempoScale, quantized, effectiveSamples / (double)m_masterLenSamples);
            setLooper(ps, looper, "finishtarget", (float)quantized);
            setLooper(ps, looper, "finishreq", 1.0f);
            m_looperFinishReqReleaseAt[looper] = now_ms + 50;
        }
    } else if (!m_looperHasContent[looper]) {
        setLooper(ps, looper, "rec", 1.0f);
        m_looperRecording[looper] = true;
        m_recordStartMs[looper] = now_ms;
        m_looperShiftHeldDuringTake[looper] = ps.get("fx/monitorfold", 0.0f) > 0.5f;
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
    // Every branch above can change whether ANY looper is playing (FINISH starts
    // one, pause/resume toggles one). Publish the resulting transport to Link.
    publishTransport(link);
}

void ApcGrid::forgetLooperFromPresets(int looper) {
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
        if (m_guitarFxHeld) {
            onSidechainLooperToggle(looper, ps);
            return;
        }
        bool alreadyHeld = m_looperHeld[looper];
        m_looperHeld[looper] = true;
        if (alreadyHeld) {
            fprintf(stderr, "[diag8] onPadPress REPEAT-SUPPRESSED looper=%d now_ms=%u recording=%d\n",
                    looper, now_ms, (int)m_looperRecording[looper]);
            return;
        }
        m_looperErased[looper] = false;
        m_looperHoldStart[looper] = now_ms;
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
            if (m_presetUsed[preset]) applyPreset(preset, ps);
        }
        m_presetHeld[preset] = false;
        return;
    }
}

void ApcGrid::pollHolds(unsigned now_ms, ParamStore& ps, LinkBridge* link) {
    if (m_bankFlashReleaseAt != 0 && now_ms >= m_bankFlashReleaseAt) {
        m_bankFlashReleaseAt = 0;
    }
    // Follow the session's transport (Test Plan STARTSTOPSTATE-1). Runs on every
    // control tick so a peer's start lands within one quantum boundary.
    applyRemoteTransport(ps, link);
    bool shiftHeldNow = ps.get("fx/monitorfold", 0.0f) > 0.5f;
    if (shiftHeldNow) {
        for (int looper = 0; looper < kLooperCount; looper++) {
            if (m_looperRecording[looper]) m_looperShiftHeldDuringTake[looper] = true;
        }
    }
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (m_looperEraseReleaseAt[looper] != 0 && now_ms >= m_looperEraseReleaseAt[looper]) {
            setLooper(ps, looper, "erase", 0.0f);
            fprintf(stderr, "[diag6b] pollHolds ERASE-RELEASE looper=%d now_ms=%u releaseAt=%u\n",
                    looper, now_ms, m_looperEraseReleaseAt[looper]);
            m_looperEraseReleaseAt[looper] = 0;
        }
    }
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (m_looperFinishReqReleaseAt[looper] != 0 && now_ms >= m_looperFinishReqReleaseAt[looper]) {
            setLooper(ps, looper, "finishreq", 0.0f);
            m_looperFinishReqReleaseAt[looper] = 0;
        }
    }
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (!m_looperHeld[looper] || m_looperErased[looper]) continue;
        if (now_ms - m_looperHoldStart[looper] < kHoldEraseMs) continue;
        setLooper(ps, looper, "erase", 1.0f);
        m_looperEraseReleaseAt[looper] = now_ms + 50;
        fprintf(stderr, "[diag6] pollHolds ERASE-FIRE looper=%d now_ms=%u holdStart=%u heldMs=%u releaseAt=%u\n",
                looper, now_ms, m_looperHoldStart[looper], now_ms - m_looperHoldStart[looper], m_looperEraseReleaseAt[looper]);
        if (m_looperRecording[looper]) {
            setLooper(ps, looper, "rec", 0.0f);
            m_looperRecording[looper] = false;
        }
        m_looperErased[looper] = true;
        m_looperArmedOnPress[looper] = false;
        m_looperHasContent[looper] = false;
        m_looperPlaying[looper] = false;
        setLooper(ps, looper, "play", 0.0f);
        forgetLooperFromPresets(looper);
        m_looperIsSidechainSource[looper] = false;
        setLooper(ps, looper, "sidechainsrc", 0.0f);
    }
    bool anyHasContent = false;
    for (int lp = 0; lp < kLooperCount; lp++) if (m_looperHasContent[lp]) { anyHasContent = true; break; }
    if (!anyHasContent && m_masterLenSamples != 0) {
        m_masterLenSamples = 0;
        ps.setByName("cmd/master_len", 0.0f);
        ps.setByName("cmd/recorded_bpm", 0.0f);
    }
    for (int p = 0; p < kPresetCount; p++) {
        if (!m_presetHeld[p] || m_presetCaptured[p]) continue;
        if (now_ms - m_presetHoldStart[p] < kHoldEraseMs) continue;
        capturePreset(p, ps);
        m_presetCaptured[p] = true;
    }
}

void ApcGrid::capturePreset(int p, ParamStore&) {
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
        if (!m_looperHasContent[n]) continue;
        bool shouldPlay = (mask & (1u << n)) != 0;
        if (shouldPlay != m_looperPlaying[n]) {
            setLooper(ps, n, "play", shouldPlay ? 1.0f : 0.0f);
            m_looperPlaying[n] = shouldPlay;
        }
    }
}

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
    m_liveEngaged = !m_liveEngaged;
    if (!m_liveEngaged) {
        ps.setByName("fx/pitchbend", 0.0f);
        ps.setByName("fx/pitchbend_engaged", 0.0f);
        for (int v = 0; v < kTransposeVoices; v++) {
            if (m_transposeVoiceNote[v] < 0) continue;
            m_transposeVoiceNote[v] = -1;
            char gateName[24];
            snprintf(gateName, sizeof gateName, "fx/xpose%d/gate", v);
            ps.setByName(gateName, 0.0f);
        }
    }
}
void ApcGrid::onStopImmediate(ParamStore& ps, LinkBridge* link) {
    for (int lp = 0; lp < kLooperCount; lp++) {
        if (m_looperRecording[lp]) {
            setLooper(ps, lp, "rec", 0.0f);
            m_looperRecording[lp] = false;
        }
        setLooper(ps, lp, "play", 0.0f);
        m_looperPlaying[lp] = false;
    }
    publishTransport(link);
}
void ApcGrid::onClearAll(bool held, ParamStore& ps, LinkBridge* link) {
    ps.setByName("cmd/clearall", held ? 1.0f : 0.0f);
    if (!held) return;
    for (int lp = 0; lp < kLooperCount; lp++) {
        m_looperHeld[lp] = false;
        m_looperErased[lp] = false;
        m_looperArmedOnPress[lp] = false;
        m_looperPlaying[lp] = false;
        m_looperHasContent[lp] = false;
        m_looperRecording[lp] = false;
        m_recordStartMs[lp] = 0;
        m_looperIsSidechainSource[lp] = false;
        setLooper(ps, lp, "sidechainsrc", 0.0f);
        setLooper(ps, lp, "play", 0.0f);
        setLooper(ps, lp, "rec", 0.0f);
        setLooper(ps, lp, "finishreq", 0.0f);
        m_looperFinishReqReleaseAt[lp] = 0;
    }
    for (int p = 0; p < kPresetCount; p++) {
        m_presetHeld[p] = false;
        m_presetCaptured[p] = false;
        m_presetUsed[p] = false;
        m_presetMask[p] = 0;
    }
    m_masterLenSamples = 0;
    ps.setByName("cmd/master_len", 0.0f);
    ps.setByName("cmd/recorded_bpm", 0.0f);
    for (int v = 0; v < kTransposeVoices; v++) {
        if (m_transposeVoiceNote[v] < 0) continue;
        m_transposeVoiceNote[v] = -1;
        char gateName[24];
        snprintf(gateName, sizeof gateName, "fx/xpose%d/gate", v);
        ps.setByName(gateName, 0.0f);
    }
    publishTransport(link);
}
int ApcGrid::allocateTransposeVoice(int note) {
    for (int v = 0; v < kTransposeVoices; v++)
        if (m_transposeVoiceNote[v] == note) return v;
    for (int v = 0; v < kTransposeVoices; v++) {
        if (m_transposeVoiceNote[v] >= 0) continue;
        m_transposeVoiceNote[v] = note;
        m_transposeVoiceOrder[v] = ++m_transposeVoiceCounter;
        return v;
    }
    int oldest = 0;
    for (int v = 1; v < kTransposeVoices; v++)
        if (m_transposeVoiceOrder[v] < m_transposeVoiceOrder[oldest]) oldest = v;
    m_transposeVoiceNote[oldest] = note;
    m_transposeVoiceOrder[oldest] = ++m_transposeVoiceCounter;
    return oldest;
}
void ApcGrid::releaseTransposeVoice(int note, ParamStore& ps) {
    for (int v = 0; v < kTransposeVoices; v++) {
        if (m_transposeVoiceNote[v] != note) continue;
        m_transposeVoiceNote[v] = -1;
        char gateName[24];
        snprintf(gateName, sizeof gateName, "fx/xpose%d/gate", v);
        ps.setByName(gateName, 0.0f);
        return;
    }
}
void ApcGrid::onKeybedNoteOn(int note, ParamStore& ps, Sampler* sampler) {
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
    m_liveEngaged = true;
    int v = allocateTransposeVoice(note);
    char noteName[24], gateName[24];
    snprintf(noteName, sizeof noteName, "fx/xpose%d/note", v);
    snprintf(gateName, sizeof gateName, "fx/xpose%d/gate", v);
    ps.setByName(noteName, (float)note);
    ps.setByName(gateName, 1.0f);
}
void ApcGrid::onKeybedNoteOff(int note, ParamStore& ps, Sampler* sampler) {
    if (m_drumRecordMode) {
        if (sampler) {
            int keyIdx = Sampler::keyIndex(note);
            if (keyIdx >= 0) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
        }
        return;
    }
    if (sampler) sampler->pushEvent(Sampler::EV_NOTE_OFF, note, 0);
    releaseTransposeVoice(note, ps);
}
void ApcGrid::onSamplerBtn65Press(Sampler* sampler) {
    if (sampler) sampler->pushEvent(Sampler::EV_REC_START, -1, 0);
}
void ApcGrid::onSamplerBtn65Release(Sampler* sampler) {
    if (sampler) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
}
void ApcGrid::onSamplerBtn66Press() {
    m_drumRecordMode = true;
}
void ApcGrid::onSamplerBtn66Release(Sampler* sampler) {
    m_drumRecordMode = false;
    if (sampler) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
}

void ApcGrid::onMicrorepeatOn(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    m_microRepeatDiv = div[note - 82];
    ps.setByName("fx/microrepeat_div", (float)m_microRepeatDiv);
}
void ApcGrid::onMicrorepeatOff(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    if (m_microRepeatDiv == div[note - 82]) {
        m_microRepeatDiv = 0;
        ps.setByName("fx/microrepeat_div", 0.0f);
    }
}

void ApcGrid::onShiftPress(ParamStore& ps) {
    m_shift = true;
    ps.setByName("fx/monitorfold", 1.0f);
}
void ApcGrid::onShiftRelease(ParamStore& ps) {
    m_shift = false;
    ps.setByName("fx/monitorfold", 0.0f);
}

void ApcGrid::onFormantCC(uint8_t data2, ParamStore& ps) {
    const bool inDeadzone = (data2 >= 60 && data2 <= 68);
    if (inDeadzone) { ps.setByName("fx/formant", 0.0f); return; }
    const float range = m_shift ? 3.0f : 1.0f;
    float v = (((float)(int)data2 - 64.0f) / 63.0f) * range;
    if (v > 3.0f) v = 3.0f; else if (v < -3.0f) v = -3.0f;
    ps.setByName("fx/formant", v);
}

static const int kFxKnobCcNumbers[kFxKnobCount] = { 48, 49, 50, 51, 54, 55, 57 };

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
// "Super music granulator" (LOFI feature, user-requested rework): the
// LofiFx bank keeps BITCRUSHAMT as its one surviving lofi audio-effect
// knob (explicit "keep the bitcrusher" decision) and hands every other
// knob position to the granulator, exposing all 6 of its meaningful
// runtime parameters directly instead of the previous 3 (grain size/
// density/scan rate only, with pitch-spray/position-jitter/reverse-
// probability all fixed at their passthrough defaults). VINYLAMT/
// FLUTTERAMT/SRRAMT are dropped from the bank's own control surface
// (guitar_lofi_fx.dsp's Faust zones for them still exist and default to
// their own passthrough values, so the DSP stages themselves are
// unaffected -- only the physical knob no longer reaches them).
static const FxKnobTarget kLofiFxTargets[kFxKnobCount] = {
    { FxKnobKind::Lv2Control, "fx2/BITCRUSHAMT" },
    { FxKnobKind::SamplerGrainSizeMs,       nullptr },
    { FxKnobKind::SamplerGrainDensityHz,    nullptr },
    { FxKnobKind::SamplerScanRate,          nullptr },
    { FxKnobKind::SamplerPitchSprayCents,   nullptr },
    { FxKnobKind::SamplerPositionJitterMs,  nullptr },
    { FxKnobKind::SamplerReverseProb,       nullptr },
};

static void applySamplerFxKnob(FxKnobKind kind, float v01, Sampler* sampler) {
    if (!sampler) return;
    switch (kind) {
        case FxKnobKind::SamplerAttackMs:       sampler->setAttackMs(v01 * 2000.0f); break;
        case FxKnobKind::SamplerReleaseMs:      sampler->setReleaseMs(v01 * 2000.0f); break;
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
        case FxKnobKind::SamplerPitchSprayCents:
            sampler->setGranulatorEnabled(true);
            sampler->setPitchSprayCents(v01 * 1200.0f);
            break;
        case FxKnobKind::SamplerPositionJitterMs:
            sampler->setGranulatorEnabled(true);
            sampler->setPositionJitterMs(v01 * 1000.0f);
            break;
        case FxKnobKind::SamplerReverseProb:
            sampler->setGranulatorEnabled(true);
            sampler->setReverseProbability(v01);
            break;
        default: break;
    }
}

static void applyFxKnobTarget(const FxKnobTarget& t, float v01, ParamStore& ps, Sampler* sampler, Lv2Host* homeFx) {
    float v = (t.name && strcmp(t.name, "fx/reverb") == 0) ? (v01 * 2.0f) : v01;
    switch (t.kind) {
        case FxKnobKind::FaustZone:  ps.setByName(t.name, v); break;
        case FxKnobKind::Lv2Control: if (homeFx) homeFx->setControl(t.name, v); break;
        default: applySamplerFxKnob(t.kind, v, sampler); break;
    }
}

void ApcGrid::onFxKnobCC(int ccNumber, uint8_t data2, ParamStore& ps, Sampler* sampler, Lv2Host* homeFx) {
    int knobIdx = -1;
    for (int k = 0; k < kFxKnobCount; k++) {
        if (kFxKnobCcNumbers[k] == ccNumber) { knobIdx = k; break; }
    }
    if (knobIdx < 0) return;
    float v = (float)data2 / 127.0f;
    m_fxBankValues[(int)m_activeBank][knobIdx] = v;
    const FxKnobTarget* targets =
        m_activeBank == FxBank::Dub ? kDubTargets :
        m_activeBank == FxBank::Guitar ? kGuitarTargets : kLofiFxTargets;
    applyFxKnobTarget(targets[knobIdx], v, ps, sampler, homeFx);
}

static unsigned nonZeroDeadline(unsigned now_ms, unsigned windowMs) {
    unsigned d = now_ms + windowMs;
    return d != 0 ? d : 1;
}

void ApcGrid::onDubFxPress(unsigned now_ms, ParamStore&) {
    m_activeBank = FxBank::Dub;
    m_bankFlashWhich = FxBank::Dub;
    m_bankFlashReleaseAt = nonZeroDeadline(now_ms, kBankFlashMs);
}
void ApcGrid::onLofiFxPress(unsigned now_ms, ParamStore& ps, Sampler* sampler) {
    bool shiftHeld = ps.get("fx/monitorfold", 0.0f) > 0.5f;
    if (shiftHeld && sampler) {
        sampler->setGranulatorEnabled(!sampler->granulatorEnabled());
        return;
    }
    m_activeBank = FxBank::LofiFx;
    m_bankFlashWhich = FxBank::LofiFx;
    m_bankFlashReleaseAt = nonZeroDeadline(now_ms, kBankFlashMs);
}
void ApcGrid::onGuitarFxPress(unsigned now_ms, ParamStore&) {
    m_activeBank = FxBank::Guitar;
    m_bankFlashWhich = FxBank::Guitar;
    m_bankFlashReleaseAt = nonZeroDeadline(now_ms, kBankFlashMs);
    m_guitarFxHeld = true;
    m_guitarFxConsumedByLooperPress = false;
}
void ApcGrid::onGuitarFxRelease(ParamStore&) {
    m_guitarFxHeld = false;
    m_guitarFxConsumedByLooperPress = false;
}

void ApcGrid::onSidechainLooperToggle(int looper, ParamStore& ps) {
    if (looper < 0 || looper >= kLooperCount) return;
    m_looperIsSidechainSource[looper] = !m_looperIsSidechainSource[looper];
    setLooper(ps, looper, "sidechainsrc", m_looperIsSidechainSource[looper] ? 1.0f : 0.0f);
    m_guitarFxConsumedByLooperPress = true;
}

}
