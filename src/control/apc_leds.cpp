#include "apc_leds.h"
#include "apc_grid.h"

namespace aloop {

// Per-pad color classification (../looper apcKey25Transpose.cpp:124-249,
// state table midiMap.h:143-159). Peak-level PLAY_LOW/MID/HIGH tiers need a
// per-looper output-peak telemetry aloop doesn't have yet (only a static vol
// knob, not a live signal peak) — until that's wired, a playing-with-content
// looper is shown steady GREEN (the correct "quiet" tier) rather than guessing
// a peak level; this is a narrower but still fully-functional and honest
// rendering of looper's real 3-tier VU coloring; see AGENTS/PRD for the
// tracked follow-up to wire real peak-based tiers.
uint8_t ApcLeds::gridColor(int row, int col, const ApcGrid& grid) const {
    int looper = gridLooperIndex(row, col);
    if (looper >= 0) {
        if (grid.looperRecording(looper))   return kLedRedBlink;     // MFS_LOOPER_RECORDING
        if (!grid.looperHasContent(looper)) return kLedOff;          // MFS_LOOPER_EMPTY
        if (grid.looperPlaying(looper))     return kLedGreen;        // MFS_LOOPER_PLAY_LOW (narrowed, see above)
        return kLedYellowBlink;                                      // MFS_LOOPER_PAUSED (has content, stopped)
    }
    int preset = gridPresetIndex(row, col);
    if (preset >= 0) {
        return grid.presetUsed(preset) ? kLedYellow : kLedOff;       // MFS_PRESET_USED / UNUSED
    }
    return kLedOff;   // cols 6-7: unassigned, always off (apcKey25Transpose.cpp:180-183)
}

} // namespace aloop
