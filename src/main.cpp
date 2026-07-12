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
#include "host/lv2_host.h"
#include "link/link_bridge.h"
#include "control/telemetry.h"
#include "control/midi.h"

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
        int v;
        if (sscanf(line, " home_fx = %d", &v) == 1) cfg.homeFxCore = v;
        else if (sscanf(line, " user_fx = %d", &v) == 1) cfg.userFxCore = v;
        else if (sscanf(line, " audio_priority = %d", &v) == 1) cfg.rtPriority = v;
        else if (sscanf(line, " block_size = %d", &v) == 1) cfg.blockSize = v;
        else if (sscanf(line, " sample_rate = %d", &v) == 1) cfg.sampleRate = v;
        else if (sscanf(line, " channels = %d", &v) == 1) cfg.channels = v;
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

    // Load the effects: the fixed home-FX bundle on the home core, then any user
    // bundles from the flash dir on the free core. A bad user bundle is skipped,
    // never fatal (DEGRADED-MODES).
    aloop::Lv2Host host;
    host.loadBundle("/effects/home/chain.lv2", cfg.homeFxCore);
    host.loadDir("/effects/user");
    host.connect(cfg.blockSize, cfg.channels);

    // Ableton Link + telemetry on the control thread (never the audio cores).
    aloop::LinkBridge link;
    link.start((double)cfg.sampleRate, /*enabled=*/true);
    aloop::Telemetry telem;
    telem.start(/*udpPort=*/4445);

    // MIDI control on its own thread (the control surface — the APC knobs/commands).
    // It writes the shared param store; the audio thread reads it. Runs on the
    // control core alongside Link. A missing controller is fine (params hold).
    aloop::ParamStore params;
    std::thread midiThread([&]{ aloop::runMidiLoop(params, "auto"); });
    midiThread.detach();

    // Start the RT audio pipeline (opens the ALSA/f_uac2 PCM, spawns the pinned
    // SCHED_FIFO worker that runs DSP -> host.runBlock() -> PCM each block).
    aloop::AudioThread audio;
    if (!audio.start(cfg, &params, &link)) {   // audio reads the control store + Link (varispeed)
        fprintf(stderr, "[aloop] fatal: could not start audio pipeline\n");
        return 1;
    }
    printf("[aloop] ready.\n");

    // Control loop: pump Link + telemetry ~ a few Hz. The audio runs independently
    // on its own cores; this thread never touches the audio hot path.
    while (g_run.load()) {
        link.controlTick();
        telem.publish();
        usleep(200 * 1000);   // 5 Hz
    }

    printf("[aloop] shutting down.\n");
    audio.stop();
    telem.stop();
    link.stop();
    return 0;
}
