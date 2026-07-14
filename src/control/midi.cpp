// aloop MIDI control — ALSA rawmidi input, driven by a REMAPPABLE control map
// (config/controls.conf). No hardcoded CC→control table: every binding comes
// from the config file, so the whole surface is re-mappable without recompiling.
//
// Flow: load the map (midi → target name) → read ALSA rawmidi → look up the
// incoming CC/note in the map → write the target's value into the ParamStore,
// keyed by TARGET NAME. The audio thread reads the store by name and sets the
// matching Faust control zone (looperN/rec, fx/hp, …).

#include "midi.h"
#include "apc_grid.h"
#include "apc_leds.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#include <poll.h>
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

static unsigned nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

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
    // Seed each fx/* target with its Faust zone's OWN compiled-in default
    // (dsp/effects_runtime.dsp) rather than a blanket 0.0 — see ParamStore::bind's
    // comment for why: a bound target whose zone default is non-zero would
    // otherwise be silently forced to 0.0 from the very first block, before any
    // MIDI event ever touches it (WITNESSED: fx/lp forced LPCUT to 0.0 = total
    // silence until the first physical knob turn). looper*/* and cmd/* targets
    // correctly default to 0.0 (unarmed/released), so they need no entry here.
    static const std::unordered_map<std::string, float> kFxDefaults = {
        {"fx/hp",      0.0f},   // HPCUT  0.0 = bypass (dsp/effects_runtime.dsp)
        {"fx/lpres",   0.0f},   // LPRES  0.0 = no resonance
        {"fx/lp",      1.0f},   // LPCUT  1.0 = fully open (bypass) -- THE bug
        {"fx/reverb",  0.0f},   // REVAMT 0.0 = dry
        {"fx/delay",   0.0f},   // DELAYAMT 0.0 = dry
        {"fx/time",    0.5f},   // TIME   0.5 = the Faust default
        {"fx/pitch",   0.0f},   // SEMIS  0.0 = unity
    };
    for (auto& kv : map) {
        auto d = kFxDefaults.find(kv.second);
        ps.bind(kv.second, d != kFxDefaults.end() ? d->second : 0.0f);
    }
    // ApcGrid's own targets (loopers, live-pitch, microrepeat, monitor-fold,
    // formant) are NOT in controls.conf's flat map -- pre-bind them here, once,
    // before any MIDI event can reach ApcGrid's dispatch methods. Those methods
    // must never call ps.bind() themselves: bind() takes bindMtx but
    // setByName/get/forEach do not, so a runtime bind() from the MIDI thread
    // would race the audio thread's unlocked forEach over the same map.
    ApcGrid::bindAll(ps);

#ifdef ALOOP_HAVE_ALSA
    // Open BOTH directions on the same device: the APC Key25's USB MIDI is one
    // bidirectional endpoint (../looper usbMidi.cpp sends LED updates back out
    // the SAME connection input arrives on, via SendPlainMIDI — no separate
    // MIDI OUT device exists). `out` may legitimately fail to open even when
    // `in` succeeds (a non-APC controller with no LEDs, or a device that only
    // exposes a capture-only rawmidi substream) — LED output is best-effort:
    // aloop still functions fully for control (the actual bug this whole
    // module exists to fix was buttons doing nothing INTERNALLY, which is a
    // separate, now-fixed issue; missing LEDs on non-APC hardware is expected,
    // not an error).
    snd_rawmidi_t* in = nullptr;
    snd_rawmidi_t* out = nullptr;
    char devbuf[16] = {0};
    // snd_rawmidi_open(&in, &out, ...) only succeeds if BOTH substreams open —
    // a device with a capture-only rawmidi substream (no OUT at all) would
    // fail the combined call even though the input-only open (the previous,
    // working behavior) would have succeeded. Try combined first (gets LEDs
    // when available); on failure retry input-only so a device that just
    // lacks MIDI OUT doesn't lose CONTROL entirely for the sake of LEDs.
    if (device && strcmp(device, "auto")) {
        // explicit device given (config/arg) — use it verbatim.
        snprintf(devbuf, sizeof devbuf, "%s", device);
        if (snd_rawmidi_open(&in, &out, devbuf, SND_RAWMIDI_SYNC) < 0) {
            in = nullptr; out = nullptr;
            if (snd_rawmidi_open(&in, nullptr, devbuf, SND_RAWMIDI_SYNC) < 0) {
                fprintf(stderr, "[midi] no controller at %s — params hold\n", devbuf);
                return;
            }
        }
    } else {
        // auto: SCAN for the first rawmidi input. The USB MIDI controller's ALSA
        // card number is NOT guaranteed (the f_uac2 gadget + USB audio also take
        // cards), so we probe hw:0..7,0,0 rather than assume card 1.
        for (int card = 0; card < 8 && !in; card++) {
            snprintf(devbuf, sizeof devbuf, "hw:%d,0,0", card);
            if (snd_rawmidi_open(&in, &out, devbuf, SND_RAWMIDI_SYNC) == 0) break;
            in = nullptr; out = nullptr;
            if (snd_rawmidi_open(&in, nullptr, devbuf, SND_RAWMIDI_SYNC) == 0) break;
            in = nullptr;
        }
        if (!in) {
            fprintf(stderr, "[midi] no MIDI input found (probed hw:0..7) — params hold\n");
            return;
        }
    }
    fprintf(stderr, "[midi] reading %s (remappable control map + APC grid engine)%s\n",
            devbuf, out ? " + LED output" : " (no MIDI OUT on this device — no LED feedback)");
    ApcGrid grid;
    ApcLeds leds;
    auto ledWrite = [&](int note, uint8_t vel) -> bool {
        if (!out) return false;
        uint8_t msg[3] = { 0x90, (uint8_t)note, vel };   // Note On, channel 0 (looper: usbMidi.cpp)
        return snd_rawmidi_write(out, msg, 3) == 3;
    };
    // Hold-duration polling (erase >=1s, preset capture >=1s) must run on a
    // WALL-CLOCK tick, not on MIDI-message arrival — a held pad with no other
    // MIDI traffic in flight would otherwise never resolve (snd_rawmidi_read is
    // SND_RAWMIDI_SYNC/blocking with no timeout; looper's own apcKey25::update()
    // is likewise driven from the audio thread's periodic tick, not from MIDI
    // events — audio.cpp:423). Use poll() on the rawmidi fd(s) with a 100ms
    // timeout so pollHolds() runs regularly even with the port idle.
    int nfds = snd_rawmidi_poll_descriptors_count(in);
    std::vector<struct pollfd> pfds((size_t)(nfds > 0 ? nfds : 1));
    if (nfds > 0) snd_rawmidi_poll_descriptors(in, pfds.data(), (unsigned)nfds);
    uint8_t st = 0, d1 = 0, d2 = 0; int phase = 0; uint8_t b;
    for (;;) {
        int pr = (nfds > 0) ? poll(pfds.data(), (nfds_t)nfds, 100) : 100;
        if (pr == 0) {
            unsigned n = nowMs();
            grid.pollHolds(n, ps);   // timeout: no MIDI, just poll holds
            leds.refresh(n, grid, grid.liveEngaged(), ledWrite);
            continue;
        }
        if (pr < 0) {
            unsigned n = nowMs();
            grid.pollHolds(n, ps);   // interrupted/error: keep polling holds, retry read
            leds.refresh(n, grid, grid.liveEngaged(), ledWrite);
            continue;
        }
        if (snd_rawmidi_read(in, &b, 1) != 1) break;
        // Diagnostic: only log NOTE-related messages (0x8x/0x9x status), so CC
        // traffic from working knobs doesn't drown out the button-press bytes
        // we actually need to see. Capped generously since note events are rare
        // compared to CC streams.
        static uint64_t noteLogCount = 0;
        bool isNoteStatusByte = (b & 0x80) && ((b & 0xF0) == 0x80 || (b & 0xF0) == 0x90);
        bool loggingThisMsg = isNoteStatusByte || (phase == 2 && ((st & 0xF0) == 0x80 || (st & 0xF0) == 0x90));
        if (loggingThisMsg && noteLogCount < 500) { fprintf(stderr, "[midi] note raw byte: 0x%02x (phase=%d)\n", b, phase); noteLogCount++; }
        if (b & 0x80) { st = b; phase = 1; continue; }
        if (phase == 1) { d1 = b; phase = 2; continue; }
        // phase 2: full message
        d2 = b; phase = 1;
        uint8_t type = st & 0xF0;
        uint8_t channel = st & 0x0F;
        unsigned now = nowMs();
        if ((type == 0x80 || type == 0x90) && noteLogCount < 500)
            fprintf(stderr, "[midi] note decoded: st=0x%02x type=0x%02x ch=%d d1=%d d2=%d\n", st, type, channel, d1, d2);
        grid.pollHolds(now, ps);   // also check on every real event, for prompt response
        leds.refresh(now, grid, grid.liveEngaged(), ledWrite);   // self-throttled to ~30Hz internally

        // --- real APC Key25 hardware surface (apcKey25.cpp/apcKey25Notes.cpp), channel 0 only ---
        if (channel == 0) {
            // SHIFT (apcKey25.cpp:96,185): channel-0-only guard is this `if
            // (channel == 0)` block itself -- the keybed's channel-1 note 98 never
            // reaches here, so it is never mistaken for SHIFT (apcKey25.cpp:91-95).
            if (d1 == kApcBtnShift) {
                if (type == 0x90 && d2 > 0) { grid.onShiftPress(ps); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onShiftRelease(ps); continue; }
            }
            if (d1 == kApcLiveLedNote && type == 0x90 && d2 > 0) { grid.onLiveEngageToggle(ps); continue; }  // note 64: live-pitch master engage toggle ("transpose on/off")
            if (type == 0xB0 && d1 == 1)  { grid.onModWheel(d2, ps); continue; }       // CC1 mod-wheel live-pitch
            if (type == 0xB0 && d1 == 52) { grid.onAbsolutePitch(d2, ps); continue; }  // CC52 absolute live-pitch
            if (type == 0xB0 && d1 == 53) { grid.onFormantCC(d2, ps); continue; }      // CC53 formant (deadzone + SHIFT range)
            if (d1 >= 82 && d1 <= 86) {                                                // microrepeat latch notes
                if (type == 0x90 && d2 > 0) { grid.onMicrorepeatOn((int)d1, ps); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onMicrorepeatOff((int)d1, ps); continue; }
            }
            if (d1 < kApcRows * kApcCols) {                                            // 5x8 pad grid
                if (type == 0x90 && d2 > 0) { grid.onPadPress((int)d1, now, ps); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onPadRelease((int)d1, now, ps); continue; }
            }
        }

        // --- everything else (filters, transport buttons, speed) via the flat remap ---
        uint32_t key = 0; float val = 0;
        if (type == 0xB0) { key = midiKey(false, d1); val = d2 / 127.0f; }         // CC
        else if (type == 0x90 && d2 > 0) { key = midiKey(true, d1); val = 1.0f; }  // note-on
        else if (type == 0x80 || (type == 0x90 && d2 == 0)) { key = midiKey(true, d1); val = 0.0f; } // note-off
        else continue;
        auto it = map.find(key);
        if (it != map.end()) ps.setByName(it->second, val);   // apply per the MAP
    }
    snd_rawmidi_close(in);
    if (out) snd_rawmidi_close(out);
#else
    (void)ps;
#endif
}

} // namespace aloop
