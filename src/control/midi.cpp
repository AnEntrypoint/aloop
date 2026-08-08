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
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

namespace aloop {

static bool parseMidiKey(const std::string& s, bool& isNote, int& num, int& ch) {
    ch = -1;
    std::string body = s;
    auto dot = s.find('.');
    if (dot != std::string::npos) { ch = atoi(s.c_str() + dot + 1); body = s.substr(0, dot); }
    if (body.rfind("cc", 0) == 0)      { isNote = false; num = atoi(body.c_str() + 2); return true; }
    if (body.rfind("note", 0) == 0)    { isNote = true;  num = atoi(body.c_str() + 4); return true; }
    return false;
}

static uint32_t midiKey(bool isNote, int num) { return ((uint32_t)isNote << 8) | (uint32_t)(num & 0xFF); }

static unsigned nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void runMidiLoop(ParamStore& ps, const char* device, AudioThread* audio, LinkBridge* link) {
    std::unordered_map<uint32_t, std::string> map;
    const char* mapPath = "/etc/aloop-controls.conf";
    FILE* mf = fopen(mapPath, "r");
    if (!mf) mf = fopen("config/controls.conf", "r");
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

    static const std::unordered_map<std::string, float> kFxDefaults = {
        {"fx/hp",      0.0f},
        {"fx/lpres",   0.0f},
        {"fx/lp",      1.0f},
        {"fx/reverb",  0.0f},
        {"fx/delay",   0.0f},
        {"fx/time",    0.5f},
        {"fx/pitch",   0.0f},
    };
    for (auto& kv : map) {
        auto d = kFxDefaults.find(kv.second);
        ps.bind(kv.second, d != kFxDefaults.end() ? d->second : 0.0f);
    }
    ApcGrid::bindAll(ps);

#ifdef ALOOP_HAVE_ALSA
    ApcGrid grid;
    ApcLeds leds;
    snd_rawmidi_t* out = nullptr;
    auto ledWrite = [&](int note, uint8_t vel) -> bool {
        if (!out) return false;
        uint8_t msg[3] = { 0x90, (uint8_t)note, vel };
        return snd_rawmidi_write(out, msg, 3) == 3;
    };
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
    bool warnedNoDevice = false;
    for (;;) {
    snd_rawmidi_t* in = nullptr;
    out = nullptr;
    char devbuf[16] = {0};
    if (device && strcmp(device, "auto")) {
        snprintf(devbuf, sizeof devbuf, "%s", device);
        if (snd_rawmidi_open(&in, &out, devbuf, SND_RAWMIDI_SYNC) < 0) {
            in = nullptr; out = nullptr;
            snd_rawmidi_open(&in, nullptr, devbuf, SND_RAWMIDI_SYNC);
        }
    } else {
        for (int card = 0; card < 8 && !in; card++) {
            snprintf(devbuf, sizeof devbuf, "hw:%d,0,0", card);
            if (snd_rawmidi_open(&in, &out, devbuf, SND_RAWMIDI_SYNC) == 0) break;
            in = nullptr; out = nullptr;
            if (snd_rawmidi_open(&in, nullptr, devbuf, SND_RAWMIDI_SYNC) == 0) break;
            in = nullptr;
        }
    }
    if (!in) {
        if (!warnedNoDevice) {
            fprintf(stderr, "[midi] no MIDI input found (probed hw:0..7) — params hold, will keep rescanning every 2s until one appears\n");
            warnedNoDevice = true;
        }
        struct timespec ts{2, 0};
        nanosleep(&ts, nullptr);
        continue;
    }
    warnedNoDevice = false;
    fprintf(stderr, "[midi] reading %s (remappable control map + APC grid engine)%s\n",
            devbuf, out ? " + LED output" : " (no MIDI OUT on this device — no LED feedback)");
    int realNfds = snd_rawmidi_poll_descriptors_count(in);
    int nfds = realNfds > 0 ? realNfds : 1;
    const int kListenSlot = nfds;
    const int kConnSlot = nfds + 1;
    std::vector<struct pollfd> pfds((size_t)(nfds + 2));
    if (realNfds > 0) {
        snd_rawmidi_poll_descriptors(in, pfds.data(), (unsigned)realNfds);
    } else {
        pfds[0].fd = -1;
        pfds[0].events = 0;
    }
    uint8_t st = 0, d1 = 0, d2 = 0; int phase = 0; uint8_t b;
    char probeStatPath[32] = {0};
    {
        int probeCard = -1;
        if (sscanf(devbuf, "hw:%d,", &probeCard) == 1)
            snprintf(probeStatPath, sizeof probeStatPath, "/dev/snd/midiC%dD0", probeCard);
    }
    bool haveProbeStatPath = probeStatPath[0] != '\0';
    constexpr unsigned kLivenessProbeMs = 1000;
    unsigned lastLivenessProbeMs = nowMs();
    for (;;) {
        pfds[(size_t)kListenSlot].fd = injectListenFd;
        pfds[(size_t)kListenSlot].events = POLLIN;
        pfds[(size_t)kConnSlot].fd = injectConnFd;
        pfds[(size_t)kConnSlot].events = POLLIN;
        int pr = poll(pfds.data(), (nfds_t)pfds.size(), 100);
        if (pr > 0 && injectListenFd >= 0 && (pfds[(size_t)kListenSlot].revents & POLLIN)) {
            int c = accept(injectListenFd, nullptr, nullptr);
            if (c >= 0) {
                if (injectConnFd >= 0) close(injectConnFd);
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
        {
            unsigned n = nowMs();
            if (haveProbeStatPath && realNfds > 0 && n - lastLivenessProbeMs >= kLivenessProbeMs) {
                lastLivenessProbeMs = n;
                struct stat fdStat{}, pathStat{};
                bool stillThere = fstat(pfds[0].fd, &fdStat) == 0
                    && stat(probeStatPath, &pathStat) == 0
                    && fdStat.st_ino == pathStat.st_ino
                    && fdStat.st_dev == pathStat.st_dev;
                if (!stillThere) break;
            }
        }
        if (!gotInjectedByte) {
            if (pr == 0) {
                unsigned n = nowMs();
                grid.pollHolds(n, ps, link, audio);
                auto t = audio ? audio->snapshotTelemetry() : AudioThread::Telemetry{};
                leds.refresh(n, grid, grid.liveEngaged(), ledWrite, audio ? t.looperLevel : nullptr, audio ? t.gridBeatIndex : -1);
                continue;
            }
            if (pr < 0) {
                unsigned n = nowMs();
                grid.pollHolds(n, ps, link, audio);
                auto t = audio ? audio->snapshotTelemetry() : AudioThread::Telemetry{};
                leds.refresh(n, grid, grid.liveEngaged(), ledWrite, audio ? t.looperLevel : nullptr, audio ? t.gridBeatIndex : -1);
                continue;
            }
            if (!realReady) continue;
            if (snd_rawmidi_read(in, &b, 1) != 1) break;
        }
        static uint64_t noteLogCount = 0;
        bool isNoteStatusByte = (b & 0x80) && ((b & 0xF0) == 0x80 || (b & 0xF0) == 0x90);
        bool loggingThisMsg = isNoteStatusByte || (phase == 2 && ((st & 0xF0) == 0x80 || (st & 0xF0) == 0x90));
        if (loggingThisMsg && noteLogCount < 500) { fprintf(stderr, "[midi] note raw byte: 0x%02x (phase=%d)\n", b, phase); noteLogCount++; }
        if (b & 0x80) { st = b; phase = 1; continue; }
        if (phase == 1) { d1 = b; phase = 2; continue; }
        d2 = b; phase = 1;
        uint8_t type = st & 0xF0;
        uint8_t channel = st & 0x0F;
        unsigned now = nowMs();
        if ((type == 0x80 || type == 0x90) && noteLogCount < 500)
            fprintf(stderr, "[midi] note decoded: st=0x%02x type=0x%02x ch=%d d1=%d d2=%d\n", st, type, channel, d1, d2);
        grid.pollHolds(now, ps, link, audio);
        {
            auto t = audio ? audio->snapshotTelemetry() : AudioThread::Telemetry{};
            leds.refresh(now, grid, grid.liveEngaged(), ledWrite, audio ? t.looperLevel : nullptr, audio ? t.gridBeatIndex : -1);
        }

        if (channel == 0) {
            if (d1 == kApcBtnShift) {
                if (type == 0x90 && d2 > 0) { grid.onShiftPress(ps); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onShiftRelease(ps); continue; }
            }
            if (d1 == kApcLiveLedNote && type == 0x90 && d2 > 0) { grid.onLiveEngageToggle(ps); continue; }
            if (type == 0xB0 && d1 == 1)  { grid.onModWheel(d2, ps); continue; }
            if (type == 0xB0 && d1 == 52) { grid.onAbsolutePitch(d2, ps); continue; }
            if (type == 0xB0 && d1 == 53) { grid.onFormantCC(d2, ps); continue; }
            if (type == 0xB0 && (d1 == 48 || d1 == 49 || d1 == 50 || d1 == 51 || d1 == 54 || d1 == 55 || d1 == 57)) {
                grid.onFxKnobCC((int)d1, d2, ps, audio ? audio->sampler() : nullptr, audio ? audio->homeFx() : nullptr);
                continue;
            }
            if (d1 >= 82 && d1 <= 86) {
                if (type == 0x90 && d2 > 0) { grid.onMicrorepeatOn((int)d1, ps); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onMicrorepeatOff((int)d1, ps); continue; }
            }
            if (d1 == kApcBtnDubFx    && type == 0x90 && d2 > 0) { grid.onDubFxPress(now, ps); continue; }
            if (d1 == kApcBtnLofiFx) {
                if (type == 0x90 && d2 > 0) { grid.onLofiFxPress(now, ps, audio ? audio->sampler() : nullptr, audio); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onLofiFxRelease(now, ps, audio ? audio->sampler() : nullptr); continue; }
            }
            if (d1 == kApcBtnGuitarFx) {
                if (type == 0x90 && d2 > 0) { grid.onGuitarFxPress(now, ps); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onGuitarFxRelease(ps); continue; }
            }
            if (d1 == 65) {
                if (type == 0x90 && d2 > 0) { grid.onSamplerBtn65Press(audio ? audio->sampler() : nullptr); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onSamplerBtn65Release(audio ? audio->sampler() : nullptr); continue; }
            }
            if (d1 == 66) {
                if (type == 0x90 && d2 > 0) { grid.onSamplerBtn66Press(); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onSamplerBtn66Release(audio ? audio->sampler() : nullptr); continue; }
            }
            if (d1 < kApcRows * kApcCols) {
                if (type == 0x90 && d2 > 0) { grid.onPadPress((int)d1, now, ps, link, audio); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onPadRelease((int)d1, now, ps, link, audio); continue; }
            }
            if (type == 0x90 && d2 > 0 && d1 == 0x51 && grid.shiftHeld()) { grid.onStopImmediate(ps, link); continue; }
            if (d1 == 0x5B) {
                if (type == 0x90 && d2 > 0) { grid.onClearAll(true, ps, link); continue; }
                if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onClearAll(false, ps, link); continue; }
            }
        }

        if (channel == 1) {
            if (type == 0x90 && d2 > 0) { grid.onKeybedNoteOn((int)d1, (int)d2, ps, audio ? audio->sampler() : nullptr); continue; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { grid.onKeybedNoteOff((int)d1, ps, audio ? audio->sampler() : nullptr); continue; }
        }

        uint32_t key = 0; float val = 0;
        if (type == 0xB0) { key = midiKey(false, d1); val = d2 / 127.0f; }
        else if (type == 0x90 && d2 > 0) { key = midiKey(true, d1); val = 1.0f; }
        else if (type == 0x80 || (type == 0x90 && d2 == 0)) { key = midiKey(true, d1); val = 0.0f; }
        else continue;
        auto it = map.find(key);
        if (it != map.end()) ps.setByName(it->second, val);
    }
    fprintf(stderr, "[midi] %s disconnected -- will keep rescanning every 2s until a controller reappears\n", devbuf);
    snd_rawmidi_close(in);
    if (out) { snd_rawmidi_close(out); out = nullptr; }
    }
#else
    (void)ps;
#endif
}

}
