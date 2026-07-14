#include "apc_leds.h"
#include "apc_grid.h"

namespace aloop {

// Per-pad color classification (../looper apcKey25Transpose.cpp:124-249,
// state table midiMap.h:143-159). Peak-level PLAY_LOW/MID/HIGH tiers now use
// real per-looper output-peak telemetry (dsp/loop.dsp's "level" hbargraph,
// read via AudioThread::Telemetry::looperLevel[] and passed in as
// looperLevels). looper's own vuLow=200/vuMid=1500/vuHigh=8000 thresholds are
// raw s16-range magnitudes (looper's internal signal chain is s16, confirmed
// via m_input_buffer/m_output_buffer casts throughout loopMachine.cpp);
// aloop's Faust engine works in normalized [-1,1] float, so the equivalent
// proportional thresholds are those raw values / 32768.
uint8_t ApcLeds::gridColor(int row, int col, const ApcGrid& grid, const float* looperLevels) const {
    // (looper's vuLow=200 has no distinct effect here -- below vuMid is already
    // the GREEN fallback, matching looper's own tier collapse for quiet signals)
    static constexpr float kVuMid  = 1500.0f / 32768.0f;   // ~0.0458 -- below this: moderate (YELLOW)
    static constexpr float kVuHigh = 8000.0f / 32768.0f;   // ~0.2441 -- at/above this: loud (RED)
    int looper = gridLooperIndex(row, col);
    if (looper >= 0) {
        if (grid.looperRecording(looper))   return kLedRedBlink;     // MFS_LOOPER_RECORDING
        if (!grid.looperHasContent(looper)) return kLedOff;          // MFS_LOOPER_EMPTY
        if (grid.looperPlaying(looper)) {
            float level = looperLevels ? looperLevels[looper] : 0.0f;
            if (level >= kVuHigh) return kLedRed;       // MFS_LOOPER_PLAY_HIGH
            if (level >= kVuMid)  return kLedYellow;    // MFS_LOOPER_PLAY_MID
            return kLedGreen;                           // MFS_LOOPER_PLAY_LOW (also the >=vuLow / null-telemetry fallback)
        }
        return kLedYellowBlink;                                      // MFS_LOOPER_PAUSED (has content, stopped)
    }
    int preset = gridPresetIndex(row, col);
    if (preset >= 0) {
        return grid.presetUsed(preset) ? kLedYellow : kLedOff;       // MFS_PRESET_USED / UNUSED
    }
    return kLedOff;   // cols 6-7: unassigned, always off (apcKey25Transpose.cpp:180-183)
}

} // namespace aloop
