// aloop audio thread — the Linux replacement for the Circle multicore audio
// dispatch (MIGRATION-MAP: CMultiCoreSupport SEV/WFE -> pthreads + affinity +
// SCHED_FIFO; futex/eventfd wakeups). The lock-free discipline is unchanged from
// looper; only the OS primitives differ.

#ifndef ALOOP_AUDIO_THREAD_H
#define ALOOP_AUDIO_THREAD_H

#include <cstdint>

namespace aloop {

struct ParamStore;   // control values from MIDI (control/midi.h)
class  LinkBridge;   // Ableton Link tempo/phase (link/link_bridge.h)

struct AudioConfig {
    int sampleRate = 48000;
    int blockSize  = 64;      // 1.333 ms budget — do not raise
    int channels   = 1;       // mono
    int homeFxCore = 1;       // pinned cores (match isolcpus in kernel/cmdline.txt)
    int userFxCore = 3;
    int rtPriority = 95;      // SCHED_FIFO
};

// Starts the RT audio pipeline:
//   - mlockall(MCL_CURRENT|MCL_FUTURE)  — no page faults in the audio path
//   - opens the ALSA PCM bridged to the f_uac2 gadget
//   - spawns the audio worker thread(s): SCHED_FIFO at rtPriority, pinned to the
//     home-FX / user-FX cores via pthread_setaffinity_np
//   - the worker loop per block: read PCM -> loopMachine::update (DSP) ->
//     Lv2Host::runBlock (home + user effects, in-process) -> write PCM
//   - reads the control/Link snapshot at the top of each block (lock-free)
// RT-safe: the per-block path does NO malloc/lock/syscall beyond the ALSA
// read/write (which is the intended blocking point).
class AudioThread {
public:
    // Start the RT audio pipeline. `params` = the shared control store the MIDI
    // thread writes (the audio thread reads it and sets the Faust control zones).
    // `link` = the Ableton Link bridge the audio thread reads each block for
    // varispeed loop-length sync. Both may be null (controls/Link inactive).
    bool start(const AudioConfig& cfg, struct ParamStore* params = nullptr,
               class LinkBridge* link = nullptr);
    void stop();

    // Telemetry the control thread reads (never written from audio hot path
    // except via atomics): per-core busy %, xrun count, current Link sync state.
    struct Telemetry {
        float    coreBusyPct[4] = {0,0,0,0};
        uint64_t xruns = 0;
        bool     linkSynced = false;
        double   bpm = 0.0;
        bool     apMode = false;    // hosting AP vs joined STA
    };
    Telemetry snapshotTelemetry() const;

private:
    void workerLoop();              // the per-block RT loop
    bool setRealtime(int core, int prio);   // affinity + SCHED_FIFO for this thread
    AudioConfig cfg_;
};

} // namespace aloop
#endif // ALOOP_AUDIO_THREAD_H
