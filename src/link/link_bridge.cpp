// aloop Ableton Link bridge implementation. See link_bridge.h + ADR-005.
//
// The official ableton::Link runs on the control thread. Each control tick we
// capture the session state and publish it into a double-buffered snapshot the
// audio thread reads lock-free — the exact discipline looper used, so the audio
// callback code (loopMachine's phase/tempo apply) is unchanged.

#include "link_bridge.h"

#include <atomic>
#include <cstddef>
#include <cstdio>

// The Ableton Link library is header-only C++ (vendored under third_party/link,
// added as a submodule during the link-official-lib row). Guarded so the bridge
// compiles for review before the submodule lands; the device build has it.
#if __has_include(<ableton/Link.hpp>)
#include <ableton/Link.hpp>
#define ALOOP_HAVE_LINK 1
#endif

namespace aloop {

namespace {
std::atomic<unsigned> g_active{0};   // which snapshot buffer the audio thread reads

// Set true once WE have written a tempo into the session, so proposeTempo can
// tell "nobody has claimed the tempo yet" from "a peer already owns it".
std::atomic<bool> g_weSetTempo{false};
}

void LinkBridge::start(double sampleRate, bool enabled) {
    (void)sampleRate;
    if (!enabled) { fprintf(stderr, "[link] disabled by config\n"); return; }
#ifdef ALOOP_HAVE_LINK
    auto* l = new ableton::Link(120.0);   // start at 120 BPM until synced
    l->enable(true);
    // Share transport start/stop across peers, matching ../esp-idf-link's own
    // g_link->enableStartStopSync(true). enable() alone carries tempo and
    // beat/phase; without start-stop-sync the two projects agree on tempo but
    // NOT on transport state, so one can be rolling while the other is stopped.
    // Both projects must set this or the pairing is asymmetric -- see AGENTS.md
    // "aloop <-> esp-idf-link mesh: paired invariants".
    l->enableStartStopSync(true);

    // Link's own notification callbacks. These fire on LINK's thread, not ours:
    // they may only touch atomics / do bounded logging. No allocation, no locks,
    // and never a direct reach into the audio thread — the audio side keeps
    // reading the double-buffered snapshot published by controlTick().
    l->setNumPeersCallback([](std::size_t peers) {
        fprintf(stderr, "[link] peers now %u\n", (unsigned)peers);
    });
    l->setTempoCallback([](double bpm) {
        fprintf(stderr, "[link] session tempo now %.3f bpm\n", bpm);
    });
    l->setStartStopCallback([](bool playing) {
        fprintf(stderr, "[link] session transport %s\n", playing ? "PLAYING" : "STOPPED");
    });

    link_ = l;
    fprintf(stderr, "[link] Ableton Link enabled (official lib, UDP multicast, start-stop-sync on, quantum %.1f)\n",
            kLinkQuantum);
#else
    fprintf(stderr, "[link] built without the Link submodule — Link inactive\n");
#endif
}

void LinkBridge::stop() {
#ifdef ALOOP_HAVE_LINK
    if (link_) { delete (ableton::Link*)link_; link_ = nullptr; }
#endif
}

// CONTROL thread: capture Link's session state and publish into the inactive
// buffer, then flip. Single writer. Mirrors looper's republishTimeline →
// paramSnapshotPublish.
void LinkBridge::controlTick() {
#ifdef ALOOP_HAVE_LINK
    if (!link_) return;
    auto* l = (ableton::Link*)link_;
    auto state = l->captureAppSessionState();     // the non-audio-thread API
    const auto now = l->clock().micros();

    unsigned cur = g_active.load(std::memory_order_relaxed);
    unsigned nxt = cur ^ 1u;                        // write the inactive buffer
    LinkSnapshot& s = buf_[nxt];
    s.bpm       = state.tempo();
    s.peerCount = (int)l->numPeers();
    s.synced    = (s.peerCount > 0);
    // Beat/phase in the looper's fixed-point micro-beat units, against the
    // SHARED phrase length kLinkQuantum (see link_bridge.h — ../esp-idf-link
    // pins the same 16.0).
    const double beat  = state.beatAtTime(now, kLinkQuantum);
    const double phase = state.phaseAtTime(now, kLinkQuantum);
    s.phaseValid          = s.synced;
    s.beatPhaseMicroBeats = (int64_t)(phase * 1e6);
    s.quantumMicroBeats   = (int64_t)(kLinkQuantum * 1e6);
    // Transport, so enabling start-stop-sync actually means something on this
    // side rather than being advertised and discarded.
    s.isPlaying           = state.isPlaying();
    (void)beat;
    g_active.store(nxt, std::memory_order_release); // flip — audio now reads this
#endif
}

// AUDIO thread: read the active buffer. Lock-free, never blocks/tears.
LinkSnapshot LinkBridge::audioRead() const {
    unsigned cur = g_active.load(std::memory_order_acquire);
    return buf_[cur];
}

// TEMPO AUTHORITY. setTempo writes the tempo for the WHOLE session, so calling
// it on every first-loop FINISH would let two aloop units (or an aloop and a
// ../esp-idf-link box) repeatedly overwrite each other's tempo. Rule: we may
// claim the tempo when nobody else has (no peers yet, or we already own it);
// once a peer is present and WE never set it, that peer's tempo wins and our
// own recorded phrase adapts to it instead (audio_thread.cpp already derives
// linkSpeedRatio = recordedBpm / sessionBpm for exactly this case).
void LinkBridge::proposeTempo(double bpm) {
#ifdef ALOOP_HAVE_LINK
    if (!link_) return;
    auto* l = (ableton::Link*)link_;
    const bool havePeers = (l->numPeers() > 0);
    if (havePeers && !g_weSetTempo.load(std::memory_order_relaxed)) {
        fprintf(stderr, "[link] not proposing %.3f bpm — %u peer(s) already own the session tempo\n",
                bpm, (unsigned)l->numPeers());
        return;
    }
    auto state = l->captureAppSessionState();
    state.setTempo(bpm, l->clock().micros());
    l->commitAppSessionState(state);
    g_weSetTempo.store(true, std::memory_order_relaxed);
    fprintf(stderr, "[link] proposed session tempo %.3f bpm\n", bpm);
#else
    (void)bpm;
#endif
}

void LinkBridge::setTransportPlaying(bool playing) {
#ifdef ALOOP_HAVE_LINK
    if (!link_) return;
    auto* l = (ableton::Link*)link_;
    auto state = l->captureAppSessionState();
    if (state.isPlaying() == playing) return;      // no-op, don't spam the session
    state.setIsPlaying(playing, l->clock().micros());
    l->commitAppSessionState(state);
    fprintf(stderr, "[link] set session transport %s\n", playing ? "PLAYING" : "STOPPED");
#else
    (void)playing;
#endif
}

} // namespace aloop
