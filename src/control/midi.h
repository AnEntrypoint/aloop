// aloop MIDI control — public interface (impl in midi.cpp).
// ALSA rawmidi input + the APC CC/note mapping into a lock-free param store the
// audio thread reads. Runs on the control thread.

#ifndef ALOOP_MIDI_H
#define ALOOP_MIDI_H

#include <atomic>

namespace aloop {

// The normalized param store (single-writer = the MIDI thread; readers = audio).
// Indices follow dubfx param_mapping.md: [0]HP [1]LPres [2]LP [3]reverb [4]delay
// [5]time [6]formant [7]pitchSemis ... plus loop commands.
struct ParamStore {
    std::atomic<float> value[32];
    std::atomic<int>   command{0};   // edge-triggered loop command (record/play/…)
    ParamStore() { for (auto& v : value) v.store(0.0f); }
};

// Blocking control-thread loop: read the ALSA rawmidi device, apply the mapping
// into `ps`. `device` = "auto" or an ALSA name. Returns on device close.
void runMidiLoop(ParamStore& ps, const char* device);

} // namespace aloop
#endif // ALOOP_MIDI_H
