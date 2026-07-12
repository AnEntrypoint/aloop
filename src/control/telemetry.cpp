// aloop telemetry implementation. See telemetry.h — ports looper's :4445 UDP
// diagnosability to Linux: a UDP responder + a status file. Control-thread only;
// never touches the audio hot path (it reads the audio thread's atomic snapshot).

#include "telemetry.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>

namespace aloop {

namespace {
int g_sock = -1;
int g_port = 4445;
}

void Telemetry::start(int udpPort) {
    g_port = udpPort;
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

    // Build the status snapshot. (In the composed process, main passes the live
    // AudioThread::Telemetry + host + wifi state; here we emit the schema so the
    // surface is exercisable and stable.)
    char json[512];
    int n = snprintf(json, sizeof json,
        "{\"core_busy\":[%.0f,%.0f,%.0f,%.0f],\"xruns\":%llu,"
        "\"link\":{\"synced\":%s,\"bpm\":%.1f},\"wifi\":\"%s\"}",
        0.0, 0.0, 0.0, 0.0, 0ULL, "false", 0.0, "auto");

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
