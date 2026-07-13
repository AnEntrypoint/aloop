// aloop remote control implementation. See remote_control.h.
//
// Reboot: looper's kernel_run.cpp receives a UDP "REBOOT" packet and sets a
// flag Core 0's bare-metal loop polls to perform the actual reboot (a
// cross-core handoff needed only because looper has no OS). aloop IS the OS
// (Alpine Linux), so the trigger and the action collapse into one step: on a
// valid REBOOT packet, sync() the filesystem (there is state worth flushing —
// telemetry's status file, any lbu-persisted apkovl changes) then call the
// real reboot(2) syscall. No cross-thread flag-poll needed.
//
// Log tail: looper's tftp-server.js/syslog-listener.js bind udp:514 and
// reassemble Circle's syslog-over-UDP protocol into a local file. aloop's logs
// are ALREADY a real file (/var/log/aloop.log, OpenRC output_log/error_log) --
// there is nothing to reassemble. The device-side job is simpler: track how
// much of that file has been sent, and on a LOGTAIL request, send whatever is
// new since the last read. A single fseek-remembered offset (not per-client)
// matches looper's implicit single-dev-host assumption and keeps this dead
// simple -- multiple simultaneous log-tail clients would each miss what
// another already consumed, which is an acceptable trade for a dev tool.

#include "remote_control.h"

#include <sys/socket.h>
#include <sys/reboot.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace aloop {

namespace {
int g_sock = -1;
long g_logOffset = -1;   // -1 = not yet initialized (start at EOF, don't dump history)
constexpr const char* kLogPath = "/var/log/aloop.log";
constexpr size_t kMaxReplyBytes = 4096;   // one UDP datagram's worth of new log lines
}

void RemoteControl::start(int udpPort, const std::string& token) {
    token_ = token;
    if (token_.empty()) {
        fprintf(stderr, "[remote] no token configured ([remote] token= in aloop.conf) — reboot/log-tail listener DISABLED\n");
        return;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { fprintf(stderr, "[remote] socket failed\n"); return; }
    int fl = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)udpPort);
    if (bind(g_sock, (sockaddr*)&a, sizeof a) < 0) {
        fprintf(stderr, "[remote] bind :%d failed\n", udpPort);
        close(g_sock); g_sock = -1;
        return;
    }
    fprintf(stderr, "[remote] listening on udp/%d (REBOOT:<token> / LOGTAIL:<token>)\n", udpPort);
}

void RemoteControl::stop() { if (g_sock >= 0) { close(g_sock); g_sock = -1; } }

// Reads whatever is new in /var/log/aloop.log since the last call (or, on the
// FIRST call, seeks to EOF and reads nothing — a fresh log-tail client should
// only see NEW lines, not the entire boot history, matching `tail -f` not `cat`).
static size_t readNewLogBytes(char* buf, size_t maxBytes) {
    FILE* f = fopen(kLogPath, "rb");
    if (!f) return 0;
    if (g_logOffset < 0) {
        fseek(f, 0, SEEK_END);
        g_logOffset = ftell(f);
        fclose(f);
        return 0;   // first call establishes the baseline, sends nothing
    }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    if (end < g_logOffset) g_logOffset = 0;   // log was rotated/truncated — restart from 0
    fseek(f, g_logOffset, SEEK_SET);
    size_t n = fread(buf, 1, maxBytes, f);
    g_logOffset = ftell(f);
    fclose(f);
    return n;
}

void RemoteControl::poll() {
    if (g_sock < 0 || token_.empty()) return;

    char req[128]; sockaddr_in from{}; socklen_t fl = sizeof from;
    for (;;) {   // drain every pending packet this tick (non-blocking socket)
        ssize_t r = recvfrom(g_sock, req, sizeof req - 1, 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        req[r] = 0;

        std::string msg(req, (size_t)r);
        auto colon = msg.find(':');
        if (colon == std::string::npos) continue;
        std::string verb = msg.substr(0, colon);
        std::string tok  = msg.substr(colon + 1);
        if (tok != token_) {
            fprintf(stderr, "[remote] rejected %s: bad token\n", verb.c_str());
            continue;
        }

        if (verb == "REBOOT") {
            fprintf(stderr, "[remote] REBOOT accepted — rebooting now\n");
            sync();
            reboot(RB_AUTOBOOT);   // does not return on success; needs CAP_SYS_BOOT (process runs as root)
            fprintf(stderr, "[remote] reboot(2) failed: %s (need CAP_SYS_BOOT?)\n", strerror(errno));
        } else if (verb == "LOGTAIL") {
            static char buf[kMaxReplyBytes];
            size_t n = readNewLogBytes(buf, sizeof buf);
            sendto(g_sock, buf, n, 0, (sockaddr*)&from, fl);   // n==0 is a valid "nothing new" reply
        }
    }
}

} // namespace aloop
