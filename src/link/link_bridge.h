#ifndef ALOOP_LINK_BRIDGE_H
#define ALOOP_LINK_BRIDGE_H

#include <cstdint>

namespace aloop {

constexpr double kLinkQuantum = 16.0;

struct LinkSnapshot {
    double  bpm          = 120.0;
    bool    synced       = false;
    bool    phaseValid   = false;
    int64_t beatPhaseMicroBeats = 0;
    int64_t quantumMicroBeats   = 0;
    bool    isPlaying    = false;
    int     peerCount    = 0;
};

class LinkBridge {
public:
    void start(double sampleRate, bool enabled);
    void stop();

    void controlTick();

    LinkSnapshot audioRead() const;

    void proposeTempo(double bpm);

    void setTransportPlaying(bool playing);

private:
    void* link_ = nullptr;
    LinkSnapshot buf_[2];
    unsigned active_ = 0;
};

}
#endif
