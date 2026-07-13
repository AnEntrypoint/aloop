// aloop APC Key25 grid engine — public interface (impl in apc_grid.cpp).
//
// Ports looper's apcKey25Notes.cpp pad-grid + tap/hold + preset logic onto
// aloop's ParamStore/named-target model. This is the part of the real hardware
// surface config/controls.conf's flat note table cannot express: the 5x8 grid
// (row*8+col), tap-vs-hold disambiguation, and 10-slot preset capture/recall.

#ifndef ALOOP_APC_GRID_H
#define ALOOP_APC_GRID_H

#include <cstdint>
#include "midi.h"

namespace aloop {

constexpr int kApcRows = 5;
constexpr int kApcCols = 8;
constexpr int kLooperCount = 20;
constexpr int kPresetCount = 10;
constexpr unsigned kHoldEraseMs = 1000;   // apcKey25.h APC_HOLD_ERASE_MS

// row*8+col grid index -> looper index (cols 2-5) or -1 (apcKey25Notes.cpp _looperFromPad)
inline int gridLooperIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 2 || col > 5) return -1;
    int idx = row * 4 + (col - 2);
    return (idx >= 0 && idx < kLooperCount) ? idx : -1;
}
// row*8+col grid index -> preset index (cols 0-1) or -1 (apcKey25Notes.cpp _presetFromPad)
inline int gridPresetIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 0 || col > 1) return -1;
    int idx = row * 2 + col;
    return (idx >= 0 && idx < kPresetCount) ? idx : -1;
}

// Owns the tap/hold state machine for the pad grid + the 10 preset slots, and
// the continuous live-pitch (mod-wheel CC1 / CC52) state. `now_ms` is a
// monotonic millisecond clock the caller supplies (from the same clock the
// MIDI read loop uses) so this stays independent of any specific timer API.
class ApcGrid {
public:
    // A pad (note 0..39, channel 0) was pressed. Writes commands into `ps`.
    void onPadPress(int note, unsigned now_ms, ParamStore& ps);
    // A pad (note 0..39, channel 0) was released. Writes commands into `ps`.
    void onPadRelease(int note, unsigned now_ms, ParamStore& ps);
    // Poll for long-holds that must fire without waiting for release (erase
    // trigger at >= kHoldEraseMs, and preset-capture at the same threshold).
    // Call once per control-thread tick (e.g. on every MIDI byte, cheap).
    void pollHolds(unsigned now_ms, ParamStore& ps);

    // Live pitch: CC1 (mod-wheel, deadzone 59-69) or CC52 (absolute 0-127).
    void onModWheel(uint8_t data2, ParamStore& ps);     // CC1
    void onAbsolutePitch(uint8_t data2, ParamStore& ps); // CC52

    // Microrepeat latch notes 82-86 (channel 0 only); div in {1,2,4,8,16}, 0=off.
    void onMicrorepeatOn(int note, ParamStore& ps);
    void onMicrorepeatOff(int note, ParamStore& ps);

private:
    bool m_looperHeld[kLooperCount] = {};
    unsigned m_looperHoldStart[kLooperCount] = {};
    bool m_looperErased[kLooperCount] = {};      // true once the hold-erase fired this press
    bool m_looperArmedOnPress[kLooperCount] = {}; // suppress the release tap (armed on press)
    bool m_looperPlaying[kLooperCount] = {};      // local shadow: last rec/play state we sent
    bool m_looperHasContent[kLooperCount] = {};   // local shadow: has this looper recorded anything

    bool m_presetHeld[kPresetCount] = {};
    unsigned m_presetHoldStart[kPresetCount] = {};
    bool m_presetCaptured[kPresetCount] = {};
    bool m_presetUsed[kPresetCount] = {};
    uint32_t m_presetMask[kPresetCount] = {};

    uint8_t m_microRepeatDiv = 0;

    void applyRecPlayCycle(int looper, ParamStore& ps);
    void capturePreset(int p, ParamStore& ps);
    void applyPreset(int p, ParamStore& ps);
    void forgetLooperFromPresets(int looper);
};

} // namespace aloop
#endif // ALOOP_APC_GRID_H
