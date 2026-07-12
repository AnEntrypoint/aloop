// aloop RT audio thread — the Linux replacement for Circle's multicore audio
// dispatch. See audio_thread.h for the contract and MIGRATION-MAP for the
// bare-metal → Linux mapping.
//
// The per-block loop is the real-time-critical path: it does NO malloc, no locks,
// no syscalls except the intended ALSA read/write (the blocking point). Memory is
// pre-faulted (mlockall in main). The thread runs SCHED_FIFO, pinned to the
// home-FX core.

#include "audio_thread.h"

#include <pthread.h>
#include <sched.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>

// ALSA is present in the build container (see build-binary.yml). Guarded so the
// design compiles for review even where ALSA headers are absent; the real device
// build always has them.
#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#define ALOOP_HAVE_ALSA 1
#endif

namespace aloop {

namespace {
std::atomic<bool> g_running{false};
pthread_t g_worker;
AudioThread::Telemetry g_telem{};
AudioConfig g_cfg;

// Pin the CURRENT thread to `core` and set SCHED_FIFO at `prio`. This is the
// Linux equivalent of the bare-metal per-core assignment (MIGRATION-MAP).
bool setRealtimeSelf(int core, int prio) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        fprintf(stderr, "[audio] warning: could not pin to core %d\n", core);
    sched_param sp{};
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "[audio] warning: SCHED_FIFO prio %d failed (need rtprio limit)\n", prio);
        return false;
    }
    return true;
}
} // namespace

// The worker: open the PCM bridged to the f_uac2 gadget, then loop per block:
//   read capture -> DSP (loopMachine::update, wired via the ported source) ->
//   Lv2Host::runBlock (home + user effects, in-process) -> write playback.
// Here we implement the RT scaffolding + a clean passthrough; the DSP and host
// calls slot in where marked once the ported source + host impl are linked (their
// interfaces are already fixed, so this file does not change when they land).
static void* worker(void*) {
    setRealtimeSelf(g_cfg.homeFxCore, g_cfg.rtPriority);
    const int N = g_cfg.blockSize;
    const int ch = g_cfg.channels;

    std::vector<int16_t> buf((size_t)N * ch, 0);   // pre-allocated, pre-faulted

#ifdef ALOOP_HAVE_ALSA
    snd_pcm_t *cap = nullptr, *play = nullptr;
    // The f_uac2 gadget exposes an ALSA card; open capture (host→Pi) + playback.
    if (snd_pcm_open(&cap,  "default", SND_PCM_STREAM_CAPTURE,  0) < 0 ||
        snd_pcm_open(&play, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "[audio] ALSA open failed — is the f_uac2 gadget up?\n");
    } else {
        snd_pcm_set_params(cap,  SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           ch, g_cfg.sampleRate, 1, 20000);
        snd_pcm_set_params(play, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           ch, g_cfg.sampleRate, 1, 20000);
        while (g_running.load()) {
            snd_pcm_sframes_t r = snd_pcm_readi(cap, buf.data(), N);
            if (r < 0) { g_telem.xruns++; snd_pcm_recover(cap, (int)r, 1); continue; }

            // === DSP + effects go here (interfaces fixed; source links in) ===
            //   loopMachine.update(buf);          // ported loop engine + DSP
            //   host.runBlock(N);                 // home-FX (Core1) + user-FX (Core3)
            // For now the RT scaffolding runs a byte-exact passthrough so the
            // thread, timing, and PCM path are exercisable before the DSP links.

            snd_pcm_sframes_t w = snd_pcm_writei(play, buf.data(), N);
            if (w < 0) { g_telem.xruns++; snd_pcm_recover(play, (int)w, 1); }
        }
        snd_pcm_close(cap); snd_pcm_close(play);
    }
#else
    // No ALSA in this build (review build). The RT thread + timing still run.
    while (g_running.load()) { /* block loop no-op */ }
#endif
    return nullptr;
}

bool AudioThread::start(const AudioConfig& cfg) {
    cfg_ = cfg; g_cfg = cfg;
    g_running.store(true);
    if (pthread_create(&g_worker, nullptr, worker, nullptr) != 0) {
        fprintf(stderr, "[audio] fatal: could not create audio thread\n");
        return false;
    }
    return true;
}

void AudioThread::stop() {
    g_running.store(false);
    pthread_join(g_worker, nullptr);
}

AudioThread::Telemetry AudioThread::snapshotTelemetry() const { return g_telem; }
bool AudioThread::setRealtime(int core, int prio) { return setRealtimeSelf(core, prio); }
void AudioThread::workerLoop() {}   // (the free function `worker` is the body)

} // namespace aloop
