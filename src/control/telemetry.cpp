// aloop telemetry implementation. See telemetry.h — ports looper's :4445 UDP
// diagnosability to Linux: a UDP responder + a status file. Control-thread only;
// never touches the audio hot path (it reads the audio thread's atomic snapshot).

#include "telemetry.h"
#include "../dsp/audio_thread.h"   // AudioThread::Telemetry (the live snapshot)

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
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
    for (int i = 0; i < AudioThread::Telemetry::kLoopers; i++) {
        if (t.looperRec[i])  recBits  |= (1u << i);
        if (t.looperPlay[i]) playBits |= (1u << i);
        vp += snprintf(vols + vp, sizeof vols - vp, i ? ",%.2f" : "%.2f", t.looperVol[i]);
    }
    vols[vp++] = ']'; vols[vp] = 0;

    char json[768];
    int n = snprintf(json, sizeof json,
        "{\"core_busy\":[%.0f,%.0f,%.0f,%.0f],\"xruns\":%llu,"
        "\"link\":{\"synced\":%s,\"bpm\":%.1f},\"wifi\":\"%s\","
        "\"loopers\":{\"rec\":%u,\"play\":%u,\"vol\":%s}}",
        t.coreBusyPct[0], t.coreBusyPct[1], t.coreBusyPct[2], t.coreBusyPct[3],
        (unsigned long long)t.xruns,
        t.linkSynced ? "true" : "false", t.bpm,
        t.apMode ? "ap" : "sta",
        recBits, playBits, vols);

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
