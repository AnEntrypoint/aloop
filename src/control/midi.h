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

class AudioThread;   // dsp/audio_thread.h -- level telemetry for the LED VU meter

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
    // `defaultVal` seeds the slot's initial value — CRITICAL for any target
    // whose Faust zone has a non-zero compiled-in default (e.g. fx/lp's
    // LPCUT defaults to 1.0 = fully open, fx/time's TIME defaults to 0.5).
    // WITNESSED live: worker() pushes g_params->get(target) into the
    // matching Faust zone EVERY block, unconditionally, starting from the
    // very first block — before any MIDI event has ever touched that
    // target. With the old blanket value{}=0.0f init, this meant every
    // bound fx/* control silently OVERWROTE the Faust program's own default
    // with 0.0 at startup: fx/lp forced LPCUT to 0.0 (fully closed low-pass
    // = total silence) until the very first physical knob turn sent a real
    // CC value — exactly matching the "no sound until I touched the
    // lowpass knob" symptom. Binding with the correct default closes this
    // gap for every future control, not just the one that happened to be
    // reported.
    void bind(const std::string& name, float defaultVal = 0.0f) {
        std::lock_guard<std::mutex> g(bindMtx);
        if (slot.find(name) == slot.end() && count < MAX) {
            int idx = count++;
            slot[name] = idx;
            value[idx].store(defaultVal, std::memory_order_relaxed);
        }
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
// each event per the map into `ps`. `device` = "auto" or an ALSA name. `audio`
// (may be null) lets the LED refresh read live per-looper output levels for
// the APC grid's VU-meter coloring; null just means level-based coloring
// degrades to the has-content/playing/paused tiers without loudness detail.
void runMidiLoop(ParamStore& ps, const char* device, class AudioThread* audio = nullptr);

} // namespace aloop
#endif // ALOOP_MIDI_H
