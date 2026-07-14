// aloop APC Key25 LED output — ports ../looper's apcKey25Transpose.cpp LED
// protocol. aloop previously never wrote MIDI back to the controller at all
// (snd_rawmidi_read only), so none of the pad/button LEDs ever lit up even
// though the underlying looper/preset state changes correctly — this module
// is the missing output half.
//
// Protocol (../looper apcKey25.cpp/apcKey25Transpose.cpp, usbMidi.cpp):
//   - LEDs are driven by sending a plain Note On (0x90, channel 0) back out
//     the SAME rawmidi handle the input comes in on (the APC's USB MIDI is
//     one bidirectional endpoint, not separate in/out devices) with the pad's
//     note number and a velocity that encodes color/blink state. Velocity 0
//     is LED-off (there is no separate Note-Off for LEDs).
//   - A coalescing cache avoids re-sending a value that hasn't changed since
//     last refresh (looper: s_lastLedState, apcKey25Transpose.cpp:91-110).

#ifndef ALOOP_APC_LEDS_H
#define ALOOP_APC_LEDS_H

#include <cstdint>
#include <array>
#include "apc_grid.h"   // kApcRows/kApcCols + ApcGrid's full definition -- refresh() is a
                        // template defined here, so it needs both visible at the point of
                        // instantiation (in midi.cpp), not just a forward declaration.

namespace aloop {

// AKAI APC-series LED velocity constants (looper apcKey25.h:20-26 / midiMap.h:249-255).
enum ApcLedVel : uint8_t {
    kLedOff         = 0,
    kLedGreen       = 1,
    kLedGreenBlink  = 2,
    kLedRed         = 3,
    kLedRedBlink    = 4,
    kLedYellow      = 5,
    kLedYellowBlink = 6,
};

// Real APC Key25 button notes this module drives (apcKey25.h, midiMap.h).
constexpr int kApcBtnStopAll = 0x51;   // 81 -- looper: LOOP_COMMAND_STOP indicator
constexpr int kApcBtnPlay    = 0x5B;   // 91 -- looper: SHIFT-held indicator (yellow while held)
constexpr int kApcLiveLedNote = 0x40;  // 64 -- live-pitch engage LED (velocity 127/0, not the ApcLedVel table)

// Owns the 128-slot coalescing cache and pushes LED updates over MIDI OUT.
// Call refresh() at ~30Hz (looper: 33ms, apcKey25.cpp:471) from the same
// control-thread tick that already drives ApcGrid::pollHolds — a plain
// wall-clock poll, not event-driven, matching looper's design (every LED is
// recomputed from current state each tick; the cache is what makes that cheap
// over MIDI, not the recompute itself).
class ApcLeds {
public:
    // `write` is a caller-supplied sink (send(note, velocity) -> bool success)
    // so this module doesn't need to know about snd_rawmidi_t directly — kept
    // decoupled from ALSA specifics the same way ApcGrid stays decoupled from
    // rawmidi read framing.
    // `looperLevels` (may be null): 20-element array of per-looper live output
    // peak (0..1, dsp/loop.dsp's "level" hbargraph via AudioThread::Telemetry)
    // -- drives the 3-tier VU-meter PLAY coloring, matching looper's
    // vuLow/vuMid/vuHigh peak-based tiers. Passed as a raw pointer rather than
    // an AudioThread reference to keep this module decoupled from the audio
    // subsystem, same as it's decoupled from ALSA specifics via WriteFn. Null
    // degrades PLAY color to a flat GREEN (the pre-metering behavior).
    template <typename WriteFn>
    void refresh(unsigned now_ms, const ApcGrid& grid, bool liveEngaged, WriteFn&& write, const float* looperLevels = nullptr) {
        // Boot delay: looper waits APC_LED_BOOT_DELAY_MS (2000ms) after boot
        // before the first LED write, so the APC has fully enumerated/settled
        // (apcKey25.h:29, apcKey25.cpp:470-473). Without it, LED writes sent
        // too early are reliably dropped/ignored by the controller's own
        // firmware — WITNESSED equivalent behavior on other class-compliant
        // USB MIDI controllers during aloop's own boot-race investigations
        // (ADR-*, .default_boot_services).
        if (!bootMs_) bootMs_ = now_ms ? now_ms : 1;   // first call establishes the epoch
        if (now_ms - bootMs_ < kBootDelayMs) return;
        if (now_ms - lastMs_ < kRefreshMs) return;
        lastMs_ = now_ms;

        for (int row = 0; row < kApcRows; row++) {
            for (int col = 0; col < kApcCols; col++) {
                int note = row * kApcCols + col;
                sendCoalesced(note, gridColor(row, col, grid, looperLevels), write);
            }
        }
        sendCoalesced(kApcBtnPlay, grid.shiftHeld() ? kLedYellow : kLedOff, write);
        sendCoalesced(kApcLiveLedNote, liveEngaged ? 127 : 0, write);
    }

    // Force a full re-send next refresh (looper: invalidateLedCache, called on
    // a USB-MIDI device roster change so a re-plugged controller resyncs
    // rather than staying stuck on stale colors, apcKey25Transpose.cpp:91-101).
    void invalidate() { cacheValid_.fill(false); }

private:
    static constexpr unsigned kBootDelayMs = 2000;
    static constexpr unsigned kRefreshMs = 33;   // ~30Hz

    unsigned bootMs_ = 0;
    unsigned lastMs_ = 0;
    std::array<uint8_t, 128> cache_{};
    std::array<bool, 128> cacheValid_{};

    template <typename WriteFn>
    void sendCoalesced(int note, uint8_t velocity, WriteFn&& write) {
        if (cacheValid_[note] && cache_[note] == velocity) return;
        if (write(note, velocity)) { cache_[note] = velocity; cacheValid_[note] = true; }
        // on send failure: leave cacheValid_ false so the next tick retries
        // (matches looper's "commit to cache only on successful send").
    }

    uint8_t gridColor(int row, int col, const ApcGrid& grid, const float* looperLevels) const;
};

} // namespace aloop
#endif // ALOOP_APC_LEDS_H
