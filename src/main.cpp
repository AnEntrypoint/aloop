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
#include <string>
#include <atomic>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> g_run{true};
void onSignal(int) { g_run.store(false); }

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
    link.start((double)cfg.sampleRate, /*enabled=*/true);

    // MIDI control on its own thread (the control surface — the APC knobs/commands).
    // It writes the shared param store; the audio thread reads it. Runs on the
    // control core alongside Link. A missing controller is fine (params hold).
    aloop::ParamStore params;
    std::thread midiThread([&, dev = cfg.midiDevice]{ aloop::runMidiLoop(params, dev.c_str()); });
    midiThread.detach();

    // Start the RT audio pipeline (opens the ALSA/f_uac2 PCM, spawns the pinned
    // SCHED_FIFO worker that runs DSP -> host.runBlock() -> PCM each block).
    aloop::AudioThread audio;
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
