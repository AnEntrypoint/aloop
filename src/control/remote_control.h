// aloop remote control — a UDP listener for reboot + log-tail, adapted from
// looper's UDP REBOOT protocol (reboot.js -> udp/4444) and syslog capture
// (tftp-server.js/syslog-listener.js). looper's protocol has NO authentication
// (a bare 6-byte "REBOOT" string reboots the device from anyone on the LAN) --
// this port fixes that: both verbs require a shared-secret token configured in
// aloop.conf, and the listener is inert (does nothing on any packet) if no
// token is configured, so there is no insecure-by-default posture.

#ifndef ALOOP_REMOTE_CONTROL_H
#define ALOOP_REMOTE_CONTROL_H

#include <string>

namespace aloop {

class RemoteControl {
public:
    // `token` = the shared secret from aloop.conf's [remote] section; empty
    // string disables the listener entirely (bind() is skipped).
    void start(int udpPort, const std::string& token);
    void stop();

    // Called from the control loop each tick (same cadence as Telemetry::publish).
    // Drains all pending UDP packets (non-blocking), dispatching:
    //   "REBOOT:<token>"       -> sync() + reboot(2), never returns on success
    //   "LOGTAIL:<token>"      -> replies with new bytes appended to
    //                             /var/log/aloop.log since the last LOGTAIL call
    //                             from this token (server tracks one read
    //                             offset per process lifetime, not per client --
    //                             matches looper's single-dev-host assumption).
    void poll();

private:
    std::string token_;
};

} // namespace aloop
#endif // ALOOP_REMOTE_CONTROL_H
