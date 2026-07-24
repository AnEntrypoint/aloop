// aloop audio thread — the Linux replacement for the Circle multicore audio
// dispatch (MIGRATION-MAP: CMultiCoreSupport SEV/WFE -> pthreads + affinity +
// SCHED_FIFO; futex/eventfd wakeups). The lock-free discipline is unchanged from
// looper; only the OS primitives differ.

#ifndef ALOOP_AUDIO_THREAD_H
#define ALOOP_AUDIO_THREAD_H

#include <cstdint>
#include <string>

namespace aloop {

struct ParamStore;   // control values from MIDI (control/midi.h)
class  LinkBridge;   // Ableton Link tempo/phase (link/link_bridge.h)
class  Sampler;       // sampler subsystem (dsp/sampler/sampler.h)
class  Lv2Host;       // in-process LV2 host (host/lv2_host.h)

struct AudioConfig {
    int sampleRate = 48000;
    int blockSize  = 64;      // 1.333 ms budget — do not raise
    int channels   = 1;       // mono
    int homeFxCore = 1;       // pinned cores (match isolcpus in kernel/cmdline.txt)
    int userFxCore = 3;
    int rtPriority = 95;      // SCHED_FIFO
    // Effect bundle directories (aloop.conf [effects]). The home dir holds the fixed
    // home-FX LV2; the user dir holds swappable bundles. Loaded by DIRECTORY (any
    // *.lv2 present), never a hardcoded bundle name — the faust2lv2 bundle is named
    // after the .dsp (aloop.lv2), not "chain.lv2".
    std::string homeDir = "/effects/home";
    std::string userDir = "/effects/user";
    // DIAGNOSTIC ONLY (aloop.conf [effects] disable_core3_lv2=1): skips both
    // homeFx.process()/userFx.process() every block entirely (not just
    // disabling individual plugins -- the Lv2Host::process() call itself,
    // including its copy-in/copy-out over ioBuffer_, never runs). Added to
    // isolate a real, live-witnessed ~30-37ms stall firing at almost exactly
    // a 1.000-second period that appeared only after this session's Core-3
    // LV2 host wiring landed (confirmed absent on the pre-LOFI baseline,
    // confirmed NOT caused by the cpufreq governor or Ableton Link via
    // direct A/B tests) -- this flag lets the next session test "does the
    // stall exist with the Core-3 host code path fully skipped" via a config
    // edit + restart, without a full rebuild/redeploy cycle each time.
    bool disableCore3Lv2 = false;

    // MIDI input device: "auto" scans hw:0..7 for the first rawmidi input; an
    // explicit "hw:N,0,0" pins it (aloop.conf midi_device).
    std::string midiDevice = "auto";
    // Two DISTINCT ALSA devices, matching looper's split exactly (see ADR-015):
    // looper's real engine input/output is the USB-HOST audio class interface
    // (AudioInputUSB/AudioOutputUSB) — an instrument/mic physically plugs into
    // it and it is the tight-latency path a musician actually hears. The OTG
    // gadget (AudioOutputOTG/AudioInputOTG) is a passive MIRROR: the same
    // processed output is also sent out the gadget to whatever host computer is
    // plugged into the Pi's OTG port, and audio arriving FROM that host is
    // additively mixed into the engine input — but OTG is never the primary
    // path and its latency is explicitly not latency-critical (looper gives it
    // 4x the buffering headroom of the real device path).
    //
    // instrumentDevice = the real USB audio interface (aloop.conf
    // [audio] instrument_device=, default hw:0,0 — the first non-gadget USB
    // audio card, e.g. the M-Audio AIR 192|4). This is what the audio thread
    // reads capture from and writes its tight-latency playback to every block.
    std::string instrumentDevice = "hw:0,0";
    // audioDevice = the f_uac2 OTG gadget (aloop.conf [audio] audio_device=).
    // The gadget's ALSA card name is stable across boots (unlike its numeric
    // index, which shifts depending on USB host-device enumeration order);
    // "default" would route through ALSA's dmix/dsnoop plugin (a large fixed
    // period, ignoring block_size) and, without an /etc/asound.conf pinning
    // it, "default" can resolve to a different card altogether rather than
    // the gadget. This device is written to on a best-effort, non-blocking
    // basis (see worker()) — it must never stall or desync the instrument
    // device's real-time path, matching looper's independent-cursor design.
    std::string audioDevice = "hw:UAC2Gadget,0";
    // Shared secret for the remote-control listener (control/remote_control.h,
    // aloop.conf [remote] token=). Empty = listener disabled (no reboot/log-tail
    // over the network at all — the safe default, unlike looper's unauthenticated
    // UDP REBOOT protocol).
    std::string remoteToken = "";
};

// Starts the RT audio pipeline:
//   - mlockall(MCL_CURRENT|MCL_FUTURE)  — no page faults in the audio path
//   - opens the real instrument USB audio interface (tight-latency capture +
//     playback, ADR-015) and, best-effort/non-blocking, the f_uac2 OTG gadget
//     as a passive mirror of the same output
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
        bool     monitorMode = false;   // SHIFT held (apcKey25.cpp:361's p.monitorMode) -- loops folded into effects
        // GLITCH VERIFICATION: fx/microrepeat_div > 0 (glitch/microrepeat
        // engaged, notes 82-86 via apc_grid.cpp's onMicrorepeatOn/Off) --
        // deliberately independent of monitorMode/SHIFT (see dsp/aloop.dsp's
        // GLITCHFOLD comment), exposed so a test harness can directly verify
        // glitch content is recordable with monitorMode=false.
        bool     glitchEngaged = false;
        // Peak level of the last block's capture input and post-effects output
        // (0..1, absolute value of the float sample) -- lets a UDP query answer
        // "is real signal reaching the DSP at all" and "is anything coming out
        // the other side" without needing an external ALSA mixer tool on a
        // minimal Alpine image (no amixer package in the local netboot repo).
        float    inPeak = 0.0f;
        float    outPeak = 0.0f;
        // TRUE varispeed telemetry: the combined effSpeed (manual half/
        // double-speed x Link-tempo ratio) actually pushed into loop.dsp's
        // read accumulator this block -- lets a UDP query / structural test
        // confirm the read rate genuinely changed (e.g. reads 0.5 while
        // half-speed is held) without needing to hear the pitch shift.
        float    effSpeed = 1.0f;
        // Per-looper state — the Linux-native equivalent of the hardware's
        // GET_STATE 0x30 dump (which of the 20 loopers are recording/playing and
        // their levels). Read from the Faust zones each block; served on udp/4445.
        static constexpr int kLoopers = 20;
        bool     looperRec[kLoopers]  = {};   // 1 = recording (rec button held)
        bool     looperPlay[kLoopers] = {};   // 1 = playing (play checkbox on)
        float    looperVol[kLoopers]  = {};   // 0..1 output level
        float    looperLevel[kLoopers] = {};  // 0..1 live output peak (dsp/loop.dsp's "level" hbargraph,
                                               // ba.slidingMax envelope) -- drives the APC grid's real
                                               // 3-tier VU-meter LED coloring (apc_leds.cpp), matching
                                               // looper's vuLow/vuMid/vuHigh peak-based PLAY color tiers.
        // ARM-QUANTIZATION compensation: writeIdx's current sample-accurate
        // value (dsp/loop.dsp's "writeidx" hbargraph) -- the TRUE elapsed
        // sample count since the real (grid-quantized) arm instant, letting
        // apc_grid.cpp compute finish-quantization's rawSamples precisely
        // instead of estimating from wall-clock press-to-press timing.
        float    looperWriteIdx[kLoopers] = {};
        // QUANTIZATION VERIFICATION: the LATCHED loop length (dsp/loop.dsp's
        // "wraplen" hbargraph, only changes at finishEdge) -- unlike
        // looperWriteIdx (only meaningful mid-recording), this is the actual
        // final length every read-side consumer uses, letting a udp/4445
        // query or scripted test harness confirm a loop's real quantized
        // length without needing to listen to it.
        float    looperWrapLen[kLoopers] = {};
    };
    Telemetry snapshotTelemetry() const;

    // Sampler subsystem (dsp/sampler/sampler.h): heap-allocated inside
    // worker() once the thread starts, null until then (and always null in a
    // no-Faust review build). ApcGrid's sampler-dispatch methods (note
    // 65/66, channel-1 keybed routing) take this pointer so the MIDI thread
    // can push events into it without a header dependency on the sampler's
    // own event-ring layout.
    Sampler* sampler() const;

    // The permanent Core-3 guitar+lofi-fx LV2 host (Core-3 redesign): loaded
    // from AudioConfig::homeDir, always active, never hot-swapped. ApcGrid's
    // fx-knob dispatch pushes guitar/lofi-fx bank values into its fx2/* ports
    // via Lv2Host::setControl, the same way sampler() reaches the sampler.
    Lv2Host* homeFx() const;

private:
    void workerLoop();              // the per-block RT loop
    bool setRealtime(int core, int prio);   // affinity + SCHED_FIFO for this thread
    AudioConfig cfg_;
};

} // namespace aloop
#endif // ALOOP_AUDIO_THREAD_H
