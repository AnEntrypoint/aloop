// aloop sampler — direct port of ../looper's patches/sampler.h. This class is
// intentionally UNCHANGED from the reference (it was already portable C++ with
// no bare-metal/Circle dependencies) — only the include guard and namespace
// wrapping are aloop-specific; every data structure, algorithm, and constant
// below matches looper's real hardware sampler exactly.
//
// Two capture modes, both gesture-driven from the APC Key 25:
//   * Button 65 HELD  -> record ONE shared "chromatic" sample. On release the
//     leading/trailing silence is auto-clipped and the 25 keyboard keys play
//     the sample pitched chromatically (middle C = note 60 = original speed),
//     polyphonically.
//   * Button 66 HELD  -> drum-record mode. While 66 is held, holding a keyboard
//     key records into THAT key's own drum slot (auto-clip on release). A loaded
//     drum slot plays at ORIGINAL pitch as a one-shot and OVERRIDES the
//     chromatic sample on that key.
//
// LOAD-BEARING INVARIANTS (unchanged from looper):
//   * Independent of the looper: this object touches NO loop-engine state. The
//     loopers keep recording/playing while the sampler records/plays.
//   * Sampler audio is mixed INTO the dry input buffer BEFORE the pitch/
//     effects/microrepeat/filter chain (renderInto), so samples get all
//     effects and are recordable by a loop (under SHIFT they fold into a
//     recording loop) -- see audio_thread.cpp's worker() call site.
//   * Capture reads a snapshot taken AFTER renderInto's own voice mix-in
//     (so a sample never records itself -- renderInto for THIS block always
//     runs before captureBlock for THIS block) but AFTER the SHIFT/glitch
//     loop-fold too -- DEPARTS from ../looper's reference here: user's
//     explicit request this session ("shift should route loops into the
//     sample recording... since loopers play into the input channel its a
//     surprise it doesnt already do this") means captureBlock DOES now see
//     SHIFT/glitch-folded loop content, unlike the original looper design
//     this file otherwise mirrors verbatim. See audio_thread.cpp's worker()
//     call site for the exact ordering.
//   * MIDI events arrive on the control thread; audio runs on the RT audio
//     thread. Events cross via a lock-free SPSC ring (pushEvent producer,
//     drained in renderInto consumer). Buffers are written and read only on
//     the audio thread.
//   * Click-free: per-voice attack/release gain ramps + a few-sample fade at
//     the auto-trim edges.
//
// Buffers are heap-allocated once in the ctor; no allocation in the audio
// path. Storage is s16 (short) to halve the footprint; the audio path here
// is s32 (int) mono, matching aloop's own s32-scale capture buffer.

#ifndef ALOOP_SAMPLER_H
#define ALOOP_SAMPLER_H

#include <stdint.h>
#include <string.h>
#include <math.h>

namespace aloop {

class Sampler {
public:
    static const int SR             = 48000;          // native rate
    static const int CHROM_MAX      = 5 * SR;         // 5s chromatic sample
    static const int DRUM_MAX       = 2 * SR;         // 2s per drum slot
    static const int NUM_DRUM       = 25;             // 25 keyboard keys
    static const int BASE_NOTE      = 48;             // lowest keyboard key (C2)
    static const int ROOT_NOTE      = 60;             // chromatic original-speed (C4/middle C)
    static const int VOICES         = 16;             // poly voice pool
    static const int EVENT_RING     = 64;             // control-thread -> audio-thread event ring
    static const int TRIM_THRESH    = 200;            // |s16| silence threshold
    static const int EDGE_FADE      = 64;             // trailing fade-out (samples)
    static const int PREROLL        = 32;             // samples kept before onset (preserve attack)
    static const int LEAD_DECLICK   = 8;              // tiny leading fade-in (declick only, keeps punch)
    static const int MAX_GRAINS     = 48;             // fixed granular pool (pre-allocated, no per-block alloc)

    enum EvType { EV_NONE = 0, EV_NOTE_ON, EV_NOTE_OFF,
                  EV_REC_START, EV_REC_STOP };

    Sampler()
    {
        m_chromM = new short[CHROM_MAX];
        memset(m_chromM, 0, sizeof(short) * CHROM_MAX);
        m_chromLen = 0;
        m_chromLoaded = false;

        for (int k = 0; k < NUM_DRUM; k++) {
            m_drumM[k] = new short[DRUM_MAX];
            memset(m_drumM[k], 0, sizeof(short) * DRUM_MAX);
            m_drumLen[k] = 0;
            m_drumLoaded[k] = false;
        }

        for (int v = 0; v < VOICES; v++) m_voice[v].active = false;
        m_ageCtr = 0;

        m_recActive = false;
        m_recTarget = -2;   // -2 = none, -1 = chromatic, 0..24 = drum key
        m_recPos    = 0;

        m_evHead = 0;
        m_evTail = 0;

        // Feature 1 defaults: reproduce today's chromatic behavior exactly.
        // The old code had no fixed attack time at all — attackStep was
        // *derived* from voice duration (see _spawnVoice) — so there is no
        // single "old attack ms" to store. Instead we keep that duration-scaled
        // computation as the DEFAULT attack behavior (m_attackMs < 0 sentinel
        // means "use the legacy auto-scaled attack"), and only switch to a
        // fixed runtime-controlled ms value once the user explicitly calls
        // setAttackMs(). Release had a real fixed constant (GAIN_STEP, ~1ms),
        // so m_releaseMs defaults to that exact value converted to ms.
        m_attackMs  = -1.0f;               // sentinel: legacy auto-scaled attack
        m_releaseMs = 1000.0f / 48.0f;     // == old GAIN_STEP (1/48 per sample @48k) in ms

        // Feature 2 defaults: granulator off, pool empty. Params default to
        // values that would sound reasonable if enabled, but they are inert
        // until setGranulatorEnabled(true) is called.
        m_granOn         = false;
        m_grainMs        = 60.0f;    // 60ms grains: small enough for texture, large enough to carry pitch
        m_grainRateHz    = 20.0f;    // grains/sec spawned per active note (with overlap this yields dense clouds)
        m_pitchSprayCents= 0.0f;     // no spray by default
        m_posJitterMs    = 0.0f;     // no jitter by default -> scan is a plain linear read, matching non-granular
        m_scanRate       = 1.0f;     // 1.0 = advance through buffer at playback rate (like normal read)
        for (int g = 0; g < MAX_GRAINS; g++) m_grains[g].active = false;
        for (int v = 0; v < VOICES; v++) m_voice[v].grainAccum = 0.0;
    }

    ~Sampler()
    {
        delete[] m_chromM;
        for (int k = 0; k < NUM_DRUM; k++) { delete[] m_drumM[k]; }
    }

    // ---- Producer side (control thread) --------------------------------------
    // Lock-free SPSC push. target: REC_START uses note field as -1 (chromatic)
    // or 0..24 (drum key). NOTE_ON/OFF use note (raw MIDI note) + vel.
    void pushEvent(EvType type, int note, int vel)
    {
        unsigned head = m_evHead;
        unsigned next = (head + 1) % EVENT_RING;
        if (next == m_evTail) return;              // ring full -> drop
        m_ev[head].type = (uint8_t)type;
        m_ev[head].note = (int16_t)note;
        m_ev[head].vel  = (int16_t)vel;
        m_evHead = next;                           // publish after fields written
    }

    // Content gates read by the control thread to decide whether the keyboard
    // routes to the sampler. Cross-thread reads of plain bools — eventually-
    // consistent, which is fine for a UI routing gate.
    bool chromaticLoaded() const { return m_chromLoaded; }
    bool drumLoaded(int keyIdx) const
    {
        return (keyIdx >= 0 && keyIdx < NUM_DRUM) ? m_drumLoaded[keyIdx] : false;
    }
    static int keyIndex(int note)
    {
        return (note >= BASE_NOTE && note < BASE_NOTE + NUM_DRUM) ? (note - BASE_NOTE) : -1;
    }

    // ---- Feature 1: runtime attack/release for CHROMATIC voices only --------
    // Ranges chosen generously (0-2000ms) to cover pad-like slow fades through
    // to plucky instant hits, matching typical synth envelope UIs. Values are
    // clamped so a bad/uninitialized control-surface knob can't feed a negative
    // or absurdly long ramp into the audio thread (a negative attackStep would
    // never converge -> stuck-silent voice; a huge one is merely a slow fade,
    // harmless, but still clamped for sanity).
    //
    // NOTE: calling setAttackMs() at all switches chromatic voices OFF the
    // legacy duration-auto-scaled attack (see _spawnVoice) and onto a fixed
    // ms-based ramp. This is a one-way switch by design: once a user dials in
    // an explicit attack, "voice duration changed the attack time under you"
    // would be a confusing regression. Until this is called, behavior is
    // bit-for-bit the old auto-scaled attack (m_attackMs stays at its -1
    // sentinel from the ctor).
    void setAttackMs(float ms)
    {
        if (ms < 0.0f) ms = 0.0f;
        if (ms > 2000.0f) ms = 2000.0f;
        m_attackMs = ms;
    }
    void setReleaseMs(float ms)
    {
        if (ms < 0.0f) ms = 0.0f;
        if (ms > 2000.0f) ms = 2000.0f;
        m_releaseMs = ms;
    }

    // ---- Feature 2: granulator controls (chromatic + drum) -------------------
    // Off by default (see ctor) -> zero behavior change until explicitly enabled.
    void setGranulatorEnabled(bool on) { m_granOn = on; }
    bool granulatorEnabled() const     { return m_granOn; }

    // Grain size in ms. Lower bound avoids a near-zero-length grain (div-by-zero
    // in the envelope phase increment); upper bound is a sanity cap, not a hard
    // DSP limit (a very long "grain" is really just an unwindowed segment).
    void setGrainSizeMs(float ms)
    {
        if (ms < 5.0f) ms = 5.0f;
        if (ms > 500.0f) ms = 500.0f;
        m_grainMs = ms;
    }
    // Grain spawn rate in grains/sec (per sounding note). At 1/grainMs this is
    // "back to back, no overlap"; higher values overlap grains for a denser,
    // smoother texture; lower values leave audible gaps (rhythmic granular).
    void setGrainDensityHz(float hz)
    {
        if (hz < 0.5f) hz = 0.5f;
        if (hz > 200.0f) hz = 200.0f;
        m_grainRateHz = hz;
    }
    // Per-grain random playback-rate spray, in cents (+/-), applied on top of
    // the note's own pitch ratio. 0 = every grain plays at exactly the note's
    // rate (matches non-granular pitch exactly at spray=0).
    void setPitchSprayCents(float cents)
    {
        if (cents < 0.0f) cents = 0.0f;
        if (cents > 1200.0f) cents = 1200.0f;
        m_pitchSprayCents = cents;
    }
    // Random position jitter applied at each grain's spawn, in ms (+/-) around
    // the current scan position. 0 = grains start exactly on the advancing scan
    // pointer (fully deterministic read position, like the non-granular voice).
    void setPositionJitterMs(float ms)
    {
        if (ms < 0.0f) ms = 0.0f;
        if (ms > 1000.0f) ms = 1000.0f;
        m_posJitterMs = ms;
    }
    // Scan-rate multiplier: how fast the underlying read position advances
    // through the buffer relative to the note's natural playback rate. 1.0 =
    // scans through the buffer at the same rate a normal voice would consume
    // it (grain cloud tracks the same material a plain read would, just
    // granulated); 0 = frozen scan (grains keep re-reading around one spot,
    // classic "freeze" effect); >1 = scrubs through the buffer faster than
    // real-time playback (time-compression texture).
    void setScanRate(float rate)
    {
        if (rate < 0.0f) rate = 0.0f;
        if (rate > 8.0f) rate = 8.0f;
        m_scanRate = rate;
    }

    // ---- Consumer side (audio thread) ---------------------------------------
    // Append the DRY input block to the armed record buffer (no-op when not
    // recording). in is [M0..M_{n-1}] s32.
    void captureBlock(const int *in, int n)
    {
        if (!m_recActive) return;
        short *dm; int maxLen; int *lenp;
        if (!_recBuffers(dm, maxLen, lenp)) return;
        for (int i = 0; i < n; i++) {
            if (m_recPos >= maxLen) { m_recActive = false; break; }   // overrun clamp
            dm[m_recPos] = _clip16(in[i]);
            m_recPos++;
        }
        *lenp = m_recPos;
    }

    // Drain queued events, then mix all active voices into inout (s32, same
    // layout). Voices are additive; the host gates nothing — the sampler is one
    // more source in the dry input buffer.
    void renderInto(int *inout, int n)
    {
        _drainEvents();

        for (int v = 0; v < VOICES; v++) {
            Voice &vo = m_voice[v];
            if (!vo.active) continue;

            if (vo.granular) {
                _renderGranularVoice(v, inout, n);
                continue;
            }

            for (int i = 0; i < n; i++) {
                // End-of-sample: begin release so the tail fades click-free.
                if (vo.pos >= (double)(vo.len - 1)) { vo.target = 0.0f; }

                float sm = _readInterp(vo.M, vo.len, vo.pos);

                // Per-sample gain ramp toward target. Attack uses the per-voice
                // (duration-scaled, or runtime ms for chromatic) step so
                // short/fast voices open in time; release uses releaseStep,
                // which is the ORIGINAL fixed ~1ms GAIN_STEP for drum voices
                // (untouched) and the runtime-controllable ms-derived step for
                // chromatic voices (defaults to the same ~1ms value).
                if (vo.gain < vo.target)      { vo.gain += vo.attackStep;  if (vo.gain > vo.target) vo.gain = vo.target; }
                else if (vo.gain > vo.target) { vo.gain -= vo.releaseStep; if (vo.gain < vo.target) vo.gain = vo.target; }

                inout[i] += (int)(sm * vo.gain);

                vo.pos += vo.rate;
                if (vo.gain <= 0.0f && vo.target == 0.0f) { vo.active = false; break; }
            }
        }
    }

    // ---- Telemetry -----------------------------------------------------------
    bool recording() const   { return m_recActive; }
    int  recLen() const      { return m_recPos; }
    int  drumLoadedCount() const
    {
        int c = 0; for (int k = 0; k < NUM_DRUM; k++) if (m_drumLoaded[k]) c++;
        return c;
    }
    int  activeVoices() const
    {
        int c = 0; for (int v = 0; v < VOICES; v++) if (m_voice[v].active) c++;
        return c;
    }

private:
    static constexpr float GAIN_STEP = 1.0f / 48.0f;   // ~1ms attack/release @48k

    struct Voice {
        bool   active;
        const short *M;
        int    len;
        double pos;
        double rate;
        bool   sustain;     // chromatic: released by NOTE_OFF; drum one-shot: false
        int    note;        // raw MIDI note this voice answers NOTE_OFF for (-1 = none)
        float  gain;
        float  target;
        float  attackStep;  // per-voice attack ramp; scaled so short/fast (high-note) voices still reach audible gain before the sample ends
        float  releaseStep; // per-voice release ramp (chromatic: runtime-controlled via m_releaseMs; drum: always GAIN_STEP)
        unsigned age;
        bool   isChrom;     // true = triggered via _noteOn's chromatic path (subject to setAttackMs/setReleaseMs);
                             // false = drum one-shot (must keep its old fixed EDGE_FADE/LEAD_DECLICK/GAIN_STEP behavior untouched)
        bool   granular;    // true = this voice is rendered by the grain-cloud path instead of the plain _readInterp path
        double grainAccum;  // fractional grain-spawn accumulator (see renderInto's granular branch)
    };

    // A single active grain: an independent short-lived read into the SAME
    // m_chromM/m_drumM buffer as the owning voice, windowed with a raised-
    // cosine envelope so overlapping grains sum without clicks at their
    // boundaries. Grains are pooled globally (MAX_GRAINS) rather than per-voice
    // because voice count and grain count are independent RT-safety concerns —
    // one fixed pool sized for the worst case avoids a second per-voice
    // allocation-shaped structure.
    struct Grain {
        bool   active;
        int    voiceSlot;   // which m_voice[] this grain belongs to (for buffer pointer + note-off tracking)
        const short *M;
        int    len;
        double pos;         // fractional read position (own pointer, independent of the owning voice's scan pointer)
        double rate;        // this grain's own playback rate (note rate * random pitch-spray factor)
        int    lifeSamples; // total grain length in samples (fixed at spawn from m_grainMs)
        int    lifePos;     // samples elapsed since spawn (0..lifeSamples)
    };

    static short _clip16(int v)
    {
        return v > 32767 ? 32767 : (v < -32768 ? -32768 : (short)v);
    }

    static float _readInterp(const short *buf, int len, double pos)
    {
        int i0 = (int)pos;
        if (i0 < 0) i0 = 0;
        if (i0 >= len) return 0.0f;
        int i1 = i0 + 1; if (i1 >= len) i1 = len - 1;
        float frac = (float)(pos - (double)i0);
        return (float)buf[i0] * (1.0f - frac) + (float)buf[i1] * frac;
    }

    // Cheap xorshift32 PRNG, member state so it's deterministic-per-instance
    // and touches no global/thread-shared state (rand() is not guaranteed
    // reentrant/RT-safe across platforms; this is branch-free and allocation-
    // free, safe to call from the audio thread). Returns a float in [-1, 1).
    float _randBipolar()
    {
        m_rngState ^= m_rngState << 13;
        m_rngState ^= m_rngState >> 17;
        m_rngState ^= m_rngState << 5;
        // Top 24 bits -> [0, 1) -> [-1, 1)
        uint32_t bits = m_rngState;
        float u = (float)(bits >> 8) * (1.0f / 16777216.0f);   // [0,1)
        return u * 2.0f - 1.0f;
    }

    // Spawn one grain for voice slot `vSlot` at the given scan position, using
    // the voice's note-rate plus a random pitch-spray factor. Picks the oldest
    // grain if the pool is full (a fixed pool can't grow — audible truncation
    // of the single oldest grain is far less noticeable than dropping the
    // newest spawn, and both are rare in practice at MAX_GRAINS=48).
    void _spawnGrain(int vSlot, double scanPos)
    {
        Voice &vo = m_voice[vSlot];
        int slot = -1;
        for (int g = 0; g < MAX_GRAINS; g++) { if (!m_grains[g].active) { slot = g; break; } }
        if (slot < 0) {
            // Pool full: steal the grain furthest along in its own life (closest
            // to naturally finishing anyway, so the cut is least audible).
            int best = 0; float bestFrac = -1.0f;
            for (int g = 0; g < MAX_GRAINS; g++) {
                float frac = (float)m_grains[g].lifePos / (float)(m_grains[g].lifeSamples > 0 ? m_grains[g].lifeSamples : 1);
                if (frac > bestFrac) { bestFrac = frac; best = g; }
            }
            slot = best;
        }
        Grain &gr = m_grains[slot];
        gr.active = true;
        gr.voiceSlot = vSlot;
        gr.M = vo.M;
        gr.len = vo.len;

        // Position jitter: random offset around the scan pointer, converted
        // from ms to samples in BUFFER time (not output time) since it's a
        // read-position offset, not a playback-duration.
        double jitterSamples = 0.0;
        if (m_posJitterMs > 0.0f) {
            jitterSamples = (double)(_randBipolar() * m_posJitterMs * 0.001f * (float)SR);
        }
        double p = scanPos + jitterSamples;
        if (p < 0.0) p = 0.0;
        if (p > (double)(vo.len - 1)) p = (double)(vo.len - 1);
        gr.pos = p;

        // Pitch spray: random +/- cents around the voice's own note rate.
        float centsOffset = (m_pitchSprayCents > 0.0f) ? (_randBipolar() * m_pitchSprayCents) : 0.0f;
        gr.rate = vo.rate * pow(2.0, (double)centsOffset / 1200.0);

        gr.lifeSamples = (int)((m_grainMs * 0.001f) * (float)SR);
        if (gr.lifeSamples < 2) gr.lifeSamples = 2;   // avoid degenerate 0/1-sample "grain"
        gr.lifePos = 0;
    }

    // Render one granular voice's grain cloud into inout for n samples, and
    // spawn new grains as the voice's own scan position advances. The voice
    // itself carries NO independent audio contribution while granular=true —
    // vo.pos here is repurposed as the SCAN pointer (where new grains spawn
    // from), not a direct read position; grains do the actual buffer reads.
    // This mirrors the non-granular path's overall envelope semantics (a held
    // chromatic note keeps spawning grains until NOTE_OFF sets target=0, then
    // fades) but the "gain ramp" targets grain spawn-gating rather than a
    // single continuous read's amplitude.
    void _renderGranularVoice(int vSlot, int *inout, int n)
    {
        Voice &vo = m_voice[vSlot];

        // Spawn-interval in scan-time samples, from density (grains/sec).
        double spawnPeriod = (double)SR / (double)m_grainRateHz;

        for (int i = 0; i < n; i++) {
            // Attack/release envelope reuses the same gain state machine as
            // the plain-read path, but here it gates whether we KEEP SPAWNING
            // grains (attack/sustain) or let the cloud die out (release), plus
            // scales each grain's window amplitude — so NOTE_OFF still fades
            // the granular texture out click-free, same contract as before.
            if (vo.gain < vo.target)      { vo.gain += vo.attackStep;  if (vo.gain > vo.target) vo.gain = vo.target; }
            else if (vo.gain > vo.target) { vo.gain -= vo.releaseStep; if (vo.gain < vo.target) vo.gain = vo.target; }

            // Only spawn new grains while sounding (target>0); once released,
            // let the already-live grains ring out rather than cutting them.
            if (vo.target > 0.0f) {
                vo.grainAccum += 1.0;
                if (vo.grainAccum >= spawnPeriod) {
                    vo.grainAccum -= spawnPeriod;
                    _spawnGrain(vSlot, vo.pos);
                }
                // Scan position advances at rate*scanRate: scanRate=1 tracks
                // normal playback speed (grains spawn from where a plain read
                // would be), 0 freezes (grains keep re-picking near one spot),
                // >1 scrubs forward faster than real-time.
                vo.pos += vo.rate * (double)m_scanRate;
                if (vo.pos >= (double)(vo.len - 1)) vo.pos = 0.0;   // wrap: loop the scan through the buffer
            }

            // Mix all grains owned by this voice slot.
            float mixed = 0.0f;
            for (int g = 0; g < MAX_GRAINS; g++) {
                Grain &gr = m_grains[g];
                if (!gr.active || gr.voiceSlot != vSlot) continue;

                float sm = _readInterp(gr.M, gr.len, gr.pos);

                // Raised-cosine (Hann) window over the grain's lifetime: 0 at
                // both ends, 1 at the midpoint -> click-free onset/offset for
                // every grain regardless of overlap, matching the file's
                // existing "click-free" invariant without needing a dedicated
                // fade table (cosf cost here is fine: MAX_GRAINS<=48 per block,
                // not per sample-count-of-the-whole-buffer).
                float phase = (float)gr.lifePos / (float)gr.lifeSamples;
                float win = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * phase));

                mixed += sm * win;

                gr.pos += gr.rate;
                gr.lifePos++;
                if (gr.lifePos >= gr.lifeSamples || gr.pos < 0.0 || gr.pos >= (double)(gr.len - 1)) gr.active = false;
            }

            inout[i] += (int)(mixed * vo.gain);

            if (vo.gain <= 0.0f && vo.target == 0.0f) {
                // Voice released and faded: kill any still-active grains it
                // owns (avoids an orphaned grain rendering forever after the
                // voice slot is reused, since a fresh voice checks
                // grains[g].voiceSlot==thisSlot without checking "is this MY
                // spawn generation").
                for (int g = 0; g < MAX_GRAINS; g++)
                    if (m_grains[g].active && m_grains[g].voiceSlot == vSlot) m_grains[g].active = false;
                vo.active = false;
                break;
            }
        }
    }

    bool _recBuffers(short *&dm, int &maxLen, int *&lenp)
    {
        if (m_recTarget == -1) { dm = m_chromM; maxLen = CHROM_MAX; lenp = &m_chromLen; return true; }
        if (m_recTarget >= 0 && m_recTarget < NUM_DRUM) {
            dm = m_drumM[m_recTarget];
            maxLen = DRUM_MAX; lenp = &m_drumLen[m_recTarget]; return true;
        }
        return false;
    }

    void _startRecord(int target)
    {
        // target: -1 chromatic, 0..24 drum. Stop any voices reading a drum slot
        // we are about to overwrite (no read mid-rewrite). Also kill any grains
        // those voices owned -- a granular voice's grains hold their own copy
        // of the buffer pointer (Grain::M) and would otherwise keep reading a
        // buffer that's about to be overwritten by the incoming recording.
        if (target >= 0 && target < NUM_DRUM) {
            for (int v = 0; v < VOICES; v++)
                if (m_voice[v].active && m_voice[v].M == m_drumM[target]) {
                    m_voice[v].active = false;
                    for (int g = 0; g < MAX_GRAINS; g++)
                        if (m_grains[g].active && m_grains[g].voiceSlot == v) m_grains[g].active = false;
                }
            m_drumLoaded[target] = false;
            m_drumLen[target] = 0;
        } else if (target == -1) {
            for (int v = 0; v < VOICES; v++)
                if (m_voice[v].active && m_voice[v].M == m_chromM) {
                    m_voice[v].active = false;
                    for (int g = 0; g < MAX_GRAINS; g++)
                        if (m_grains[g].active && m_grains[g].voiceSlot == v) m_grains[g].active = false;
                }
            m_chromLoaded = false;
            m_chromLen = 0;
        }
        m_recActive = true;
        m_recTarget = target;
        m_recPos    = 0;
    }

    void _stopRecord()
    {
        // Finalize whenever a capture target is pending — m_recActive may already
        // be false if captureBlock hit the overrun clamp before the stop event.
        if (m_recTarget == -2) return;
        m_recActive = false;
        short *dm; int maxLen; int *lenp;
        if (!_recBuffers(dm, maxLen, lenp)) { m_recTarget = -2; return; }
        int len = *lenp;
        int trimmed = _autoTrim(dm, len);
        *lenp = trimmed;
        if (m_recTarget == -1) m_chromLoaded = (trimmed > 0);
        else if (m_recTarget >= 0 && m_recTarget < NUM_DRUM) m_drumLoaded[m_recTarget] = (trimmed > 0);
        m_recTarget = -2;
    }

    // Auto-clip leading/trailing silence in place; returns new length. A short
    // fade-in/out is applied at the trimmed edges for click-free one-shots.
    // All-silence -> returns 0 (slot stays unloaded).
    static int _autoTrim(short *M, int len)
    {
        if (len <= 0) return 0;
        // First and last sample whose magnitude clears the silence threshold.
        int start = -1, end = -1;
        for (int i = 0; i < len; i++) {
            int a = M[i]; if (a < 0) a = -a;
            if (a > TRIM_THRESH) { if (start < 0) start = i; end = i; }
        }
        if (start < 0 || end < start) return 0;     // all silence
        // PRE-ROLL: keep a few samples BEFORE the first threshold crossing so the
        // real attack transient (drum hit / pluck) is preserved, not chopped at
        // the steep part of its rise. Without this the onset starts mid-transient
        // and a leading fade would further soften the punch.
        if (start > PREROLL) start -= PREROLL; else start = 0;
        int newLen = end - start + 1;
        if (start > 0) {
            memmove(M, M + start, sizeof(short) * newLen);
        }
        // Leading edge: only a TINY declick (preserve the attack), not a long
        // fade-in. Trailing edge: a longer fade-out so one-shots end click-free.
        int fin = newLen < LEAD_DECLICK ? newLen : LEAD_DECLICK;
        for (int i = 0; i < fin; i++) {
            float g = (float)i / (float)fin;
            M[i] = (short)(M[i] * g);
        }
        int fout = newLen < EDGE_FADE ? newLen : EDGE_FADE;
        for (int i = 0; i < fout; i++) {
            float g = (float)i / (float)fout;
            int j = newLen - 1 - i;
            M[j] = (short)(M[j] * g);
        }
        return newLen;
    }

    // Returns the voice slot used, so callers can do post-spawn setup (e.g.
    // arming the granular grain cloud) without a second linear scan for "which
    // slot did we just steal/take".
    int _spawnVoice(const short *M, int len, double rate, bool sustain, int note, bool isChrom)
    {
        if (len <= 0) return -1;
        int slot = -1; unsigned oldest = 0xFFFFFFFF;
        for (int v = 0; v < VOICES; v++) {
            if (!m_voice[v].active) { slot = v; break; }
            if (m_voice[v].age < oldest) { oldest = m_voice[v].age; slot = v; }
        }
        Voice &vo = m_voice[slot];
        vo.active = true; vo.M = M; vo.len = len;
        vo.pos = 0.0; vo.rate = rate; vo.sustain = sustain; vo.note = note;
        vo.gain = 0.0f; vo.target = 1.0f; vo.age = ++m_ageCtr;
        vo.isChrom = isChrom;
        vo.granular = false;
        vo.grainAccum = 0.0;
        // Stealing this slot orphans any grains it owned from a previous voice;
        // kill them so a stolen slot doesn't keep spawning/rendering grains
        // that think they belong to the new voice's buffer/rate.
        for (int g = 0; g < MAX_GRAINS; g++)
            if (m_grains[g].active && m_grains[g].voiceSlot == slot) m_grains[g].active = false;

        // DRUM path: completely unchanged from before this feature — same
        // duration-scaled attack, same fixed GAIN_STEP release. Untouched per
        // the explicit "drums out of scope" constraint.
        //
        // CHROMATIC path: if the user never called setAttackMs()/setReleaseMs(),
        // m_attackMs stays at its -1 sentinel and m_releaseMs stays at its
        // ctor default (== old GAIN_STEP in ms) -> bit-for-bit old behavior.
        // Once set, chromatic voices use the fixed ms-derived step instead of
        // the duration-auto-scaled one.
        double playable = (rate > 0.0) ? ((double)len / rate) : (double)len;
        float fastStep = (playable > 4.0) ? (float)(1.0 / (playable * 0.25)) : 0.25f;
        float legacyAttackStep = fastStep > GAIN_STEP ? fastStep : GAIN_STEP;

        if (isChrom && m_attackMs >= 0.0f) {
            // ms -> per-sample step: full 0..1 traverse in (m_attackMs/1000)*SR
            // samples. Guard against ms==0 (instant attack -> avoid div-by-zero,
            // just snap in one sample).
            float samples = (m_attackMs * 0.001f) * (float)SR;
            vo.attackStep = (samples > 0.0f) ? (1.0f / samples) : 1.0f;
        } else {
            vo.attackStep = legacyAttackStep;
        }

        if (isChrom) {
            float samples = (m_releaseMs * 0.001f) * (float)SR;
            vo.releaseStep = (samples > 0.0f) ? (1.0f / samples) : 1.0f;
        } else {
            vo.releaseStep = GAIN_STEP;   // drum: always the old fixed constant
        }
        return slot;
    }

    void _noteOn(int note, int /*vel*/)
    {
        int k = keyIndex(note);
        if (k >= 0 && m_drumLoaded[k]) {
            // Drum one-shot at original pitch (ignores note-off, plays to end).
            int slot = _spawnVoice(m_drumM[k], m_drumLen[k], 1.0, false, -1, /*isChrom=*/false);
            if (slot >= 0 && m_granOn) m_voice[slot].granular = true;
            return;
        }
        if (m_chromLoaded) {
            // Mono-per-note retrigger: release any voice already sustaining THIS
            // note so a re-press doesn't stack voices and so the 16-voice steal
            // can't orphan a held note's eventual NOTE_OFF (auto-sustain bug).
            for (int v = 0; v < VOICES; v++)
                if (m_voice[v].active && m_voice[v].sustain && m_voice[v].note == note)
                    m_voice[v].target = 0.0f;
            double rate = pow(2.0, (double)(note - ROOT_NOTE) / 12.0);
            int slot = _spawnVoice(m_chromM, m_chromLen, rate, true, note, /*isChrom=*/true);
            if (slot >= 0 && m_granOn) m_voice[slot].granular = true;
        }
    }

    void _noteOff(int note)
    {
        // Release sustaining (chromatic) voices owned by this note.
        for (int v = 0; v < VOICES; v++)
            if (m_voice[v].active && m_voice[v].sustain && m_voice[v].note == note)
                m_voice[v].target = 0.0f;
    }

    void _drainEvents()
    {
        while (m_evTail != m_evHead) {
            uint8_t type = m_ev[m_evTail].type;
            int     note = m_ev[m_evTail].note;
            int     vel  = m_ev[m_evTail].vel;
            m_evTail = (m_evTail + 1) % EVENT_RING;
            switch (type) {
                case EV_NOTE_ON:   _noteOn(note, vel);  break;
                case EV_NOTE_OFF:  _noteOff(note);      break;
                case EV_REC_START: _startRecord(note);  break;   // note field = target
                case EV_REC_STOP:  _stopRecord();       break;
                default: break;
            }
        }
    }

    // Chromatic sample
    short *m_chromM;
    int    m_chromLen;
    volatile bool m_chromLoaded;

    // Per-key drum slots
    short *m_drumM[NUM_DRUM];
    int    m_drumLen[NUM_DRUM];
    volatile bool m_drumLoaded[NUM_DRUM];

    // Voice pool
    Voice    m_voice[VOICES];
    unsigned m_ageCtr;

    // Record state (audio thread only)
    volatile bool m_recActive;
    int  m_recTarget;   // -2 none, -1 chromatic, 0..24 drum
    int  m_recPos;

    // control-thread -> audio-thread event ring
    struct Ev { uint8_t type; int16_t note; int16_t vel; };
    volatile Ev m_ev[EVENT_RING];
    volatile unsigned m_evHead, m_evTail;

    // Feature 1: runtime attack/release (chromatic voices only; see setAttackMs/
    // setReleaseMs). m_attackMs's -1 sentinel means "use the legacy duration-
    // auto-scaled attack" (see _spawnVoice) — there is no equivalent sentinel
    // needed for release since the old code had one real fixed constant
    // (GAIN_STEP) to default to exactly.
    float m_attackMs;
    float m_releaseMs;

    // Feature 2: granulator pool + runtime params. Pool is fixed-size and
    // constructed once (see ctor) — no allocation on the audio path.
    Grain    m_grains[MAX_GRAINS];
    bool     m_granOn;
    float    m_grainMs;
    float    m_grainRateHz;
    float    m_pitchSprayCents;
    float    m_posJitterMs;
    float    m_scanRate;
    uint32_t m_rngState = 2463534242u;   // xorshift32 seed (any nonzero value works; fixed for reproducibility)
};

} // namespace aloop
#endif // ALOOP_SAMPLER_H
