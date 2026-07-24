// aloop telemetry implementation. See telemetry.h — ports looper's :4445 UDP
// diagnosability to Linux: a UDP responder + a status file. Control-thread only;
// never touches the audio hot path (it reads the audio thread's atomic snapshot).

#include "telemetry.h"
#include "../dsp/audio_thread.h"   // AudioThread::Telemetry (the live snapshot)

#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace aloop {

namespace {
int g_sock = -1;
int g_port = 4445;
}

void Telemetry::start(int udpPort, const AudioThread* audio) {
    g_port = udpPort;
    audio_ = audio;   // the live snapshot source (may be null before audio starts)
    // Ensure the status-file directory exists BEFORE publish() writes it. /run is a
    // tmpfs that exists, but the /run/aloop subdir does not — without this mkdir the
    // fopen("/run/aloop/status.json","w") in publish() silently returns NULL and the
    // status file (docs/FLASHING.md step 5) never appears. Idempotent: EEXIST is fine.
    if (mkdir("/run/aloop", 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "[telem] warning: could not create /run/aloop (%s)\n", strerror(errno));
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { fprintf(stderr, "[telem] socket failed\n"); return; }
    int fl = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, fl | O_NONBLOCK);   // non-blocking: never stall the loop
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)g_port);
    if (bind(g_sock, (sockaddr*)&a, sizeof a) < 0)
        fprintf(stderr, "[telem] bind :%d failed\n", g_port);
    else
        fprintf(stderr, "[telem] listening on udp/%d (query for status)\n", g_port);
}

void Telemetry::stop() { if (g_sock >= 0) { close(g_sock); g_sock = -1; } }

// Called ~5 Hz from the control loop. Answer any pending query with the current
// status JSON, and refresh the status file. The status content is assembled from
// the audio thread's atomic telemetry + the host's plugin states + the wifi state
// (wired to those getters as the process composes them in main).
void Telemetry::publish() {
    if (g_sock < 0) return;

    // Build the status snapshot from the LIVE audio-thread telemetry (atomic
    // snapshot — no lock, never touches the hot path). Before audio starts, or if
    // no source was wired, report the not-yet-running defaults.
    AudioThread::Telemetry t{};
    if (audio_) t = audio_->snapshotTelemetry();

    // Per-looper state (GET_STATE 0x30 equivalent): compact rec/play bitmaps over
    // the 20 loopers + the vols array, so a controller can reflect looper state.
    uint32_t recBits = 0, playBits = 0;
    char vols[20 * 5 + 2]; int vp = 0; vols[vp++] = '[';
    char levels[20 * 7 + 2]; int lvp = 0; levels[lvp++] = '[';
    // QUANTIZATION VERIFICATION: each looper's latched loop length in
    // samples (dsp/loop.dsp's "wraplen" hbargraph) -- exposed so a scripted
    // test harness (test/hardware/) can confirm a finished recording's real
    // quantized length without needing to listen to it. Up to 8 digits per
    // entry (MAXLEN = 48000*60 = 2,880,000 fits in 7 digits) plus separator.
    char wraplens[20 * 9 + 2]; int wlp = 0; wraplens[wlp++] = '[';
    for (int i = 0; i < AudioThread::Telemetry::kLoopers; i++) {
        if (t.looperRec[i])  recBits  |= (1u << i);
        if (t.looperPlay[i]) playBits |= (1u << i);
        vp += snprintf(vols + vp, sizeof vols - vp, i ? ",%.2f" : "%.2f", t.looperVol[i]);
        // TEMPORARY diagnostic (tracked for removal): per-looper live output
        // level, for the "second clear-and-restart cycle produces blank
        // loops" investigation -- confirms/refutes whether the RECORDED
        // content is genuinely silent vs. audible-but-not-heard for another
        // reason (routing, volume, playback gating).
        lvp += snprintf(levels + lvp, sizeof levels - lvp, i ? ",%.4f" : "%.4f", t.looperLevel[i]);
        wlp += snprintf(wraplens + wlp, sizeof wraplens - wlp, i ? ",%.0f" : "%.0f", t.looperWrapLen[i]);
    }
    vols[vp++] = ']'; vols[vp] = 0;
    levels[lvp++] = ']'; levels[lvp] = 0;
    wraplens[wlp++] = ']'; wraplens[wlp] = 0;

    char json[1024];
    int n = snprintf(json, sizeof json,
        "{\"core_busy\":[%.0f,%.0f,%.0f,%.0f],\"xruns\":%llu,"
        "\"link\":{\"synced\":%s,\"bpm\":%.1f},\"wifi\":\"%s\",\"monitor_mode\":%s,"
        "\"audio_peak\":{\"in\":%.4f,\"out\":%.4f},\"eff_speed\":%.4f,"
        "\"loopers\":{\"rec\":%u,\"play\":%u,\"vol\":%s,\"level\":%s,\"wraplen\":%s}}",
        t.coreBusyPct[0], t.coreBusyPct[1], t.coreBusyPct[2], t.coreBusyPct[3],
        (unsigned long long)t.xruns,
        t.linkSynced ? "true" : "false", t.bpm,
        t.apMode ? "ap" : "sta",
        t.monitorMode ? "true" : "false",
        t.inPeak, t.outPeak, t.effSpeed,
        recBits, playBits, vols, levels, wraplens);

    // Write the status file for shell/curl inspection.
    FILE* f = fopen("/run/aloop/status.json", "w");
    if (f) { fwrite(json, 1, (size_t)n, f); fclose(f); }

    // Answer any UDP query (non-blocking; drop if none).
    char req[64]; sockaddr_in from{}; socklen_t fl = sizeof from;
    ssize_t r = recvfrom(g_sock, req, sizeof req - 1, 0, (sockaddr*)&from, &fl);
    if (r > 0) {
        req[r] = 0;   // any request → full status (looper's :4445 verb style)
        sendto(g_sock, json, (size_t)n, 0, (sockaddr*)&from, fl);
    }
}

} // namespace aloop
