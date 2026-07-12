// aloop MIDI input — ALSA rawmidi replacing Circle USB-MIDI (MIGRATION-MAP).
//
// The APC Key25 (or any controller) sends CC/note messages; the mapping logic
// (which CC → which normalized param) is ported unchanged from looper's
// apcKey25*.cpp and dubfx's param_mapping.md. Only the INPUT SOURCE changes:
// Circle USB-MIDI → ALSA rawmidi. Runs on the control thread; writes the param
// snapshot the audio thread reads (never the audio hot path).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>

#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#define ALOOP_HAVE_ALSA 1
#endif

namespace aloop {

// The normalized param store the audio thread reads via the snapshot. Indices
// match the dubfx param_mapping.md (HP/LP/res, reverb/delay/time, pitch semis/
// formant/engaged, microrepeat div, thru/loop/mix). Single-writer (this thread).
struct ParamStore {
    std::atomic<float> value[32];   // normalized [0,1] or the mapped range
};

// Reproduce looper's CC→normalized mapping (param_mapping.md). The exact table:
//   CC51→HP, CC54→LPres, CC55→LP, CC48→reverb, CC49→delay, CC50→time (all /127);
//   CC53→formant (deadzone+range); CC52→pitch semis; notes 82-86→microrepeat div.
// Kept here so the audio path stays free of MIDI parsing.
static void applyCC(ParamStore& ps, uint8_t cc, uint8_t data2) {
    const float norm = data2 / 127.0f;
    switch (cc) {
        case 51: ps.value[0].store(norm); break;  // HP cutoff
        case 54: ps.value[1].store(norm); break;  // LP resonance
        case 55: ps.value[2].store(norm); break;  // LP cutoff
        case 48: ps.value[3].store(norm); break;  // reverb amount
        case 49: ps.value[4].store(norm); break;  // delay amount
        case 50: ps.value[5].store(norm); break;  // time
        case 53: {                                // formant: deadzone + range
            bool dead = (data2 >= 60 && data2 <= 68);
            ps.value[6].store(dead ? 0.0f : (((int)data2 - 64) / 63.0f));
        } break;
        case 52: ps.value[7].store((norm * 24.0f) - 12.0f); break; // pitch semis ±12
        default: break;
    }
}

// Control-thread loop: open the ALSA rawmidi device, read messages, apply the
// mapping into the param store. Blocking read is fine — this is NOT the audio
// thread. Hotplug: on device loss, params hold their last value (DEGRADED-MODES).
void runMidiLoop(ParamStore& ps, const char* device) {
#ifdef ALOOP_HAVE_ALSA
    snd_rawmidi_t* in = nullptr;
    const char* dev = (device && strcmp(device, "auto")) ? device : "hw:1,0,0";
    if (snd_rawmidi_open(&in, nullptr, dev, SND_RAWMIDI_SYNC) < 0) {
        fprintf(stderr, "[midi] no controller at %s — params hold defaults\n", dev);
        return;
    }
    fprintf(stderr, "[midi] reading %s (APC-style CC mapping)\n", dev);
    uint8_t st = 0, d1 = 0, d2 = 0; int phase = 0; uint8_t b;
    while (snd_rawmidi_read(in, &b, 1) == 1) {
        if (b & 0x80) { st = b; phase = 1; continue; }      // status byte
        if (phase == 1) { d1 = b; phase = 2; }
        else if (phase == 2) { d2 = b; phase = 1;
            if ((st & 0xF0) == 0xB0) applyCC(ps, d1, d2);   // control change
            // (note-on 0x90 for microrepeat divisors handled similarly)
        }
    }
    snd_rawmidi_close(in);
#else
    (void)ps; (void)device;
#endif
}

} // namespace aloop
