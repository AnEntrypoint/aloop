// aloop Ableton Link bridge — the official lib on the control core, feeding the
// audio thread through the same lock-free snapshot the looper used (ADR-005).
//
// WHY this shape: Ableton Link's own API is RT-safe by exactly this pattern — a
// network thread maintains the session, and the audio thread reads a captured
// session state. We map Link's SessionState onto looper's LiveParams snapshot so
// the audio callback code (loopMachine's phase/tempo apply) is UNCHANGED from
// bare metal — it still just reads the snapshot. WiFi jitter therefore affects
// Link SYNC accuracy, never the audio deadline (feasibility R1).

#ifndef ALOOP_LINK_BRIDGE_H
#define ALOOP_LINK_BRIDGE_H

#include <cstdint>

namespace aloop {

// The shared Link phrase length, in beats. 16 beats = 4 bars at 4/4, the user's
// standing requirement that every looper length be a multiple/division of.
// ../esp-idf-link pins the SAME value as `#define LINK_QUANTUM 16.0` (main.h);
// the two MUST agree or the projects disagree on phase — see AGENTS.md
// "aloop <-> esp-idf-link mesh: paired invariants".
constexpr double kLinkQuantum = 16.0;

// The subset of Link state the loop engine needs (mirrors looper LiveParams).
struct LinkSnapshot {
    double  bpm          = 120.0;
    bool    synced       = false;   // are we in a Link session with a valid phase?
    bool    phaseValid   = false;
    int64_t beatPhaseMicroBeats = 0;   // sub-beat phase, fixed-point (looper units)
    int64_t quantumMicroBeats   = 0;
    // Transport, shared across peers because start/stop sync is enabled. Reading
    // this is what makes that capability real rather than advertised-and-ignored.
    bool    isPlaying    = false;
    int     peerCount    = 0;       // raw count, not just synced>0 (mesh testing)
};

class LinkBridge {
public:
    // Start Link on the CONTROL core (never the audio core). enabled from config.
    void start(double sampleRate, bool enabled);
    void stop();

    // Called from the CONTROL thread each tick: capture Link's current session
    // state and publish it into the double-buffered atomic snapshot. Single
    // writer. (Uses Link's captureAppSessionState — the non-audio-thread API.)
    void controlTick();

    // Called from the AUDIO thread: read the latest published snapshot. Lock-free,
    // never blocks, never tears (double-buffer flip). This is the ONLY thing the
    // audio callback touches — exactly as looper's paramSnapshotLoad().
    LinkSnapshot audioRead() const;

    // Propose our internal tempo to the session (when a loop defines the phrase),
    // mirroring looper's tempo-set burst. Called from control, not audio.
    //
    // TEMPO AUTHORITY: this writes the tempo for EVERY peer in the session, so it
    // is deliberately not called unconditionally. See the .cpp — a propose is
    // skipped when peers are already present and have a tempo we did not set,
    // so two devices cannot fight over the session tempo.
    void proposeTempo(double bpm);

    // Share OUR transport state with the session (start-stop-sync is enabled, and
    // a peer that only ever consumes transport is half-wired). Control thread.
    void setTransportPlaying(bool playing);

private:
    // The official ableton::Link instance lives here (control-thread owned).
    void* link_ = nullptr;         // ableton::Link* (opaque to keep the header lib-free)
    // Double-buffered snapshot: writer flips index; reader reads active. The
    // exact discipline from looper/patches/paramSnapshot.h.
    LinkSnapshot buf_[2];
    unsigned active_ = 0;          // atomic index in the .cpp
};

} // namespace aloop
#endif // ALOOP_LINK_BRIDGE_H
