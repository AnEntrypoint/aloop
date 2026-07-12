// aloop MIDI control — public interface (impl in midi.cpp).
// ALSA rawmidi input + a REMAPPABLE control map (config/controls.conf) into a
// name-keyed lock-free-ish param store the audio thread reads by target name.

#ifndef ALOOP_MIDI_H
#define ALOOP_MIDI_H

#include <atomic>
#include <array>
#include <string>
#include <unordered_map>
#include <mutex>

namespace aloop {

// Name-keyed control store. Targets are the control names the map binds to
// ("looper3/rec", "fx/hp", "cmd/clearall", …). The MIDI thread writes values by
// name; the audio thread reads by name to set the matching Faust control zone.
//
// The set of names is fixed at startup (bind() during map load), so the value
// array never resizes at runtime — reads/writes are plain atomics indexed by a
// slot resolved once. This keeps the audio-thread read lock-free.
struct ParamStore {
    static constexpr int MAX = 256;
    std::array<std::atomic<float>, MAX> value;
    // name → slot index, populated by bind() at startup (before the audio thread
    // reads). After startup this map is read-only, so name→slot lookup is safe.
    std::unordered_map<std::string, int> slot;
    std::mutex bindMtx;            // guards slot during startup binding only
    int count = 0;

    ParamStore() { for (auto& v : value) v.store(0.0f); }

    // Register a target name → a slot (idempotent). Called during map load.
    void bind(const std::string& name) {
        std::lock_guard<std::mutex> g(bindMtx);
        if (slot.find(name) == slot.end() && count < MAX) slot[name] = count++;
    }
    // Write a value by target name (MIDI thread). No-op if unbound.
    void setByName(const std::string& name, float v) {
        auto it = slot.find(name);
        if (it != slot.end()) value[it->second].store(v, std::memory_order_relaxed);
    }
    // Read a value by target name (audio thread). Returns def if unbound.
    float get(const std::string& name, float def = 0.0f) const {
        auto it = slot.find(name);
        return it != slot.end() ? value[it->second].load(std::memory_order_relaxed) : def;
    }
    // Iterate bound names (audio thread uses this once to map names→Faust zones).
    template <typename F> void forEach(F&& f) const { for (auto& kv : slot) f(kv.first, kv.second); }
};

// Blocking control-thread loop: load the remappable map, read ALSA rawmidi, apply
// each event per the map into `ps`. `device` = "auto" or an ALSA name.
void runMidiLoop(ParamStore& ps, const char* device);

} // namespace aloop
#endif // ALOOP_MIDI_H
