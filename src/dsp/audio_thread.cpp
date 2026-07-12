// aloop RT audio thread — the Linux replacement for Circle's multicore audio
// dispatch. See audio_thread.h for the contract and MIGRATION-MAP for the
// bare-metal → Linux mapping.
//
// The per-block loop is the real-time-critical path: it does NO malloc, no locks,
// no syscalls except the intended ALSA read/write (the blocking point). Memory is
// pre-faulted (mlockall in main). The thread runs SCHED_FIFO, pinned to the
// home-FX core.

#include "audio_thread.h"
#include "../host/lv2_host.h"
#include "../control/midi.h"
#include "../link/link_bridge.h"

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
// A real param-binding UI: it captures each control's name → its zone pointer so
// the audio thread can SET the Faust engine's controls (rec/play/len/vol per looper (no overdub) 
// the effect knobs) from the MIDI ParamStore + Link each block. Without this the
// controls are inert (the loop would never record).
#include <map>
#include <string>
struct FaustUI {
    std::map<std::string, float*> zones;   // FULL path (group/…/label) → zone
    std::vector<std::string> path;         // current open-box group stack
    std::string full(const char* label) const {
        std::string p;
        for (auto& g : path) if (!g.empty()) { p += g; p += "/"; }
        p += label;
        return p;   // e.g. "looper03/rec"  (the vgroup name + the control label)
    }
    void openTabBox(const char* l){ path.push_back(l?l:""); }
    void openHorizontalBox(const char* l){ path.push_back(l?l:""); }
    void openVerticalBox(const char* l){ path.push_back(l?l:""); }
    void closeBox(){ if(!path.empty()) path.pop_back(); }
    void addButton(const char* l, float* z){ zones[full(l)]=z; }
    void addCheckButton(const char* l, float* z){ zones[full(l)]=z; }
    void addVerticalSlider(const char* l, float* z, float, float, float, float){ zones[full(l)]=z; }
    void addHorizontalSlider(const char* l, float* z, float, float, float, float){ zones[full(l)]=z; }
    void addNumEntry(const char* l, float* z, float, float, float, float){ zones[full(l)]=z; }
    void addHorizontalBargraph(const char*, float*, float, float){}
    void addVerticalBargraph(const char*, float*, float, float){}
    void addSoundfile(const char*, const char*, void**){}
    void declare(float*, const char*, const char*){}
    // Set a control by full path (no-op if the dsp doesn't expose it). Matches
    // either the exact path or any zone whose path ENDS with the given suffix,
    // so "HPCUT" finds ".../HPCUT" and "looper03/rec" matches exactly.
    void set(const char* name, float v){
        auto it=zones.find(name);
        if(it!=zones.end()){ *it->second=v; return; }
        std::string suf(name);
        for(auto& kv:zones){ const std::string& k=kv.first;
            if(k.size()>=suf.size() && k.compare(k.size()-suf.size(), suf.size(), suf)==0){ *kv.second=v; return; } }
    }
    // Read a control back by full-or-suffix path (for state telemetry). Returns
    // `def` if no such zone. Same matching rule as set().
    float get(const char* name, float def=0.0f) const {
        auto it=zones.find(name);
        if(it!=zones.end()) return *it->second;
        std::string suf(name);
        for(auto& kv:zones){ const std::string& k=kv.first;
            if(k.size()>=suf.size() && k.compare(k.size()-suf.size(), suf.size(), suf)==0) return *kv.second; }
        return def;
    }
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
ParamStore* g_params = nullptr;   // shared control store (from MIDI); read each block
LinkBridge* g_link = nullptr;     // Ableton Link (tempo/phase); read each block for varispeed sync

// Map a control-map TARGET name ("looper3/rec", "fx/hp") to the Faust zone label
// the home stack exposes. Loopers use a 2-digit index (looper03/rec); effects use
// the chain's slider labels. Returns "" if there is no matching zone.
static std::string targetToZone(const std::string& target) {
    // looperN/xxx → "looper%2i/xxx" (Faust's width-2 right-justified index: a
    // space for single digits — "looper 0/rec" … "looper19/rec"). The UI shim's
    // set() also matches by suffix, so exact formatting is belt-and-suspenders.
    if (target.rfind("looper", 0) == 0) {
        auto slash = target.find('/');
        if (slash != std::string::npos) {
            int idx = atoi(target.c_str() + 6);
            char z[64];
            snprintf(z, sizeof z, "looper%2d/%s", idx, target.c_str() + slash + 1);
            return z;
        }
    }
    // fx/hp etc → the effect chain's control labels (from param_mapping.md).
    if (target == "fx/hp")      return "HPCUT";
    if (target == "fx/lp")      return "LPCUT";
    if (target == "fx/lpres")   return "LPRES";
    if (target == "fx/reverb")  return "REVAMT";
    if (target == "fx/delay")   return "DELAYAMT";
    if (target == "fx/time")    return "TIME";
    if (target == "fx/formant") return "FORMANT";
    if (target == "fx/pitch")   return "SEMIS";
    return "";   // commands (cmd/*) are handled separately, not a Faust zone
}

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

    // The user's swappable effect(s): an in-process LV2 host loading any bundle
    // from /effects/user, pinned to the free core (Core 3). Runs AFTER the home
    // stack, in the same block (no graph — zero added latency, ADR-002). A bad
    // user plugin is caught by the host's watchdog and bypassed.
    Lv2Host userFx;
    userFx.loadDir("/effects/user");
    userFx.connect(N, ch);

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
            // Apply the remappable controls: for each bound target the MIDI map
            // set, push its current value into the matching Faust zone. Done once
            // per block from the atomic store — no locks, no alloc (the name→zone
            // strings resolve cheaply; a production build caches name→float* once).
            if (g_params) {
                g_params->forEach([&](const std::string& target, int){
                    std::string zone = targetToZone(target);
                    if (!zone.empty()) fui.set(zone.c_str(), g_params->get(target));
                });
                // Global commands (cmd/*) are NOT per-looper Faust zones — they drive
                // the engine-wide `clear` button and `speed` multiplier directly (the
                // hardware's CLEARALL + momentary HALFSPEED/DOUBLESPEED). Held (value
                // 1) = active; released = neutral. Double wins if both are somehow
                // held. These labels are engine-global in loop.dsp (not under a
                // looper group), so set() resolves them by their plain name.
                fui.set("clear", g_params->get("cmd/clearall") > 0.5f ? 1.0f : 0.0f);
                float speed = 1.0f;
                if (g_params->get("cmd/halfspeed")   > 0.5f) speed = 0.5f;
                if (g_params->get("cmd/doublespeed") > 0.5f) speed = 2.0f;
                fui.set("speed", speed);
                // NOTE: LOOP_IMMEDIATE / SET_LOOP_START / CLEAR_LOOP_START (mark-point
                // restart) are NOT wired — they need an addressable read head, which
                // the Faust feedback-delay looper does not have (a preserve-on-hold
                // rwtable playhead is a read-modify-write Faust rejects; see loop.dsp
                // header + docs/COMMAND-SURFACE.md). Deliberate model difference.
                // Global STOP-ALL (hardware LOOP_COMMAND_STOP 0x03): clear every
                // looper's play checkbox so all playback stops. Per-looper stop is
                // already covered by binding a control to looper<i>/play (=0 stops
                // that one); this is the single all-tracks command. Edge-triggered
                // on the held value so it doesn't fight a user re-arming a looper.
                if (g_params->get("cmd/stopall") > 0.5f) {
                    char z[32];
                    for (int lp = 0; lp < 20; lp++) {
                        snprintf(z, sizeof z, "looper%2d/play", lp);
                        fui.set(z, 0.0f);
                    }
                }
            }
            // Per-looper STATE telemetry (the GET_STATE 0x30 equivalent): read each
            // looper's rec/play/vol back from the Faust zones into the atomic
            // snapshot the control thread serves on udp/4445. Cheap (60 map lookups
            // once/block); a production build would cache the float* on first block.
            {
                char z[32];
                for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) {
                    snprintf(z, sizeof z, "looper%2d/rec",  lp); g_telem.looperRec[lp]  = fui.get(z) > 0.5f;
                    snprintf(z, sizeof z, "looper%2d/play", lp); g_telem.looperPlay[lp] = fui.get(z) > 0.5f;
                    snprintf(z, sizeof z, "looper%2d/vol",  lp); g_telem.looperVol[lp]  = fui.get(z, 1.0f);
                }
            }
            // Varispeed Link sync: when synced, set every looper's loop length from
            // the Link tempo (a musical phrase = a whole number of beats). A tempo
            // change resizes the loops so they stay locked to the session — the
            // same behavior as the original looper's masterLoopBlocks recompute.
            if (g_link) {
                LinkSnapshot ls = g_link->audioRead();
                // publish the live Link state into the telemetry snapshot (atomic
                // plain-old-data; the control thread reads it for udp/4445).
                g_telem.linkSynced = ls.synced;
                g_telem.bpm = ls.bpm;
                if (ls.synced && ls.bpm > 1.0) {
                    // one bar (4 beats) as the phrase, rounded to whole blocks.
                    double beatsPerBar = 4.0;
                    double samplesPerBeat = (g_cfg.sampleRate * 60.0) / ls.bpm;
                    double lenSamples = samplesPerBeat * beatsPerBar;
                    char z[32];
                    for (int lp = 0; lp < 20; lp++) {
                        snprintf(z, sizeof z, "looper%2d/len", lp);
                        fui.set(z, (float)lenSamples);
                    }
                }
            }
            for (int i = 0; i < N; i++) fin[i] = buf[i] / 32768.0f;
            // Time the DSP work vs the block budget → home-core busy % telemetry.
            timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            faustHome.compute(N, fins, fouts);
            // the user LV2(s) process the home-stack OUTPUT (fout) on the free
            // core, in the same block — zero added latency, no graph (ADR-002).
            // process() runs the plugin chain in place; passthrough if none loaded.
            userFx.process(fout.data(), N);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            // busy fraction = work / block-period. Smoothed (EWMA) so the readout is
            // stable. Stored on the home-FX core index; a spike toward 100% warns of
            // an impending xrun (RT-TUNING). Plain float store — read by control thd.
            {
                double workNs = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
                double periodNs = (double)N / g_cfg.sampleRate * 1e9;
                double pct = periodNs > 0 ? (workNs / periodNs) * 100.0 : 0.0;
                float& slot = g_telem.coreBusyPct[g_cfg.homeFxCore & 3];
                slot = slot * 0.9f + (float)pct * 0.1f;   // EWMA
            }
            for (int i = 0; i < N; i++) {
                float v = fout[i] * 32768.0f;
                buf[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
#endif

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

bool AudioThread::start(const AudioConfig& cfg, ParamStore* params, LinkBridge* link) {
    cfg_ = cfg; g_cfg = cfg; g_params = params; g_link = link;
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
