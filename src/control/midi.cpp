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
#include "../dsp/audio_thread.h"
#include "../link/link_bridge.h"

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

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

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

void runMidiLoop(ParamStore& ps, const char* device, AudioThread* audio, LinkBridge* link) {
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
    // Synthetic-input injection socket (TCP :9401, localhost+LAN): accepts
    // raw MIDI bytes and feeds them into the EXACT SAME byte-parsing state
    // machine below as real hardware input. Built to close a real gap found
    // in this project's own history: every hardware-input bug (APC grid
    // buttons, the blank-loop-after-erase investigation) could only be
    // reproduced by asking a human to physically press pads on the real
    // controller, because there was no way to inject a synthetic
    // note-on/note-off byte sequence into this exact dispatch path -- a raw
    // write() to the ALSA rawmidi device node itself does NOT work as an
    // injection channel (WITNESSED: it targets the USB MIDI OUT endpoint
    // toward the physical controller, and blocks/hangs with no receiver;
    // confirmed live via `timeout 3 sh -c "printf ... > /dev/snd/midiC1D0"`
    // hanging until killed). TCP (not a Unix socket) specifically because
    // this device's shell has no `nc -U`/`socat` to bridge stdin to a Unix
    // socket, and the workstation already reaches the device directly over
    // the LAN for SSH -- a plain TCP connect needs no on-device relay tool
    // at all, just `node test/hardware/midi-inject.js <host> <bytes...>`.
    // This socket joins THIS thread's poll() set alongside the real rawmidi
    // fd; any byte read from it is parsed by the identical state machine
    // (st/d1/d2/phase) real MIDI bytes go through, so a scripted
    // reproduction is indistinguishable from a real button press to every
    // downstream consumer (ApcGrid, the LED refresh, telemetry).
    // Best-effort: a bind/listen failure here must never prevent real MIDI
    // control from working, so failures just leave injection unavailable.
    int injectListenFd = -1, injectConnFd = -1;
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd >= 0) {
            int yes = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(9401);
            if (bind(fd, (sockaddr*)&addr, sizeof addr) == 0 && listen(fd, 1) == 0) {
                injectListenFd = fd;
                fprintf(stderr, "[midi] injection socket listening on tcp/9401 (synthetic MIDI bytes for scripted reproduction)\n");
            } else {
                close(fd);
            }
        }
    }
    // Fixed pollfd layout: [0..nfds) = real rawmidi fds, [nfds] = injection
    // listen socket (always present if it opened; harmlessly polls -1
    // otherwise, which poll() ignores), [nfds+1] = the current injection
    // connection (-1/ignored when no client is connected).
    int realNfds = snd_rawmidi_poll_descriptors_count(in);
    int nfds = realNfds > 0 ? realNfds : 1;
    const int kListenSlot = nfds;
    const int kConnSlot = nfds + 1;
    std::vector<struct pollfd> pfds((size_t)(nfds + 2));
    if (realNfds > 0) {
        snd_rawmidi_poll_descriptors(in, pfds.data(), (unsigned)realNfds);
    } else {
        pfds[0].fd = -1;   // no real descriptor available -- poll() ignores fd<0 entries
        pfds[0].events = 0;
    }
    uint8_t st = 0, d1 = 0, d2 = 0; int phase = 0; uint8_t b;
    for (;;) {
        pfds[(size_t)kListenSlot].fd = injectListenFd;
        pfds[(size_t)kListenSlot].events = POLLIN;
        pfds[(size_t)kConnSlot].fd = injectConnFd;
        pfds[(size_t)kConnSlot].events = POLLIN;
        int pr = poll(pfds.data(), (nfds_t)pfds.size(), 100);
        if (pr > 0 && injectListenFd >= 0 && (pfds[(size_t)kListenSlot].revents & POLLIN)) {
            int c = accept(injectListenFd, nullptr, nullptr);
            if (c >= 0) {
                if (injectConnFd >= 0) close(injectConnFd);   // one injector at a time
                injectConnFd = c;
            }
        }
        bool gotInjectedByte = false;
        if (pr > 0 && injectConnFd >= 0 && (pfds[(size_t)kConnSlot].revents & (POLLIN | POLLHUP))) {
            uint8_t ib;
            ssize_t rr = read(injectConnFd, &ib, 1);
            if (rr == 1) { b = ib; gotInjectedByte = true; }
            else { close(injectConnFd); injectConnFd = -1; }
        }
        bool realReady = false;
        for (int i = 0; i < nfds; i++) if (pfds[(size_t)i].revents & POLLIN) { realReady = true; break; }
        if (!gotInjectedByte) {
            if (pr == 0) {
                unsigned n = nowMs();
                grid.pollHolds(n, ps);   // timeout: no MIDI, just poll holds
                auto t = audio ? audio->snapshotTelemetry() : AudioThread::Telemetry{};
                leds.refresh(n, grid, grid.liveEngaged(), ledWrite, audio ? t.looperLevel : nullptr);
                continue;
            }
            if (pr < 0) {
                unsigned n = nowMs();
                grid.pollHolds(n, ps);   // interrupted/error: keep polling holds, retry read
                auto t = audio ? audio->snapshotTelemetry() : AudioThread::Telemetry{};
                leds.refresh(n, grid, grid.liveEngaged(), ledWrite, audio ? t.looperLevel : nullptr);
                continue;
            }
            if (!realReady) continue;   // only the listen socket (or nothing) was ready
            if (snd_rawmidi_read(in, &b, 1) != 1) break;
        }
        // `b` now holds either a real rawmidi byte or an injected synthetic
        // byte -- both flow through the identical parse/dispatch below.
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
        {
            auto t = audio ? audio->snapshotTelemetry() : AudioThread::Telemetry{};
            leds.refresh(now, grid, grid.liveEngaged(), ledWrite, audio ? t.looperLevel : nullptr);   // self-throttled to ~30Hz internally
        }

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
            // Sampler record-arm buttons (apcKey25.cpp:157-168,198-208), built
            // per explicit request -- previously entirely unimplemented
            // (docs/DECISIONS.md ADR-012). 65 = chromatic record, 66 = arm
            // drum-record-mode (gates channel-1 key routing below).
            if (d1 == 65) {
                if (type == 0x90 && d2 > 0) { grid.onSamplerBtn65Press(audio ? audio->sampler() : nullptr); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onSamplerBtn65Release(audio ? audio->sampler() : nullptr); continue; }
            }
            if (d1 == 66) {
                if (type == 0x90 && d2 > 0) { grid.onSamplerBtn66Press(); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onSamplerBtn66Release(audio ? audio->sampler() : nullptr); continue; }
            }
            if (d1 < kApcRows * kApcCols) {                                            // 5x8 pad grid
                if (type == 0x90 && d2 > 0) { grid.onPadPress((int)d1, now, ps, link); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onPadRelease((int)d1, now, ps, link); continue; }
            }
            // SHIFT-gated transport button reroute (apcKey25Notes.cpp:170-175):
            // STOP_ALL (note 0x51/81) unshifted = quantized stop (aloop: the
            // existing cmd/stopall flat binding below); shift+STOP_ALL =
            // IMMEDIATE stop that ALSO aborts any in-progress recording
            // (looper's LOOP_COMMAND_STOP_IMMEDIATE) -- previously aloop had
            // no shift branch at all on this button, so "shift button
            // rerouting didnt work" for transport controls specifically.
            // shift+PLAY = LOOP_IMMEDIATE, which aloop's Faust feedback-delay
            // engine has no addressable read head for (ADR-010/
            // docs/COMMAND-SURFACE.md, a deliberate model difference) -- so
            // shift+PLAY has nothing to reroute TO yet and is left unbound
            // rather than silently doing the wrong thing.
            if (type == 0x90 && d2 > 0 && d1 == 0x51 && grid.shiftHeld()) { grid.onStopImmediate(ps); continue; }
            // PLAY (note 0x5B/91) = CLEAR_ALL. Previously routed ONLY through
            // controls.conf's flat note91->cmd/clearall binding, which
            // ApcGrid never observed -- its own shadow state never got reset
            // on clear, breaking every subsequent recording attempt
            // (WITNESSED live: "doing a new set didn't work" after clearing).
            // Now intercepted here so ApcGrid::onClearAll can reset its state
            // in the same call that wipes the DSP-side content.
            //
            // WITNESSED bug (2nd generation): this used to be gated on
            // `!grid.shiftHeld()`, matching looper's documented shift+PLAY =
            // loop-immediate intent -- but loop-immediate is NOT wired (no
            // addressable read head, ADR-010), so a PLAY press that happened
            // to land while SHIFT was still held (e.g. mid-release of an
            // adjacent gesture) fell through this whole channel-0 block
            // entirely and hit controls.conf's flat map, which (before that
            // line was removed) set cmd/clearall directly without ever
            // calling onClearAll -- silently desyncing ApcGrid's shadow
            // state from the DSP's actual (wiped) content, producing a
            // "blank recording" on the very next press with no observable
            // clear-all event in ApcGrid's own log. CLEAR_ALL must always be
            // reachable regardless of shift state until loop-immediate is
            // actually implemented.
            if (d1 == 0x5B) {
                if (type == 0x90 && d2 > 0) { grid.onClearAll(true, ps); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onClearAll(false, ps); continue; }
            }
        }

        // channel 1 (keybed): looper's apcKey25.cpp:103-125 -- any keybed key
        // press engages live-pitch at that key's own semitone offset (or
        // triggers the sampler if it has content for that key). Was
        // completely unhandled (aloop only ever inspected channel 0),
        // directly explaining "keys didnt arm transpose".
        if (channel == 1) {
            if (type == 0x90 && d2 > 0) { grid.onKeybedNoteOn((int)d1, ps, audio ? audio->sampler() : nullptr); continue; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onKeybedNoteOff((int)d1, audio ? audio->sampler() : nullptr); continue; }
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
