// aloop — process entry point.
//
// Wires the whole runtime together in the order docs/BOOT.md describes:
//   parse config -> mlockall -> start audio (RT threads pinned) -> load LV2s ->
//   start Link + telemetry on the control thread -> run until stopped.
//
// This is the top-level composition; each subsystem's behavior lives in its own
// translation unit (audio_thread.cpp, lv2_host.cpp, link_bridge.cpp,
// telemetry.cpp). Keeping main.cpp a thin wiring layer keeps the spine flat.

#include "dsp/audio_thread.h"
#include "link/link_bridge.h"
#include "control/telemetry.h"
#include "control/midi.h"
#include "control/remote_control.h"

#include <sys/mman.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <dirent.h>
#include <sched.h>

namespace {
std::atomic<bool> g_run{true};
void onSignal(int) { g_run.store(false); }

// WITNESSED live on a real Pi 4 (this session): a genuinely NEW ~30-37ms
// stall on the audio thread's per-block read, firing at almost exactly a
// 1.000-second period (measured directly via wall-clock-timestamped
// [diag-gap] log lines: t=26.037, 27.037, 28.037, 29.036, 30.036, 31.036...
// -- a real, consistent period, not jitter). Confirmed absent in the
// pre-LOFI baseline (100% hitch-free), so this is a regression from this
// session's own work, not a pre-existing hardware limit.
//
// Ableton Link (third-party, vendored ableton::Link) spawns its OWN internal
// threads (named "Link Main" and "Link Dispatcher" by the library itself,
// not by aloop) the moment `ableton::Link(...)` is constructed in
// LinkBridge::start() -- this codebase has never had any control over their
// scheduling, since Link's own public API exposes no thread-affinity hook.
// `ps`/`/proc/<tid>/stat` on the live device showed both threads' cumulative
// kernel time growing in lockstep, consistent with Link's own known
// multicast-peer-discovery protocol timing (its "gateway"/session-sync
// messages are commonly sent on ~second-scale intervals) -- a real
// candidate for periodically waking on or otherwise contending with the
// isolated audio cores (1, 3), since nothing has ever excluded them.
//
// This does NOT add any audio-path latency (the explicit, hard constraint
// for this investigation) -- it only steers two pre-existing background
// threads' CPU affinity onto the control core (2), matching
// kernel/rt-tune.sh's existing "steer non-audio work off the isolated
// cores" strategy for IRQs, applied here to these two specific threads by
// name since Link creates them internally with no host-visible handle.
void pinLinkThreadsToControlCore(int controlCore) {
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent* e;
    int pinned = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char commPath[64];
        snprintf(commPath, sizeof commPath, "/proc/self/task/%s/comm", e->d_name);
        FILE* f = fopen(commPath, "r");
        if (!f) continue;
        char name[64] = {0};
        if (fgets(name, sizeof name, f)) {
            size_t len = strlen(name);
            if (len && name[len - 1] == '\n') name[len - 1] = '\0';
        }
        fclose(f);
        if (strcmp(name, "Link Main") == 0 || strcmp(name, "Link Dispatcher") == 0) {
            pid_t tid = (pid_t)atoi(e->d_name);
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(controlCore, &set);
            if (sched_setaffinity(tid, sizeof set, &set) == 0) {
                fprintf(stderr, "[link] pinned %s (tid %d) to control core %d\n", name, (int)tid, controlCore);
                pinned++;
            } else {
                fprintf(stderr, "[link] warning: failed to pin %s (tid %d) to core %d\n", name, (int)tid, controlCore);
            }
        }
    }
    closedir(d);
    if (!pinned) fprintf(stderr, "[link] warning: no Link Main/Dispatcher threads found to pin yet (may not have spawned)\n");
}

// Minimal config load: reads /etc/aloop.conf key=value under [sections]. Missing
// file / keys fall back to documented defaults (DEGRADED-MODES: config missing).
aloop::AudioConfig loadConfig(const char* path) {
    aloop::AudioConfig cfg;   // defaults from the struct
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[aloop] no config at %s — using defaults\n", path); return cfg; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int v; char s[200];
        if (sscanf(line, " home_fx = %d", &v) == 1) cfg.homeFxCore = v;
        else if (sscanf(line, " user_fx = %d", &v) == 1) cfg.userFxCore = v;
        else if (sscanf(line, " audio_priority = %d", &v) == 1) cfg.rtPriority = v;
        else if (sscanf(line, " block_size = %d", &v) == 1) cfg.blockSize = v;
        else if (sscanf(line, " sample_rate = %d", &v) == 1) cfg.sampleRate = v;
        else if (sscanf(line, " channels = %d", &v) == 1) cfg.channels = v;
        // effect bundle dirs ([effects] home_dir / user_dir). Strip a trailing
        // inline comment / whitespace so the path is clean.
        else if (sscanf(line, " home_dir = %199s", s) == 1) cfg.homeDir = s;
        else if (sscanf(line, " user_dir = %199s", s) == 1) cfg.userDir = s;
        // optional explicit MIDI device (else "auto" scans hw:0..7).
        else if (sscanf(line, " midi_device = %199s", s) == 1) cfg.midiDevice = s;
        // wire audio device (else the f_uac2 gadget's stable-named hw device).
        else if (sscanf(line, " audio_device = %199s", s) == 1) cfg.audioDevice = s;
        // the real USB audio interface an instrument/mic plugs into (else hw:0,0).
        else if (sscanf(line, " instrument_device = %199s", s) == 1) cfg.instrumentDevice = s;
        // remote-control shared secret ([remote] token=); unset = listener disabled.
        else if (sscanf(line, " token = %199s", s) == 1) cfg.remoteToken = s;
    }
    fclose(f);
    return cfg;
}
} // namespace

int main(int argc, char** argv) {
    const char* configPath = "/etc/aloop.conf";
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--config")) configPath = argv[i + 1];

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    aloop::AudioConfig cfg = loadConfig(configPath);
    printf("[aloop] %d Hz, %d-sample block, home-FX core %d, user-FX core %d, rtprio %d\n",
           cfg.sampleRate, cfg.blockSize, cfg.homeFxCore, cfg.userFxCore, cfg.rtPriority);

    // Lock all memory into RAM — the audio path must never page-fault (RT-TUNING).
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[aloop] warning: mlockall failed (need CAP_IPC_LOCK / rtprio limits)\n");

    // NOTE on the effects topology (see docs/ARCHITECTURE.md):
    //   · HOME effects are NOT a separately-loaded LV2 here — they are the Faust
    //     home stack (dsp/aloop.dsp = loop engine : effects_runtime), compiled INTO
    //     the aloop binary and run by the audio thread's `faustHome`. (The
    //     build-lv2 `aloop.lv2` bundle is the alternative hot-swap packaging of that
    //     same stack; the binary runs the compiled-in version directly.)
    //   · USER effects ARE dynamic LV2s: the audio thread loads them from userDir on
    //     the free core via its own in-process host (audio_thread.cpp).
    // So there is no top-level Lv2Host here — the audio thread owns the only host,
    // and we hand it the user-effects dir + core through the config.

    // Ableton Link on the control thread (never the audio cores). Telemetry is
    // started AFTER the audio thread below so it can read the live snapshot.
    aloop::LinkBridge link;
    link.start((double)cfg.sampleRate, /*enabled=*/false); // TEMP A/B TEST -- Link disabled to isolate the 1Hz stall, revert before committing
    // See pinLinkThreadsToControlCore's own comment: steers Link's internal
    // threads off the isolated audio cores (1, 3) onto the control core (2),
    // matching kernel/rt-tune.sh's CONTROL_CORE. Core 2 is currently hardcoded
    // here and in rt-tune.sh (not read from AudioConfig) -- both must be kept
    // in sync if that ever changes.
    pinLinkThreadsToControlCore(2);

    // aloop::AudioThread is declared before the MIDI thread launches (but
    // started after) so runMidiLoop can hold a pointer to it for reading live
    // per-looper level telemetry (LED VU-meter coloring) — snapshotTelemetry()
    // is safe to call before start() (returns the default-constructed
    // all-zero Telemetry), so there's no ordering hazard even though the MIDI
    // thread may begin running before audio.start() completes below.
    aloop::AudioThread audio;

    // MIDI control on its own thread (the control surface — the APC knobs/commands).
    // It writes the shared param store; the audio thread reads it. Runs on the
    // control core alongside Link. A missing controller is fine (params hold).
    aloop::ParamStore params;
    std::thread midiThread([&, dev = cfg.midiDevice]{ aloop::runMidiLoop(params, dev.c_str(), &audio, &link); });
    midiThread.detach();

    // Start the RT audio pipeline (opens the ALSA/f_uac2 PCM, spawns the pinned
    // SCHED_FIFO worker that runs DSP -> host.runBlock() -> PCM each block).
    if (!audio.start(cfg, &params, &link)) {   // audio reads the control store + Link (varispeed)
        fprintf(stderr, "[aloop] fatal: could not start audio pipeline\n");
        return 1;
    }

    // Telemetry now that audio is running — it serves the audio thread's live
    // atomic snapshot (core load, xruns, Link sync/bpm) on udp/4445 + status file.
    aloop::Telemetry telem;
    telem.start(/*udpPort=*/4445, &audio);

    // Remote control (reboot + log-tail, dev-tooling parity with looper's UDP
    // REBOOT/syslog mechanisms — see docs/REMOTE-CONTROL.md). Own port (4446,
    // not 4445) so telemetry's query-response semantics never conflate with a
    // destructive verb. Inert unless aloop.conf sets [remote] token=.
    aloop::RemoteControl remote;
    remote.start(/*udpPort=*/4446, cfg.remoteToken);

    printf("[aloop] ready.\n");

    // Control loop: pump Link + telemetry + remote-control ~ a few Hz. The audio
    // runs independently on its own cores; this thread never touches the audio
    // hot path.
    while (g_run.load()) {
        link.controlTick();
        telem.publish();
        remote.poll();
        usleep(200 * 1000);   // 5 Hz
    }

    printf("[aloop] shutting down.\n");
    audio.stop();
    telem.stop();
    remote.stop();
    link.stop();
    return 0;
}
