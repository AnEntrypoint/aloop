#include "audio_thread.h"
#include "../host/lv2_host.h"
#include "../control/midi.h"
#include "../link/link_bridge.h"
#include "sampler/sampler.h"
#include "../storage/usb_recorder.h"

#include <pthread.h>
#include <sched.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#if defined(__SSE2__) && !defined(__aarch64__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#define ALOOP_HAVE_ALSA 1
#endif

#include <fstream>
#include <sstream>
#include <string>

static std::string resolveInstrumentDevice(const std::string& configured, const std::string& matchName) {
    if (matchName.empty()) return configured;
    std::ifstream cards("/proc/asound/cards");
    if (!cards.is_open()) return configured;
    std::string line;
    int index = -1;
    while (std::getline(cards, line)) {
        if (line.find(matchName) == std::string::npos) continue;
        std::istringstream iss(line);
        iss >> index;
        if (iss.fail()) { index = -1; continue; }
        break;
    }
    if (index < 0) {
        fprintf(stderr, "[audio] instrument_device_match '%s' not found in /proc/asound/cards -- falling back to configured %s\n",
                matchName.c_str(), configured.c_str());
        return configured;
    }
    char resolved[32];
    snprintf(resolved, sizeof resolved, "hw:%d,0", index);
    fprintf(stderr, "[audio] instrument device '%s' matched card %d -> %s\n", matchName.c_str(), index, resolved);
    return resolved;
}

#if __has_include("loop.cpp")
#define FAUSTFLOAT float
struct FaustMeta { void declare(const char*, const char*) {} };
#include <map>
#include <string>
struct FaustUI {
    std::map<std::string, float*> zones;
    std::vector<std::string> path;
    std::string full(const char* label) const {
        std::string p;
        for (auto& g : path) if (!g.empty()) { p += g; p += "/"; }
        p += label;
        return p;
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
    void addHorizontalBargraph(const char* l, float* z, float, float){ zones[full(l)]=z; }
    void addVerticalBargraph(const char* l, float* z, float, float){ zones[full(l)]=z; }
    void addSoundfile(const char*, const char*, void**){}
    void declare(float*, const char*, const char*){}
    void set(const char* name, float v){
        auto it=zones.find(name);
        if(it!=zones.end()){ *it->second=v; return; }
        std::string suf(name);
        for(auto& kv:zones){ const std::string& k=kv.first;
            if(k.size()>=suf.size() && k.compare(k.size()-suf.size(), suf.size(), suf)==0){ *kv.second=v; return; } }
    }
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
#include "loop.cpp"
#undef dsp
#define ALOOP_HAVE_FAUST_LOOP 1
#endif

namespace aloop {

namespace {
std::atomic<bool> g_running{false};
pthread_t g_worker;
AudioThread::Telemetry g_telem{};
AudioConfig g_cfg;
ParamStore* g_params = nullptr;
LinkBridge* g_link = nullptr;
aloop::Sampler* g_sampler = nullptr;
aloop::Lv2Host* g_homeFx = nullptr;
aloop::UsbRecorder* g_usbRecorder = nullptr;

float g_manualSpeedMul = 1.0f;
constexpr int kTransposeVoices = 6;
constexpr int kResonodeVoices = 4;


static std::string targetToZone(const std::string& target) {
    if (target.rfind("looper", 0) == 0) {
        auto slash = target.find('/');
        if (slash != std::string::npos) {
            int idx = atoi(target.c_str() + 6);
            char z[64];
            snprintf(z, sizeof z, "looper%2d/%s", idx, target.c_str() + slash + 1);
            return z;
        }
    }
    if (target == "fx/hp")      return "HPCUT";
    if (target == "fx/lp")      return "LPCUT";
    if (target == "fx/lpres")   return "LPRES";
    if (target == "fx/reverb")  return "REVAMT";
    if (target == "fx/delay")   return "DELAYAMT";
    if (target == "fx/time")    return "TIME";
    if (target == "fx/formant") return "FORMANT";
    if (target == "fx/pitch")   return "SEMIS";
    if (target == "fx/bank")    return "fx/bank";
    if (target.rfind("fx/resonode/", 0) == 0) return target;
    return "";
}

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
}

static void setFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
#elif defined(__SSE2__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

static void* worker(void*) {
    setRealtimeSelf(g_cfg.homeFxCore, g_cfg.rtPriority);
    setFlushToZero();
    const int N = g_cfg.blockSize;
    const int ch = g_cfg.channels;

    const int wireCh = (ch < 2) ? 2 : ch;

    std::vector<int32_t> buf((size_t)N * wireCh, 0);
    std::vector<int16_t> otgBuf((size_t)N * wireCh, 0);
    std::vector<float> fin((size_t)N, 0.0f), fout((size_t)N, 0.0f);
    std::vector<float> prevLoopSum((size_t)N, 0.0f);
    std::vector<float> prevFiltOut((size_t)N, 0.0f);

#ifdef ALOOP_HAVE_FAUST_LOOP
    auto faustHomePtr = std::make_unique<AloopLoopDsp>();
    AloopLoopDsp& faustHome = *faustHomePtr;
    faustHome.init((int)g_cfg.sampleRate);
    FaustUI fui; faustHome.buildUserInterface(&fui);
    std::vector<float> clearBuf((size_t)N, 0.0f);
    std::vector<float> speedBuf((size_t)N, 1.0f);
    std::vector<float> masterPhaseBuf((size_t)N, 0.0f);
    std::vector<float> masterLenBuf((size_t)N, 0.0f);
    std::vector<float> sidechainEnvBuf((size_t)N, 0.0f);
    std::vector<float> freeXposeBuf((size_t)N, 0.0f);
    std::vector<float> xposeNoteBuf[kTransposeVoices];
    std::vector<float> xposeGateBuf[kTransposeVoices];
    for (int v = 0; v < kTransposeVoices; v++) {
        xposeNoteBuf[v].assign((size_t)N, 0.0f);
        xposeGateBuf[v].assign((size_t)N, 0.0f);
    }
    std::vector<float> resonodeNoteBuf[kResonodeVoices];
    std::vector<float> resonodeGateBuf[kResonodeVoices];
    std::vector<float> resonodeVelBuf[kResonodeVoices];
    for (int v = 0; v < kResonodeVoices; v++) {
        resonodeNoteBuf[v].assign((size_t)N, 0.0f);
        resonodeGateBuf[v].assign((size_t)N, 0.0f);
        resonodeVelBuf[v].assign((size_t)N, 1.0f);
    }
    float* fins[32] = {
        fin.data(), prevFiltOut.data(), clearBuf.data(), speedBuf.data(), masterPhaseBuf.data(), masterLenBuf.data(), sidechainEnvBuf.data(),
        freeXposeBuf.data(),
        xposeNoteBuf[0].data(), xposeGateBuf[0].data(),
        xposeNoteBuf[1].data(), xposeGateBuf[1].data(),
        xposeNoteBuf[2].data(), xposeGateBuf[2].data(),
        xposeNoteBuf[3].data(), xposeGateBuf[3].data(),
        xposeNoteBuf[4].data(), xposeGateBuf[4].data(),
        xposeNoteBuf[5].data(), xposeGateBuf[5].data(),
        resonodeNoteBuf[0].data(), resonodeGateBuf[0].data(), resonodeVelBuf[0].data(),
        resonodeNoteBuf[1].data(), resonodeGateBuf[1].data(), resonodeVelBuf[1].data(),
        resonodeNoteBuf[2].data(), resonodeGateBuf[2].data(), resonodeVelBuf[2].data(),
        resonodeNoteBuf[3].data(), resonodeGateBuf[3].data(), resonodeVelBuf[3].data(),
    };
    int sidechainSrcSlot[AudioThread::Telemetry::kLoopers];
    for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) sidechainSrcSlot[lp] = -1;
    struct ResolvedControl { int slot; float* zone; };
    std::vector<ResolvedControl> resolvedControls;
    int resolvedControlsForCount = -1;
    struct LooperTelemetryZones { float* rec=nullptr; float* play=nullptr; float* vol=nullptr; float* level=nullptr; float* writeidx=nullptr; float* wraplen=nullptr; float* readpos=nullptr; };
    LooperTelemetryZones looperTelemetryZones[AudioThread::Telemetry::kLoopers];
    {
        char z[32];
        auto resolveZone = [&]() -> float* {
            auto it = fui.zones.find(z);
            if (it != fui.zones.end()) return it->second;
            std::string suf(z);
            for (auto& kv : fui.zones) {
                const std::string& k = kv.first;
                if (k.size() >= suf.size() && k.compare(k.size() - suf.size(), suf.size(), suf) == 0) return kv.second;
            }
            return nullptr;
        };
        for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) {
            auto& tz = looperTelemetryZones[lp];
            snprintf(z, sizeof z, "looper%2d/rec",  lp);         tz.rec      = resolveZone();
            snprintf(z, sizeof z, "looper%2d/play", lp);         tz.play     = resolveZone();
            snprintf(z, sizeof z, "looper%2d/vol",  lp);         tz.vol      = resolveZone();
            snprintf(z, sizeof z, "looper%2d/level", lp);        tz.level    = resolveZone();
            snprintf(z, sizeof z, "looper%2d/writeidx", lp);     tz.writeidx = resolveZone();
            snprintf(z, sizeof z, "looper%2d/wraplen", lp);      tz.wraplen  = resolveZone();
            snprintf(z, sizeof z, "looper%2d/readposdiag2", lp); tz.readpos  = resolveZone();
        }
    }
    int xposeNoteSlot[kTransposeVoices];
    int xposeGateSlot[kTransposeVoices];
    {
        char z[32];
        for (int v = 0; v < kTransposeVoices; v++) {
            snprintf(z, sizeof z, "fx/xpose%d/note", v);
            xposeNoteSlot[v] = g_params ? g_params->getSlot(z) : -1;
            snprintf(z, sizeof z, "fx/xpose%d/gate", v);
            xposeGateSlot[v] = g_params ? g_params->getSlot(z) : -1;
        }
    }
    int resonodeNoteSlot[kResonodeVoices];
    int resonodeGateSlot[kResonodeVoices];
    int resonodeVelSlot[kResonodeVoices];
    {
        char z[32];
        for (int v = 0; v < kResonodeVoices; v++) {
            snprintf(z, sizeof z, "fx/resonodevoice%d/note", v);
            resonodeNoteSlot[v] = g_params ? g_params->getSlot(z) : -1;
            snprintf(z, sizeof z, "fx/resonodevoice%d/gate", v);
            resonodeGateSlot[v] = g_params ? g_params->getSlot(z) : -1;
            snprintf(z, sizeof z, "fx/resonodevoice%d/vel", v);
            resonodeVelSlot[v] = g_params ? g_params->getSlot(z) : -1;
        }
    }
    std::vector<float> rawLoopSum((size_t)N, 0.0f);
    std::vector<float> rawFiltTap((size_t)N, 0.0f);
    float* fouts[3] = { fout.data(), rawLoopSum.data(), rawFiltTap.data() };
#endif

#ifdef ALOOP_HAVE_FAUST_LOOP
    auto samplerPtr = std::make_unique<Sampler>();
    g_sampler = samplerPtr.get();
    std::vector<int32_t> samplerBuf((size_t)N, 0);
#endif

    Lv2Host homeFx;
    homeFx.loadDir(g_cfg.homeDir, g_cfg.userFxCore);
    homeFx.connect(N, ch);
    g_homeFx = &homeFx;

    Lv2Host userFx;
    userFx.loadDir(g_cfg.userDir, g_cfg.userFxCore);
    userFx.connect(N, ch);

    UsbRecorder usbRecorder(g_cfg.usbMountPoint, g_cfg.sampleRate, g_cfg.usbChunkMinutes, g_cfg.usbChunkCount);
    if (g_cfg.usbRecordEnabled) g_usbRecorder = &usbRecorder;

#ifdef ALOOP_HAVE_ALSA
    snd_pcm_t *cap = nullptr, *play = nullptr;
    const int kAlsaOpenRetries = 30;
    std::string wireDevStr;
    for (int attempt = 0; attempt < kAlsaOpenRetries; attempt++) {
        wireDevStr = resolveInstrumentDevice(g_cfg.instrumentDevice, g_cfg.instrumentDeviceMatch);
        const char* wireDev = wireDevStr.c_str();
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
        auto configurePcm = [&](snd_pcm_t* pcm) -> bool {
            snd_pcm_hw_params_t* hw;
            snd_pcm_hw_params_alloca(&hw);
            snd_pcm_hw_params_any(pcm, hw);
            snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
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
            snd_pcm_format_t negotiatedFmt;
            if (snd_pcm_hw_params_get_format(hw, &negotiatedFmt) == 0 && negotiatedFmt != SND_PCM_FORMAT_S32_LE)
                fprintf(stderr, "[audio] warning: instrument device negotiated format %s, not S32_LE — audio will be corrupted (buf is int32_t)\n",
                        snd_pcm_format_name(negotiatedFmt));
            if (period != (snd_pcm_uframes_t)N)
                fprintf(stderr, "[audio] warning: device would not grant period=%d frames, got %lu — latency will not match block_size\n",
                        N, (unsigned long)period);
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
            fprintf(stderr, "[audio] warning: explicit hw_params rejected by %s — falling back to driver defaults (higher latency)\n", wireDevStr.c_str());
        snd_pcm_prepare(cap);
        snd_pcm_prepare(play);

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

        timespec lastReadTs{}; bool haveLastReadTs = false;
        const double kExpectedPeriodMs = (double)N / g_cfg.sampleRate * 1000.0;
        while (g_running.load()) {
            timespec nowTs; clock_gettime(CLOCK_MONOTONIC, &nowTs);
            if (haveLastReadTs) {
                double gapMs = (nowTs.tv_sec - lastReadTs.tv_sec) * 1000.0 + (nowTs.tv_nsec - lastReadTs.tv_nsec) / 1e6;
                if (gapMs > kExpectedPeriodMs * 1.5) {
                    fprintf(stderr, "[diag-gap] t=%ld.%03ld readi gap=%.3fms (expected ~%.3fms)\n",
                            (long)nowTs.tv_sec, nowTs.tv_nsec / 1000000, gapMs, kExpectedPeriodMs);
                }
            }
            lastReadTs = nowTs;
            haveLastReadTs = true;
            timespec readStartTs; clock_gettime(CLOCK_MONOTONIC, &readStartTs);
            snd_pcm_sframes_t r = snd_pcm_readi(cap, buf.data(), N);
            timespec readEndTs; clock_gettime(CLOCK_MONOTONIC, &readEndTs);
            {
                double readMs = (readEndTs.tv_sec - readStartTs.tv_sec) * 1000.0 + (readEndTs.tv_nsec - readStartTs.tv_nsec) / 1e6;
                if (readMs > kExpectedPeriodMs * 1.5) {
                    fprintf(stderr, "[diag-gap] t=%ld.%03ld readi ITSELF took=%.3fms (expected ~%.3fms)\n",
                            (long)readEndTs.tv_sec, readEndTs.tv_nsec / 1000000, readMs, kExpectedPeriodMs);
                }
            }
            if (r < 0) { g_telem.xruns++; snd_pcm_recover(cap, (int)r, 1); continue; }

#ifdef ALOOP_HAVE_FAUST_LOOP
            if (g_params) {
                if (resolvedControlsForCount != g_params->count) {
                    resolvedControls.clear();
                    g_params->forEach([&](const std::string& target, int slotIdx){
                        std::string zone = targetToZone(target);
                        if (zone.empty()) return;
                        auto it = fui.zones.find(zone);
                        if (it == fui.zones.end()) {
                            for (auto& kv : fui.zones) {
                                const std::string& k = kv.first;
                                if (k.size() >= zone.size() && k.compare(k.size() - zone.size(), zone.size(), zone) == 0) {
                                    it = fui.zones.find(k);
                                    break;
                                }
                            }
                        }
                        if (it != fui.zones.end()) resolvedControls.push_back({slotIdx, it->second});
                    });
                    resolvedControlsForCount = g_params->count;
                }
                for (auto& rc : resolvedControls) *rc.zone = g_params->getBySlot(rc.slot);
                bool clearAllHeld = g_params->get("cmd/clearall") > 0.5f;
                std::fill(clearBuf.begin(), clearBuf.end(), clearAllHeld ? 1.0f : 0.0f);
                if (clearAllHeld) {
                    g_params->setByName("cmd/master_len", 0.0f);
                    g_params->setByName("cmd/recorded_bpm", 0.0f);
                }
                float manualSpeedMul = 1.0f;
                if (g_params->get("cmd/halfspeed")   > 0.5f) manualSpeedMul = 0.5f;
                if (g_params->get("cmd/doublespeed") > 0.5f) manualSpeedMul = 2.0f;
                g_manualSpeedMul = manualSpeedMul;
                if (g_params->get("cmd/stopall") > 0.5f) {
                    char z[32];
                    for (int lp = 0; lp < 20; lp++) {
                        snprintf(z, sizeof z, "looper%2d/play", lp);
                        fui.set(z, 0.0f);
                    }
                }
                for (int v = 0; v < kTransposeVoices; v++) {
                    std::fill(xposeNoteBuf[v].begin(), xposeNoteBuf[v].end(), g_params->getBySlot(xposeNoteSlot[v]));
                    std::fill(xposeGateBuf[v].begin(), xposeGateBuf[v].end(), g_params->getBySlot(xposeGateSlot[v]));
                }
                for (int v = 0; v < kResonodeVoices; v++) {
                    std::fill(resonodeNoteBuf[v].begin(), resonodeNoteBuf[v].end(), g_params->getBySlot(resonodeNoteSlot[v]));
                    std::fill(resonodeGateBuf[v].begin(), resonodeGateBuf[v].end(), g_params->getBySlot(resonodeGateSlot[v]));
                    std::fill(resonodeVelBuf[v].begin(), resonodeVelBuf[v].end(), g_params->getBySlot(resonodeVelSlot[v]));
                }
                std::fill(freeXposeBuf.begin(), freeXposeBuf.end(), g_params->get("fx/monitorfold") > 0.5f ? 1.0f : 0.0f);
                float staticSemis = g_params->get("fx/pitch");
                if (g_params->get("fx/pitchbend_engaged") > 0.5f) {
                    fui.set("SEMIS", staticSemis + g_params->get("fx/pitchbend"));
                    fui.set("ENGAGED", 1.0f);
                } else {
                    fui.set("SEMIS", staticSemis);
                }
                fui.set("DIV", g_params->get("fx/microrepeat_div"));
            }
            {
                for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) {
                    auto& tz = looperTelemetryZones[lp];
                    g_telem.looperRec[lp]      = tz.rec      && *tz.rec > 0.5f;
                    g_telem.looperPlay[lp]     = tz.play     && *tz.play > 0.5f;
                    g_telem.looperVol[lp]      = tz.vol      ? *tz.vol : 1.0f;
                    g_telem.looperLevel[lp]    = tz.level    ? *tz.level : 0.0f;
                    g_telem.looperWriteIdx[lp] = tz.writeidx ? *tz.writeidx : 0.0f;
                    g_telem.looperWrapLen[lp]  = tz.wraplen  ? *tz.wraplen : 0.0f;
                    g_telem.looperReadPos[lp]  = tz.readpos  ? *tz.readpos : 0.0f;
                }
            }
            g_telem.monitorMode = g_params && g_params->get("fx/monitorfold") > 0.5f;
            g_telem.glitchEngaged = g_params && g_params->get("fx/microrepeat_div") > 0.5f;
            bool linkDrivingLength = false;
            if (g_link) {
                LinkSnapshot ls = g_link->audioRead();
                g_telem.linkSynced = ls.synced;
                g_telem.bpm = ls.bpm;
                g_telem.linkPeers = ls.peerCount;
                g_telem.linkPlaying = ls.isPlaying;
                if (ls.synced && ls.bpm > 1.0) {
                    linkDrivingLength = true;
                    double beatsPerBar = 4.0;
                    double samplesPerBeat = (g_cfg.sampleRate * 60.0) / ls.bpm;
                    double lenSamples = samplesPerBeat * beatsPerBar;
                    char z[32];
                    for (int lp = 0; lp < 20; lp++) {
                        snprintf(z, sizeof z, "looper%2d/len", lp);
                        fui.set(z, (float)lenSamples);
                    }
                    fui.set("MLB", (float)(lenSamples / N));
                }
            }
            if (!linkDrivingLength && g_params) {
                static float frozenMasterLen = 0.0f;
                float masterLen = g_params->get("cmd/master_len", 0.0f);
                if (masterLen <= 0.0f) {
                    frozenMasterLen = 0.0f;
                } else if (frozenMasterLen <= 0.0f) {
                    frozenMasterLen = masterLen;
                }
                fui.set("MLB", frozenMasterLen > 0.0f ? (frozenMasterLen / (float)N) : 0.0f);
            }
            float linkSpeedRatio = 1.0f;
            if (linkDrivingLength && g_params && g_link) {
                float recordedBpm = g_params->get("cmd/recorded_bpm", 0.0f);
                LinkSnapshot ls2 = g_link->audioRead();
                if (recordedBpm > 1.0f && ls2.bpm > 1.0) {
                    linkSpeedRatio = recordedBpm / (float)ls2.bpm;
                }
            }
            {
                float effSpeed = g_manualSpeedMul * linkSpeedRatio;
                std::fill(speedBuf.begin(), speedBuf.end(), effSpeed);
                g_telem.effSpeed = effSpeed;
            }
            {
                static double masterPhaseSamples = 0.0;
                static double standaloneQuantumPhaseSamples = 0.0;
                static int64_t lastLinkPhaseMicroBeats = -1;
                float masterLen = g_params ? g_params->get("cmd/master_len", 0.0f) : 0.0f;
                if (masterLen > 0.0f) {
                    if (linkDrivingLength && g_link) {
                        LinkSnapshot ls3 = g_link->audioRead();
                        bool freshSnapshot = ls3.phaseValid && ls3.quantumMicroBeats > 0 &&
                                              ls3.beatPhaseMicroBeats != lastLinkPhaseMicroBeats;
                        if (freshSnapshot) {
                            lastLinkPhaseMicroBeats = ls3.beatPhaseMicroBeats;
                            double linkPhaseFrac = (double)ls3.beatPhaseMicroBeats / (double)ls3.quantumMicroBeats;
                            if (linkPhaseFrac < 0.0) linkPhaseFrac = 0.0;
                            if (linkPhaseFrac >= 1.0) linkPhaseFrac = 0.0;
                            masterPhaseSamples = linkPhaseFrac * (double)masterLen;
                        } else {
                            masterPhaseSamples += (double)N;
                        }
                    } else {
                        lastLinkPhaseMicroBeats = -1;
                        masterPhaseSamples += (double)N;
                    }
                    masterPhaseSamples = std::fmod(masterPhaseSamples, (double)masterLen);
                    if (masterPhaseSamples < 0.0) masterPhaseSamples += masterLen;

                    float recordedBpmForQuantum = g_params ? g_params->get("cmd/recorded_bpm", 0.0f) : 0.0f;
                    if (recordedBpmForQuantum > 1.0f) {
                        double quantumSamples = (g_cfg.sampleRate * 60.0 / (double)recordedBpmForQuantum) * 16.0;
                        standaloneQuantumPhaseSamples += (double)N;
                        standaloneQuantumPhaseSamples = std::fmod(standaloneQuantumPhaseSamples, quantumSamples);
                        if (standaloneQuantumPhaseSamples < 0.0) standaloneQuantumPhaseSamples += quantumSamples;
                    } else {
                        standaloneQuantumPhaseSamples = masterPhaseSamples;
                    }
                } else {
                    masterPhaseSamples = 0.0;
                    standaloneQuantumPhaseSamples = 0.0;
                    lastLinkPhaseMicroBeats = -1;
                }
                if (masterLen > 0.0f) {
                    const double lenD = (double)masterLen;
                    if (lenD >= (double)N) {
                        double p = masterPhaseSamples;
                        for (int i = 0; i < N; i++) {
                            masterPhaseBuf[(size_t)i] = (float)p;
                            p += 1.0;
                            if (p >= lenD) p -= lenD;
                        }
                    } else {
                        for (int i = 0; i < N; i++) {
                            double p = masterPhaseSamples + (double)i;
                            p = std::fmod(p, lenD);
                            if (p < 0.0) p += lenD;
                            masterPhaseBuf[(size_t)i] = (float)p;
                        }
                    }
                } else {
                    std::fill(masterPhaseBuf.begin(), masterPhaseBuf.end(), 0.0f);
                }
                std::fill(masterLenBuf.begin(), masterLenBuf.end(), masterLen);
                if (linkDrivingLength && g_link) {
                    LinkSnapshot lsBeat = g_link->audioRead();
                    if (lsBeat.phaseValid && lsBeat.quantumMicroBeats > 0) {
                        double frac = (double)lsBeat.beatPhaseMicroBeats / (double)lsBeat.quantumMicroBeats;
                        if (frac < 0.0) frac = 0.0;
                        if (frac >= 1.0) frac = 0.0;
                        int idx = (int)(frac * 16.0);
                        if (idx < 0) idx = 0;
                        if (idx > 15) idx = 15;
                        g_telem.gridBeatIndex = idx;
                    } else {
                        g_telem.gridBeatIndex = -1;
                    }
                } else if (masterLen > 0.0f) {
                    float recordedBpmForQuantum = g_params ? g_params->get("cmd/recorded_bpm", 0.0f) : 0.0f;
                    double quantumSamples = recordedBpmForQuantum > 1.0f
                        ? (g_cfg.sampleRate * 60.0 / (double)recordedBpmForQuantum) * 16.0
                        : (double)masterLen;
                    double gridStep = quantumSamples / 16.0;
                    int idx = (int)(standaloneQuantumPhaseSamples / gridStep);
                    if (idx < 0) idx = 0;
                    if (idx > 15) idx = 15;
                    g_telem.gridBeatIndex = idx;
                } else {
                    g_telem.gridBeatIndex = -1;
                }
            }
            float inPeak = 0.0f;
            for (int i = 0; i < N; i++) {
                float acc = 0.0f;
                for (int c = 0; c < wireCh; c++) acc += (float)buf[(size_t)i * wireCh + c];
                fin[i] = (acc / wireCh) / 2147483648.0f;
                float a = fin[i] < 0 ? -fin[i] : fin[i];
                if (a > inPeak) inPeak = a;
            }
            g_telem.inPeak = inPeak;
            for (int i = 0; i < N; i++) samplerBuf[(size_t)i] = (int32_t)(fin[i] * 32768.0f);
            g_sampler->renderInto(samplerBuf.data(), N);
            for (int i = 0; i < N; i++) fin[i] = (float)samplerBuf[(size_t)i] / 32768.0f;
            if (g_params) {
                static float foldGain = 0.0f;
                bool anyXposeVoiceGatedNow = false;
                for (int v = 0; v < kTransposeVoices; v++) {
                    if (g_params->getBySlot(xposeGateSlot[v]) > 0.5f) { anyXposeVoiceGatedNow = true; break; }
                }
                bool shiftHeldNow = g_params->get("fx/monitorfold") > 0.5f;
                float foldTarget = (shiftHeldNow && !anyXposeVoiceGatedNow) ? 1.0f : 0.0f;
                const float kFoldStep = 1.0f / 16.0f;
                const float kFoldStepPerSample = kFoldStep / (float)N;
                static float glitchFoldGain = 0.0f;
                float glitchFoldTarget = g_params->get("fx/microrepeat_div") > 0.5f ? 1.0f : 0.0f;
                for (int i = 0; i < N; i++) {
                    if (foldGain < foldTarget)      { foldGain += kFoldStepPerSample; if (foldGain > foldTarget) foldGain = foldTarget; }
                    else if (foldGain > foldTarget) { foldGain -= kFoldStepPerSample; if (foldGain < foldTarget) foldGain = foldTarget; }
                    if (glitchFoldGain < glitchFoldTarget)      { glitchFoldGain += kFoldStepPerSample; if (glitchFoldGain > glitchFoldTarget) glitchFoldGain = glitchFoldTarget; }
                    else if (glitchFoldGain > glitchFoldTarget) { glitchFoldGain -= kFoldStepPerSample; if (glitchFoldGain < glitchFoldTarget) glitchFoldGain = glitchFoldTarget; }
                    float combinedFold = foldGain + glitchFoldGain;
                    if (combinedFold > 1.0f) combinedFold = 1.0f;
                    fin[i] += prevLoopSum[i] * combinedFold;
                }
                fui.set("MONITORFOLD", foldGain);
                fui.set("GLITCHFOLD", glitchFoldGain);
            }
            for (int i = 0; i < N; i++) samplerBuf[(size_t)i] = (int32_t)(prevFiltOut[i] * 32768.0f);
            g_sampler->captureBlock(samplerBuf.data(), N);
            if (g_usbRecorder) g_usbRecorder->pushBlock(prevFiltOut.data(), N);
            g_telem.usbRecording = g_usbRecorder && g_usbRecorder->recording();
            g_telem.usbRecOverruns = g_usbRecorder ? g_usbRecorder->overruns() : 0;
            {
                float sidechainEnv = 0.0f;
                if (g_params) {
                    for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) {
                        if (sidechainSrcSlot[lp] < 0) {
                            char z[32];
                            snprintf(z, sizeof z, "looper%d/sidechainsrc", lp);
                            sidechainSrcSlot[lp] = g_params->getSlot(z);
                        }
                        if (g_params->getBySlot(sidechainSrcSlot[lp]) > 0.5f) {
                            float lvl = g_telem.looperLevel[lp];
                            if (lvl > sidechainEnv) sidechainEnv = lvl;
                        }
                    }
                }
                std::fill(sidechainEnvBuf.begin(), sidechainEnvBuf.end(), sidechainEnv);
            }
            timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            faustHome.compute(N, fins, fouts);
            if (!g_cfg.disableCore3Lv2) {
                homeFx.process(fout.data(), N);
                userFx.process(fout.data(), N);
            }
            prevLoopSum = rawLoopSum;
            prevFiltOut = rawFiltTap;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            {
                double workNs = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
                double periodNs = (double)N / g_cfg.sampleRate * 1e9;
                double pct = periodNs > 0 ? (workNs / periodNs) * 100.0 : 0.0;
                float& slot = g_telem.coreBusyPct[g_cfg.homeFxCore & 3];
                slot = slot * 0.9f + (float)pct * 0.1f;
            }
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

            timespec writeStartTs; clock_gettime(CLOCK_MONOTONIC, &writeStartTs);
            snd_pcm_sframes_t w = snd_pcm_writei(play, buf.data(), N);
            timespec writeEndTs; clock_gettime(CLOCK_MONOTONIC, &writeEndTs);
            {
                double writeMs = (writeEndTs.tv_sec - writeStartTs.tv_sec) * 1000.0 + (writeEndTs.tv_nsec - writeStartTs.tv_nsec) / 1e6;
                if (writeMs > kExpectedPeriodMs * 1.5) {
                    fprintf(stderr, "[diag-gap] writei ITSELF took=%.3fms (expected ~%.3fms)\n", writeMs, kExpectedPeriodMs);
                }
            }
            if (w < 0) { g_telem.xruns++; snd_pcm_recover(play, (int)w, 1); }

            if (otgReady) {
                snd_pcm_sframes_t ow = snd_pcm_writei(otgPlay, otgBuf.data(), N);
                if (ow < 0 && ow != -EAGAIN) snd_pcm_recover(otgPlay, (int)ow, 1);
            }
        }
        if (otgPlay) snd_pcm_close(otgPlay);
        snd_pcm_close(cap); snd_pcm_close(play);
    }
#else
    while (g_running.load()) {}
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
Sampler* AudioThread::sampler() const { return g_sampler; }
Lv2Host* AudioThread::homeFx() const { return g_homeFx; }
UsbRecorder* AudioThread::usbRecorder() const { return g_usbRecorder; }
bool AudioThread::setRealtime(int core, int prio) { return setRealtimeSelf(core, prio); }
void AudioThread::workerLoop() {}

}
