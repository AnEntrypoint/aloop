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

// The Faust home stack (loop engine + effects) generated from dsp/loop.dsp (and
// composable with dsp/aloop.dsp) is compiled to loop.cpp by the CMake custom
// command and included here. Guarded so this file compiles before the generated
// file exists (review builds); the device build always generates it first.
#if __has_include("loop.cpp")
#define FAUSTFLOAT float
struct FaustMeta { void declare(const char*, const char*) {} };
struct FaustUI {
    void openTabBox(const char*){} void openHorizontalBox(const char*){}
    void openVerticalBox(const char*){} void closeBox(){}
    void addButton(const char*, float*){} void addCheckButton(const char*, float*){}
    void addVerticalSlider(const char*, float*, float, float, float, float){}
    void addHorizontalSlider(const char*, float*, float, float, float, float){}
    void addNumEntry(const char*, float*, float, float, float, float){}
    void addHorizontalBargraph(const char*, float*, float, float){}
    void addVerticalBargraph(const char*, float*, float, float){}
    void addSoundfile(const char*, const char*, void**){}
    void declare(float*, const char*, const char*){}
};
#define Meta FaustMeta
#define UI FaustUI
#define dsp FaustDspBase
struct FaustDspBase { virtual ~FaustDspBase(){} };
#include "loop.cpp"     // defines class AloopLoopDsp : public FaustDspBase
#undef dsp
#define ALOOP_HAVE_FAUST_LOOP 1
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
    std::vector<float> fin((size_t)N, 0.0f), fout((size_t)N, 0.0f);

    // Instantiate the Faust home stack (loop engine + effects). Its compute()
    // runs the whole home DSP per block — the loop record/play + the effects.
#ifdef ALOOP_HAVE_FAUST_LOOP
    AloopLoopDsp faustHome;
    faustHome.init((int)g_cfg.sampleRate);
    FaustUI fui; faustHome.buildUserInterface(&fui);
    float* fins[1]  = { fin.data() };
    float* fouts[1] = { fout.data() };
#endif

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

            // === run the Faust home stack (loop engine + effects) this block ===
            // s16 -> float, compute(), float -> s16. The Faust program does the
            // record/play loop + the effects in one pass. The user-FX LV2 (Core 3)
            // runs after via the in-process host (host.runBlock), joined this block.
#ifdef ALOOP_HAVE_FAUST_LOOP
            for (int i = 0; i < N; i++) fin[i] = buf[i] / 32768.0f;
            faustHome.compute(N, fins, fouts);
            for (int i = 0; i < N; i++) {
                float v = fout[i] * 32768.0f;
                buf[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
#endif
            //   host.runBlock(N);   // user-FX LV2 on the free core (in-process, no graph)

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
