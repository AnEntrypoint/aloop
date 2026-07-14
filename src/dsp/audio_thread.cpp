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
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <vector>
#include <memory>

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
    if (target == "fx/monitorfold") return "MONITORFOLD";
    return "";   // commands (cmd/*) and fx/pitchbend*, fx/microrepeat_div are handled separately, not a plain 1:1 Faust zone
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
    const int ch = g_cfg.channels;   // DSP channel count (mono = 1), the Faust I/O.

    // The f_uac2 USB gadget presents a STEREO wire (c_chmask/p_chmask = 0x3), the
    // same as the looper's UAC2 (stereo wire, mono internally). So the ALSA PCM is
    // opened with `wireCh` channels and we deinterleave capture -> mono for Faust,
    // then duplicate mono -> both wire channels on playback. If the device is truly
    // mono (channels config == the wire), wireCh collapses to ch and this is a
    // straight copy. See ADR-008 / f_uac2-gadget.sh.
    const int wireCh = (ch < 2) ? 2 : ch;   // USB wire is stereo; DSP is mono

    // Instrument device sample buffer is int32_t, NOT int16_t. WITNESSED live:
    // the M-Audio AIR 192|4 (and most class-compliant USB audio interfaces)
    // only supports S32_LE (24-bit data left-justified in a 32-bit word,
    // confirmed via /proc/asound/card0/stream0: "Format: S32_LE, Bits: 24") —
    // there is no S16_LE fallback on this hardware. The prior code hardcoded
    // SND_PCM_FORMAT_S16_LE and an int16_t buffer without checking whether the
    // format request actually succeeded; ALSA silently negotiated S32_LE
    // anyway (hw_params still succeeded — only the specific format request was
    // ignored) while the code kept treating the wire as 16-bit, writing
    // half-width garbage into what the hardware read as 32-bit words — this
    // is what produced loud static once the instrument device was finally
    // opened at all (ADR-015). The OTG gadget, by contrast, genuinely IS
    // S16_LE (src/usb/f_uac2-gadget.sh sets c_ssize/p_ssize=2 ourselves), so
    // the two devices need separate wire buffers in their own native formats.
    std::vector<int32_t> buf((size_t)N * wireCh, 0);       // instrument device (S32_LE)
    std::vector<int16_t> otgBuf((size_t)N * wireCh, 0);    // OTG gadget mirror (S16_LE)
    std::vector<float> fin((size_t)N, 0.0f), fout((size_t)N, 0.0f);  // mono DSP buffers

    // Instantiate the Faust home stack (loop engine + effects). Its compute()
    // runs the whole home DSP per block — the loop record/play + the effects.
    //
    // WITNESSED live on a real Pi 4 (gdb + a real core dump, built with -g -O0
    // via a temporary debug CI job): AloopLoopDsp is 336,326,896 bytes (~320
    // MiB) — the 20 loopers' MAXLEN=48000*60 (60s) delay-line buffers add up
    // fast. As a STACK-LOCAL variable inside this thread's entry function, no
    // pthread stack size (musl's small default, or an explicit 8 MiB — both
    // tried and both crashed identically) could ever be large enough; the
    // SIGSEGV at setRealtimeSelf's very first local-variable stack write was
    // this frame simply being un-mapped from the moment the thread's stack
    // pointer moved past its real (small) allocation to make room for a
    // 320 MiB local later in the same function. Heap-allocate it instead —
    // this is a one-time allocation at thread startup, never in the per-block
    // RT hot path, so it carries none of the "no allocation in the audio
    // callback" real-time risk the rest of this file is written to avoid.
#ifdef ALOOP_HAVE_FAUST_LOOP
    auto faustHomePtr = std::make_unique<AloopLoopDsp>();
    AloopLoopDsp& faustHome = *faustHomePtr;
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
    userFx.loadDir(g_cfg.userDir, g_cfg.userFxCore);   // honor aloop.conf [effects] user_dir + core
    userFx.connect(N, ch);

#ifdef ALOOP_HAVE_ALSA
    snd_pcm_t *cap = nullptr, *play = nullptr;
    // TWO distinct devices, matching looper's split exactly (ADR-015):
    //   - instrumentDevice (default hw:0,0, e.g. the M-Audio AIR 192|4): the
    //     REAL tight-latency capture+playback path a musician plugs an
    //     instrument/mic into and actually hears. `cap`/`play` below are
    //     ALWAYS this device — never the OTG gadget.
    //   - audioDevice (the f_uac2 OTG gadget, hw:UAC2Gadget,0): a best-effort
    //     MIRROR of the same processed output, opened separately below
    //     (`otgPlay`) and written non-blocking so an absent/slow/non-streaming
    //     OTG host can never stall or desync the instrument device's
    //     real-time path (looper: AudioOutputUSB is the graph's real output;
    //     AudioOutputOTG is a passive tap on the same ring with its own,
    //     looser-latency read cursor).
    const int kAlsaOpenRetries = 30;
    const char* wireDev = g_cfg.instrumentDevice.c_str();
    for (int attempt = 0; attempt < kAlsaOpenRetries; attempt++) {
        if (snd_pcm_open(&cap,  wireDev, SND_PCM_STREAM_CAPTURE,  0) == 0 &&
            snd_pcm_open(&play, wireDev, SND_PCM_STREAM_PLAYBACK, 0) == 0) break;
        if (cap)  { snd_pcm_close(cap);  cap  = nullptr; }
        if (play) { snd_pcm_close(play); play = nullptr; }
        if (attempt == 0) fprintf(stderr, "[audio] ALSA open of %s failed — is the instrument USB audio interface plugged in? retrying...\n", wireDev);
        struct timespec ts{1, 0}; nanosleep(&ts, nullptr);
    }
    if (!cap || !play) {
        fprintf(stderr, "[audio] ALSA still unavailable after %ds — is the instrument USB audio interface plugged in? audio stays down until restart\n", kAlsaOpenRetries);
    } else {
        // Explicit hw_params targeting the REAL block_size (N frames/period), not
        // ALSA's snd_pcm_set_params() convenience call — that call picks whatever
        // period/buffer satisfies a requested LATENCY (previously 20ms), which both
        // ignores block_size entirely and, combined with opening "default" (routed
        // through the dmix/dsnoop plugin's own large fixed period), was the actual
        // cause of the "massive latency vs looper" symptom: the wire path was
        // running at ~20ms+ per direction instead of the intended N/sampleRate
        // (1.33ms at the default 64-sample block).
        //
        // WITNESSED live on a real Pi 4: an initial 2-period buffer (256 frames
        // total, the ALSA minimum) produced 690 xruns within seconds — too tight
        // for a USB gadget PCM, where each read/write also rides USB's own
        // transfer-scheduling jitter on top of this thread's SCHED_FIFO jitter
        // (unlike looper's bare-metal Circle build, which has no OS/USB-stack
        // contention at all). 4 periods (256 total *frames of headroom* — i.e.
        // 4*N frames of buffer against the same N-frame period/wakeup size) keeps
        // the same per-period 1.33ms granularity (the actual latency-determining
        // number) while giving the ring enough slack to absorb that jitter
        // without underrunning constantly.
        auto configurePcm = [&](snd_pcm_t* pcm) -> bool {
            snd_pcm_hw_params_t* hw;
            snd_pcm_hw_params_alloca(&hw);
            snd_pcm_hw_params_any(pcm, hw);
            snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
            // The instrument device is S32_LE (24-bit data left-justified in a
            // 32-bit word — most class-compliant USB audio interfaces, including
            // the M-Audio AIR 192|4, have no S16_LE mode at all). WITNESSED live:
            // requesting S16_LE here previously succeeded at the snd_pcm_hw_params()
            // call (no error returned) while the device silently negotiated S32_LE
            // anyway — the return value of set_format() itself was never checked,
            // so the mismatch went undetected until it produced loud static on
            // real hardware. Now explicit, and the buffer type (int32_t `buf`,
            // declared above) matches.
            if (snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE) < 0)
                fprintf(stderr, "[audio] warning: instrument device rejected S32_LE format request\n");
            snd_pcm_hw_params_set_channels(pcm, hw, wireCh);
            unsigned int rate = (unsigned int)g_cfg.sampleRate;
            snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
            snd_pcm_uframes_t period = (snd_pcm_uframes_t)N;
            snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
            snd_pcm_uframes_t bufSize = period * 4;
            snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufSize);
            if (snd_pcm_hw_params(pcm, hw) < 0) return false;
            // Verify the format actually negotiated matches what buf's element
            // type assumes — a silent mismatch here is exactly the loud-static bug.
            snd_pcm_format_t negotiatedFmt;
            if (snd_pcm_hw_params_get_format(hw, &negotiatedFmt) == 0 && negotiatedFmt != SND_PCM_FORMAT_S32_LE)
                fprintf(stderr, "[audio] warning: instrument device negotiated format %s, not S32_LE — audio will be corrupted (buf is int32_t)\n",
                        snd_pcm_format_name(negotiatedFmt));
            if (period != (snd_pcm_uframes_t)N)
                fprintf(stderr, "[audio] warning: device would not grant period=%d frames, got %lu — latency will not match block_size\n",
                        N, (unsigned long)period);
            // sw_params: the hw_params default start_threshold for a PLAYBACK
            // stream is the full buffer_size — WITNESSED live on a real Pi 4:
            // /proc/asound/.../pcm0p/sub0/status stayed stuck in "PREPARED"
            // forever (never auto-started), because this loop only ever writes
            // one N-frame period per snd_pcm_writei() call and immediately
            // blocks on the next capture read, so the ring never reached a full
            // buffer_size of queued frames to cross that default threshold —
            // meanwhile CAPTURE (which starts as soon as ANY data is available,
            // not gated on a full buffer) ran fine, so the two streams silently
            // desynced and playback undor-ran on every single write. Lowering
            // start_threshold to exactly one period means playback triggers on
            // the very first snd_pcm_writei(), matching how capture already
            // behaves.
            snd_pcm_sw_params_t* sw;
            snd_pcm_sw_params_alloca(&sw);
            snd_pcm_sw_params_current(pcm, sw);
            snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
            snd_pcm_sw_params_set_avail_min(pcm, sw, period);
            if (snd_pcm_sw_params(pcm, sw) < 0)
                fprintf(stderr, "[audio] warning: sw_params (start_threshold) rejected — playback may not auto-start\n");
            return true;
        };
        if (!configurePcm(cap) || !configurePcm(play))
            fprintf(stderr, "[audio] warning: explicit hw_params rejected by %s — falling back to driver defaults (higher latency)\n", wireDev);
        snd_pcm_prepare(cap);
        snd_pcm_prepare(play);

        // OTG gadget mirror output: opened NONBLOCK so a missing/non-streaming
        // host on the other end of the OTG cable (the common case — see
        // ADR-014's idle-USB-audio-class finding) never blocks this thread. A
        // failed open here is silent-degrade-only: the instrument-device path
        // above is already fully functional without it, matching looper's
        // "OTG is a passive mirror, never the graph's real output" design.
        snd_pcm_t* otgPlay = nullptr;
        bool otgReady = false;
        if (snd_pcm_open(&otgPlay, g_cfg.audioDevice.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0) {
            snd_pcm_hw_params_t* ohw;
            snd_pcm_hw_params_alloca(&ohw);
            snd_pcm_hw_params_any(otgPlay, ohw);
            snd_pcm_hw_params_set_access(otgPlay, ohw, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(otgPlay, ohw, SND_PCM_FORMAT_S16_LE);
            snd_pcm_hw_params_set_channels(otgPlay, ohw, wireCh);
            unsigned int otgRate = (unsigned int)g_cfg.sampleRate;
            snd_pcm_hw_params_set_rate_near(otgPlay, ohw, &otgRate, nullptr);
            // Looser latency target than the instrument device on purpose
            // (looper: OTG_LAG_TARGET=384 vs the real path's 96, 4x headroom) —
            // the OTG side doesn't need tight timing, just enough buffer that
            // its own USB-gadget scheduling jitter doesn't underrun constantly
            // (WITNESSED: a period matching block_size alone produced hundreds
            // of xruns/sec on the gadget path even with a real host attached).
            snd_pcm_uframes_t otgPeriod = (snd_pcm_uframes_t)N * 4;
            snd_pcm_hw_params_set_period_size_near(otgPlay, ohw, &otgPeriod, nullptr);
            snd_pcm_uframes_t otgBufFrames = otgPeriod * 4;
            snd_pcm_hw_params_set_buffer_size_near(otgPlay, ohw, &otgBufFrames);
            if (snd_pcm_hw_params(otgPlay, ohw) == 0) {
                snd_pcm_sw_params_t* osw;
                snd_pcm_sw_params_alloca(&osw);
                snd_pcm_sw_params_current(otgPlay, osw);
                snd_pcm_sw_params_set_start_threshold(otgPlay, osw, otgPeriod);
                snd_pcm_sw_params_set_avail_min(otgPlay, osw, otgPeriod);
                snd_pcm_sw_params(otgPlay, osw);
                snd_pcm_prepare(otgPlay);
                otgReady = true;
            }
        }
        if (!otgReady) {
            fprintf(stderr, "[audio] OTG gadget mirror (%s) unavailable — instrument-device audio is unaffected, gadget mirror stays off until it appears\n", g_cfg.audioDevice.c_str());
            if (otgPlay) { snd_pcm_close(otgPlay); otgPlay = nullptr; }
        }

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
                // APC live-pitch (CC1 mod-wheel / CC52 absolute, apc_grid.cpp): a
                // performance offset ON TOP of the static SEMIS knob (fx/pitch), so
                // add rather than overwrite — releasing the mod-wheel (engaged=0)
                // must fall back to the static knob value, not silently zero it.
                float staticSemis = g_params->get("fx/pitch");
                if (g_params->get("fx/pitchbend_engaged") > 0.5f) {
                    fui.set("SEMIS", staticSemis + g_params->get("fx/pitchbend"));
                    fui.set("ENGAGED", 1.0f);
                } else {
                    fui.set("SEMIS", staticSemis);
                }
                // Microrepeat latch (apc_grid.cpp notes 82-86) -> the microStage's
                // DIV zone (dsp/effects_runtime.dsp; was hardcoded 0, never wired).
                fui.set("DIV", g_params->get("fx/microrepeat_div"));
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
            // monitorMode telemetry (apcKey25.cpp:361's p.monitorMode = m_shift):
            // read back the same MONITORFOLD zone ApcGrid::onShiftPress/Release
            // drives, so a dev host can observe SHIFT/monitor-fold state live.
            g_telem.monitorMode = fui.get("MONITORFOLD") > 0.5f;
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
                    // Microrepeat's MLB (masterLoopBlocks, effects_runtime.dsp): the
                    // same phrase length expressed in DSP blocks, so a repeat slice
                    // (lenSamples/DIV) stays grid-aligned with the loop.
                    fui.set("MLB", (float)(lenSamples / N));
                } else {
                    fui.set("MLB", 0.0f);   // unsynced: microrepeat stays disabled (DIV!=0 & MLB>=16 gate)
                }
            }
            // Deinterleave the stereo wire -> mono DSP input: average the wire
            // channels (mono content is identical L/R; a stereo source is summed to
            // mono, matching the looper's mono internal path). wireCh==1 degenerates
            // to a straight copy. `buf` is S32_LE (24-bit data left-justified in the
            // top of a 32-bit word — confirmed via /proc/asound stream0 "Bits: 24"),
            // so normalize by INT32_MAX-equivalent range (2147483648.0), NOT 32768 —
            // using the s16 divisor here was the earlier "loud static" bug: it
            // treated 32-bit samples as if they were 16-bit magnitude, producing
            // values ~65536x too large before Faust even saw them.
            float inPeak = 0.0f;
            for (int i = 0; i < N; i++) {
                float acc = 0.0f;
                for (int c = 0; c < wireCh; c++) acc += (float)buf[(size_t)i * wireCh + c];
                fin[i] = (acc / wireCh) / 2147483648.0f;
                float a = fin[i] < 0 ? -fin[i] : fin[i];
                if (a > inPeak) inPeak = a;
            }
            g_telem.inPeak = inPeak;
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
            // Interleave the mono DSP output onto every wire channel (mono
            // duplicated to L and R so a stereo host hears it centered). wireCh==1
            // degenerates to a straight copy. Two separate conversions since the
            // instrument device (S32_LE) and the OTG gadget (S16_LE, our own
            // configfs c_ssize/p_ssize=2) have genuinely different wire formats —
            // see the capture-side comment above for why a shared 16-bit buffer
            // caused loud static once the instrument device was opened at all.
            float outPeak = 0.0f;
            for (int i = 0; i < N; i++) {
                float v32 = fout[i] * 2147483648.0f;
                int32_t s32 = (int32_t)(v32 > 2147483647.0f ? 2147483647 : (v32 < -2147483648.0f ? -2147483648.0f : v32));
                float v16 = fout[i] * 32768.0f;
                int16_t s16 = (int16_t)(v16 > 32767 ? 32767 : (v16 < -32768 ? -32768 : v16));
                for (int c = 0; c < wireCh; c++) {
                    buf[(size_t)i * wireCh + c] = s32;
                    otgBuf[(size_t)i * wireCh + c] = s16;
                }
                float a = fout[i] < 0 ? -fout[i] : fout[i];
                if (a > outPeak) outPeak = a;
            }
            g_telem.outPeak = outPeak;
#endif

            snd_pcm_sframes_t w = snd_pcm_writei(play, buf.data(), N);
            if (w < 0) { g_telem.xruns++; snd_pcm_recover(play, (int)w, 1); }

            // OTG mirror: best-effort, never allowed to affect the instrument
            // device's timing above. -EAGAIN (nonblock, ring still has enough
            // queued) is expected and silently skipped — it just means this
            // block's mirror copy is dropped, not a real error. Any other
            // negative return is a genuine device-level problem (unplugged,
            // reset); recover once, and if the device is gone for good the
            // next block's -EAGAIN/error keeps getting silently absorbed here
            // rather than ever blocking or crashing the RT path.
            if (otgReady) {
                snd_pcm_sframes_t ow = snd_pcm_writei(otgPlay, otgBuf.data(), N);
                if (ow < 0 && ow != -EAGAIN) snd_pcm_recover(otgPlay, (int)ow, 1);
            }
        }
        if (otgPlay) snd_pcm_close(otgPlay);
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
    // A real, reproducible SIGSEGV was WITNESSED live on a real Pi 4 here.
    // Root-caused via a debug build (-g -O0, unstripped) + gdb against a real
    // core dump: worker() declared `AloopLoopDsp faustHome;` as a stack-local
    // (sizeof(AloopLoopDsp) == 336,326,896 bytes, ~320 MiB — 20 loopers' worth
    // of 60s delay-line buffers) — no thread stack size, default or an
    // earlier-tried explicit 8 MiB, could ever hold that. Fixed by
    // heap-allocating it inside worker() instead (std::make_unique). The
    // default (nullptr) pthread attr here is correct as-is; do not add a large
    // explicit stack size in its place, since the fix is heap allocation, not
    // stack sizing.
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
