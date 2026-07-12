// aloop MIDI control — ALSA rawmidi input, driven by a REMAPPABLE control map
// (config/controls.conf). No hardcoded CC→control table: every binding comes
// from the config file, so the whole surface is re-mappable without recompiling.
//
// Flow: load the map (midi → target name) → read ALSA rawmidi → look up the
// incoming CC/note in the map → write the target's value into the ParamStore,
// keyed by TARGET NAME. The audio thread reads the store by name and sets the
// matching Faust control zone (looperN/rec, fx/hp, …).

#include "midi.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>

#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#define ALOOP_HAVE_ALSA 1
#endif

namespace aloop {

// Parse "cc51" / "note20" / "cc51.2" into (isNote, number, channel or -1).
static bool parseMidiKey(const std::string& s, bool& isNote, int& num, int& ch) {
    ch = -1;
    std::string body = s;
    auto dot = s.find('.');
    if (dot != std::string::npos) { ch = atoi(s.c_str() + dot + 1); body = s.substr(0, dot); }
    if (body.rfind("cc", 0) == 0)      { isNote = false; num = atoi(body.c_str() + 2); return true; }
    if (body.rfind("note", 0) == 0)    { isNote = true;  num = atoi(body.c_str() + 4); return true; }
    return false;
}

// The loaded map: a MIDI event key → the target control name (e.g. "looper3/rec").
// Key packs (isNote<<16 | num<<1 | anychannel) — simple + fast.
static uint32_t midiKey(bool isNote, int num) { return ((uint32_t)isNote << 8) | (uint32_t)(num & 0xFF); }

void runMidiLoop(ParamStore& ps, const char* device) {
    // --- load the remappable control map ---
    std::unordered_map<uint32_t, std::string> map;
    const char* mapPath = "/etc/aloop-controls.conf";
    FILE* mf = fopen(mapPath, "r");
    if (!mf) mf = fopen("config/controls.conf", "r");   // dev fallback
    if (mf) {
        char line[256];
        while (fgets(line, sizeof line, mf)) {
            char midi[64], target[128];
            if (line[0] == '#' || line[0] == '\n') continue;
            if (sscanf(line, " %63s %127s", midi, target) == 2) {
                bool isNote; int num, ch;
                if (parseMidiKey(midi, isNote, num, ch))
                    map[midiKey(isNote, num)] = target;
            }
        }
        fclose(mf);
        fprintf(stderr, "[midi] loaded %zu control bindings from %s\n", map.size(), mapPath);
    } else {
        fprintf(stderr, "[midi] no control map — controls unbound until %s exists\n", mapPath);
    }

    // Publish each binding's target into the store's name index so the audio
    // thread knows which Faust zones to set. (ParamStore holds a name→value map.)
    for (auto& kv : map) ps.bind(kv.second);

#ifdef ALOOP_HAVE_ALSA
    snd_rawmidi_t* in = nullptr;
    const char* dev = (device && strcmp(device, "auto")) ? device : "hw:1,0,0";
    if (snd_rawmidi_open(&in, nullptr, dev, SND_RAWMIDI_SYNC) < 0) {
        fprintf(stderr, "[midi] no controller at %s — params hold\n", dev);
        return;
    }
    fprintf(stderr, "[midi] reading %s (remappable control map)\n", dev);
    uint8_t st = 0, d1 = 0, d2 = 0; int phase = 0; uint8_t b;
    while (snd_rawmidi_read(in, &b, 1) == 1) {
        if (b & 0x80) { st = b; phase = 1; continue; }
        if (phase == 1) { d1 = b; phase = 2; continue; }
        // phase 2: full message
        d2 = b; phase = 1;
        uint8_t type = st & 0xF0;
        uint32_t key = 0; float val = 0;
        if (type == 0xB0) { key = midiKey(false, d1); val = d2 / 127.0f; }         // CC
        else if (type == 0x90 && d2 > 0) { key = midiKey(true, d1); val = 1.0f; }  // note-on
        else if (type == 0x80 || (type == 0x90 && d2 == 0)) { key = midiKey(true, d1); val = 0.0f; } // note-off
        else continue;
        auto it = map.find(key);
        if (it != map.end()) ps.setByName(it->second, val);   // apply per the MAP
    }
    snd_rawmidi_close(in);
#else
    (void)ps;
#endif
}

} // namespace aloop
