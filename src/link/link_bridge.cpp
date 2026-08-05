#include "link_bridge.h"

#include <atomic>
#include <cstddef>
#include <cstdio>

#if __has_include(<ableton/Link.hpp>)
#include <ableton/Link.hpp>
#define ALOOP_HAVE_LINK 1
#endif

namespace aloop {

namespace {
std::atomic<unsigned> g_active{0};
std::atomic<bool> g_weSetTempo{false};
}

void LinkBridge::start(double sampleRate, bool enabled) {
    (void)sampleRate;
    if (!enabled) { fprintf(stderr, "[link] disabled by config\n"); return; }
#ifdef ALOOP_HAVE_LINK
    auto* l = new ableton::Link(120.0);
    l->enable(true);
    l->enableStartStopSync(true);

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

void LinkBridge::controlTick() {
#ifdef ALOOP_HAVE_LINK
    if (!link_) return;
    auto* l = (ableton::Link*)link_;
    auto state = l->captureAppSessionState();
    const auto now = l->clock().micros();

    unsigned cur = g_active.load(std::memory_order_relaxed);
    unsigned nxt = cur ^ 1u;
    LinkSnapshot& s = buf_[nxt];
    s.bpm       = state.tempo();
    s.peerCount = (int)l->numPeers();
    s.synced    = (s.peerCount > 0);
    const double beat  = state.beatAtTime(now, kLinkQuantum);
    const double phase = state.phaseAtTime(now, kLinkQuantum);
    s.phaseValid          = true;
    s.beatPhaseMicroBeats = (int64_t)(phase * 1e6);
    s.quantumMicroBeats   = (int64_t)(kLinkQuantum * 1e6);
    s.isPlaying           = state.isPlaying();
    (void)beat;
    g_active.store(nxt, std::memory_order_release);
#endif
}

LinkSnapshot LinkBridge::audioRead() const {
    unsigned cur = g_active.load(std::memory_order_acquire);
    return buf_[cur];
}

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
    if (state.isPlaying() == playing) return;
    state.setIsPlaying(playing, l->clock().micros());
    l->commitAppSessionState(state);
    fprintf(stderr, "[link] set session transport %s\n", playing ? "PLAYING" : "STOPPED");
#else
    (void)playing;
#endif
}

}
