#ifndef ALOOP_APC_LEDS_H
#define ALOOP_APC_LEDS_H

#include <cstdint>
#include <array>
#include "apc_grid.h"

namespace aloop {

enum ApcLedVel : uint8_t {
    kLedOff         = 0,
    kLedGreen       = 1,
    kLedGreenBlink  = 2,
    kLedRed         = 3,
    kLedRedBlink    = 4,
    kLedYellow      = 5,
    kLedYellowBlink = 6,
};

constexpr int kApcBtnStopAll = 0x51;
constexpr int kApcBtnPlay    = 0x5B;
constexpr int kApcLiveLedNote = 0x40;

class ApcLeds {
public:
    template <typename WriteFn>
    void refresh(unsigned now_ms, const ApcGrid& grid, bool liveEngaged, WriteFn&& write, const float* looperLevels = nullptr, int gridBeatIndex = -1) {
        if (!bootMs_) bootMs_ = now_ms ? now_ms : 1;
        if (now_ms - bootMs_ < kBootDelayMs) return;
        if (now_ms - lastMs_ < kRefreshMs) return;
        lastMs_ = now_ms;

        for (int row = 0; row < kApcRows; row++) {
            for (int col = 0; col < kApcCols; col++) {
                int note = row * kApcCols + col;
                sendCoalesced(note, gridColor(row, col, grid, looperLevels), write);
            }
        }
        {
            static constexpr uint8_t kBeatDigitColors[4] = { kLedOff, kLedYellow, kLedRed, kLedGreen };
            static constexpr int kBeatPadNotes[4] = { 15, 23, 31, 39 };
            int activeGroup = gridBeatIndex >= 0 ? (gridBeatIndex >> 2) : -1;
            int posInGroup  = gridBeatIndex >= 0 ? (gridBeatIndex & 0x3) : 0;
            for (int g = 0; g < 4; g++) {
                uint8_t color;
                if (activeGroup < 0 || g > activeGroup)      color = kLedOff;
                else if (g < activeGroup)                    color = kLedGreen;
                else                                         color = kBeatDigitColors[posInGroup];
                sendCoalesced(kBeatPadNotes[g], color, write);
            }
        }
        sendCoalesced(kApcBtnPlay, grid.shiftHeld() ? kLedYellow : kLedOff, write);
        sendCoalesced(kApcLiveLedNote, liveEngaged ? 127 : 0, write);
        {
            bool flash = grid.bankFlashActive();
            FxBank which = grid.bankFlashWhich();
            sendCoalesced(kApcBtnDubFx,    (flash && which == FxBank::Dub)    ? kLedGreen : kLedOff, write);
            sendCoalesced(kApcBtnGuitarFx, (flash && which == FxBank::Guitar) ? kLedGreen : kLedOff, write);
        }
        sendCoalesced(kApcBtnLofiFx,
            grid.objektEngaged()     ? kLedRedBlink :
            grid.granulatorHeld()    ? kLedGreenBlink :
            grid.granulatorLatched() ? kLedGreen :
                                        kLedOff,
            write);
    }

    void invalidate() { cacheValid_.fill(false); }

private:
    static constexpr unsigned kBootDelayMs = 2000;
    static constexpr unsigned kRefreshMs = 33;

    unsigned bootMs_ = 0;
    unsigned lastMs_ = 0;
    std::array<uint8_t, 128> cache_{};
    std::array<bool, 128> cacheValid_{};

    template <typename WriteFn>
    void sendCoalesced(int note, uint8_t velocity, WriteFn&& write) {
        if (cacheValid_[note] && cache_[note] == velocity) return;
        if (write(note, velocity)) { cache_[note] = velocity; cacheValid_[note] = true; }
    }

    uint8_t gridColor(int row, int col, const ApcGrid& grid, const float* looperLevels) const;
};

}
#endif
