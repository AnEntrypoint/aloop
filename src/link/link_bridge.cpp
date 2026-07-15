// aloop Ableton Link bridge implementation. See link_bridge.h + ADR-005.
//
// The official ableton::Link runs on the control thread. Each control tick we
// capture the session state and publish it into a double-buffered snapshot the
// audio thread reads lock-free — the exact discipline looper used, so the audio
// callback code (loopMachine's phase/tempo apply) is unchanged.

#include "link_bridge.h"

#include <atomic>
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
}

void LinkBridge::start(double sampleRate, bool enabled) {
    (void)sampleRate;
    if (!enabled) { fprintf(stderr, "[link] disabled by config\n"); return; }
#ifdef ALOOP_HAVE_LINK
    auto* l = new ableton::Link(120.0);   // start at 120 BPM until synced
    l->enable(true);
    link_ = l;
    fprintf(stderr, "[link] Ableton Link enabled (official lib, UDP multicast)\n");
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
    s.bpm    = state.tempo();
    s.synced = (l->numPeers() > 0);
    // Beat/phase in the looper's fixed-point micro-beat units.
    // Quantum is 16.0 beats (4 bars at 4/4), matching the user's explicit
    // standing requirement: "all loopers must absolutely and permanently
    // stick and track to ableton links phrasing... the ableton link phrase
    // must be as close to 4 bars 120 as we can get it." This is the FIXED
    // base every looper's own length is a multiple/division of (see
    // apc_grid.cpp's deriveTempoQuant, which picks tempo + a beat-count
    // candidate relative to this same base) -- previously 4.0 (one bar),
    // which under-scoped the phrase to 1/4 of the user's intended grid.
    const double quantum = 16.0;
    const double beat  = state.beatAtTime(now, quantum);
    const double phase = state.phaseAtTime(now, quantum);
    s.phaseValid          = s.synced;
    s.beatPhaseMicroBeats = (int64_t)(phase * 1e6);
    s.quantumMicroBeats   = (int64_t)(quantum * 1e6);
    (void)beat;
    g_active.store(nxt, std::memory_order_release); // flip — audio now reads this
#endif
}

// AUDIO thread: read the active buffer. Lock-free, never blocks/tears.
LinkSnapshot LinkBridge::audioRead() const {
    unsigned cur = g_active.load(std::memory_order_acquire);
    return buf_[cur];
}

void LinkBridge::proposeTempo(double bpm) {
#ifdef ALOOP_HAVE_LINK
    if (!link_) return;
    auto* l = (ableton::Link*)link_;
    auto state = l->captureAppSessionState();
    state.setTempo(bpm, l->clock().micros());
    l->commitAppSessionState(state);
#else
    (void)bpm;
#endif
}

} // namespace aloop
