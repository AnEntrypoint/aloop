// aloop telemetry — the diagnosability surface, ported from looper's :4445 UDP
// verb table (docs/ARCHITECTURE.md). A running device must be inspectable: core
// load, xruns, Link sync, AP/STA state, which effects are loaded/disabled.
//
// Two surfaces: a UDP socket (query/response, like looper :4445) AND a plain
// status file (/run/aloop/status.json) for shell/curl inspection. Neither runs
// on the audio thread — the control thread reads the atomic telemetry snapshot
// and serves it.

#ifndef ALOOP_TELEMETRY_H
#define ALOOP_TELEMETRY_H

namespace aloop {

class Telemetry {
public:
    void start(int udpPort = 4445);   // matches looper's telemetry port
    void stop();

    // Called from the control loop ~2 Hz: pull the audio thread's atomic
    // telemetry + the host's plugin states + the wifi state, and publish to the
    // UDP responder and the status file. Never touches the audio hot path.
    void publish();
};

} // namespace aloop
#endif // ALOOP_TELEMETRY_H
