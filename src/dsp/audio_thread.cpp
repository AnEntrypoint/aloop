// aloop RT audio thread — the Linux replacement for Circle's multicore audio
// dispatch. See audio_thread.h for the contract and MIGRATION-MAP for the
// bare-metal → Linux mapping.
//
// The per-block loop is the real-time-critical path: it does NO malloc, no locks,
// no syscalls except the intended ALSA read/write (the blocking point). Memory is
// pre-faulted (mlockall in main). The thread runs SCHED_FIFO, pinned to the
// home-FX core.

#include "audio_thread.h"
#include "../host/lv2_host.h"
#include "../control/midi.h"
#include "../link/link_bridge.h"
#include "sampler/sampler.h"

#include <pthread.h>
#include <sched.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

// ALSA is present in the build container (see build-binary.yml). Guarded so the
// design compiles for review even where ALSA headers are absent; the real device
// build always has them.
#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#define ALOOP_HAVE_ALSA 1
#endif

// The Faust home stack (loop engine + effects) generated from dsp/loop.dsp (and
// composable with dsp/aloop.dsp) is compiled to loop.cpp by the CMake custom
// command and included here. Guarded so this file compiles before the generated
// file exists (review builds); the device build always generates it first.
#if __has_include("loop.cpp")
#define FAUSTFLOAT float
struct FaustMeta { void declare(const char*, const char*) {} };
// A real param-binding UI: it captures each control's name → its zone pointer so
// the audio thread can SET the Faust engine's controls (rec/play/len/vol per looper (no overdub) 
// the effect knobs) from the MIDI ParamStore + Link each block. Without this the
// controls are inert (the loop would never record).
#include <map>
#include <string>
struct FaustUI {
    std::map<std::string, float*> zones;   // FULL path (group/…/label) → zone
    std::vector<std::string> path;         // current open-box group stack
    std::string full(const char* label) const {
        std::string p;
        for (auto& g : path) if (!g.empty()) { p += g; p += "/"; }
        p += label;
        return p;   // e.g. "looper03/rec"  (the vgroup name + the control label)
    }
    void openTabBox(const char* l){ path.push_back(l?l:""); }
    void openHorizontalBox(const char* l){ path.push_back(l?l:""); }
    void openVerticalBox(const char* l){ path.push_back(l?l:""); }
    void closeBox(){ if(!path.empty()) path.pop_back(); }
    void addButton(const char* l, float* z){ zones[full(l)]=z; }
    void addCheckButton(const char* l, float* z){ zones[full(l)]=z; }
    void addVerticalSlider(const char* l, float* z, float, float, float, float){ zones[full(l)]=z; }
    void addHorizontalSlider(const char* l, float* z, float, float, float, float){ zones[full(l)]=z; }
    void addNumEntry(const char* l, float* z, float, float, float, float){ zones[full(l)]=z; }
    void addHorizontalBargraph(const char*, float*, float, float){}
    void addVerticalBargraph(const char*, float*, float, float){}
    void addSoundfile(const char*, const char*, void**){}
    void declare(float*, const char*, const char*){}
    // Set a control by full path (no-op if the dsp doesn't expose it). Matches
    // either the exact path or any zone whose path ENDS with the given suffix,
    // so "HPCUT" finds ".../HPCUT" and "looper03/rec" matches exactly.
    void set(const char* name, float v){
        auto it=zones.find(name);
        if(it!=zones.end()){ *it->second=v; return; }
        std::string suf(name);
        for(auto& kv:zones){ const std::string& k=kv.first;
            if(k.size()>=suf.size() && k.compare(k.size()-suf.size(), suf.size(), suf)==0){ *kv.second=v; return; } }
    }
    // Read a control back by full-or-suffix path (for state telemetry). Returns
    // `def` if no such zone. Same matching rule as set().
    float get(const char* name, float def=0.0f) const {
        auto it=zones.find(name);
        if(it!=zones.end()) return *it->second;
        std::string suf(name);
        for(auto& kv:zones){ const std::string& k=kv.first;
            if(k.size()>=suf.size() && k.compare(k.size()-suf.size(), suf.size(), suf)==0) return *kv.second; }
        return def;
    }
};
#define Meta FaustMeta
#define UI FaustUI
#define dsp FaustDspBase
struct FaustDspBase { virtual ~FaustDspBase(){} };
#include "loop.cpp"     // defines class AloopLoopDsp : public FaustDspBase
#undef dsp
#define ALOOP_HAVE_FAUST_LOOP 1
#endif

namespace aloop {

namespace {
std::atomic<bool> g_running{false};
pthread_t g_worker;
AudioThread::Telemetry g_telem{};
AudioConfig g_cfg;
ParamStore* g_params = nullptr;   // shared control store (from MIDI); read each block
LinkBridge* g_link = nullptr;     // Ableton Link (tempo/phase); read each block for varispeed sync
// The sampler (src/dsp/sampler/sampler.h, a direct port of ../looper's
// patches/sampler.h): owns its own lock-free event ring, so the MIDI/control
// thread can push events into it via this pointer without touching ParamStore
// at all -- matching looper's own ISR-pushes/audio-thread-drains split.
// Constructed once inside worker() (its buffers are ~1MB+, too large to be a
// stack-local for the same reason AloopLoopDsp was moved to the heap -- see
// ADR-013) and published here for the MIDI thread to reach.
aloop::Sampler* g_sampler = nullptr;

// TRUE varispeed state (see dsp/loop.dsp's top-of-file comment + the
// "Varispeed Link sync" block below). Mirrors looper's split exactly:
// g_globalSpeedMul (manual half/double-speed, PURELY button-driven,
// confirmed via ../looper's loopMachine.cpp:530-545) is entirely separate
// from the Link-tempo-driven per-phrase ratio (looper's per-clip
// m_playRate = m_nativeBlocks/currentMasterBlocks, Looper.h:301-305).
// aloop shares ONE master phrase length across the whole rig (apc_grid.cpp's
// cmd/master_len, unlike looper's per-clip m_nativeBlocks -- see
// deriveTempoBpm's own comment for why aloop's design is already
// rig-wide-shared, not per-looper), so the architecturally-correct port is a
// SINGLE shared "recorded BPM" (the tempo implied by cmd/master_len at the
// moment it was established) tracked via the SAME ParamStore mechanism as
// cmd/master_len itself (see apc_grid.cpp's applyRecPlayCycle, which now
// also writes cmd/recorded_bpm alongside cmd/master_len) rather than a
// per-looper value or a private static local here -- keeps a single source
// of truth reachable from both the control thread (which establishes it)
// and this audio thread (which reads it every block below).
float g_manualSpeedMul = 1.0f;   // 1.0 / 0.5 / 2.0, set by the halfspeed/doublespeed block


// Map a control-map TARGET name ("looper3/rec", "fx/hp") to the Faust zone label
// the home stack exposes. Loopers use a 2-digit index (looper03/rec); effects use
// the chain's slider labels. Returns "" if there is no matching zone.
static std::string targetToZone(const std::string& target) {
    // looperN/xxx → "looper%2i/xxx" (Faust's width-2 right-justified index: a
    // space for single digits — "looper 0/rec" … "looper19/rec"). The UI shim's
    // set() also matches by suffix, so exact formatting is belt-and-suspenders.
    if (target.rfind("looper", 0) == 0) {
        auto slash = target.find('/');
        if (slash != std::string::npos) {
            int idx = atoi(target.c_str() + 6);
            char z[64];
            snprintf(z, sizeof z, "looper%2d/%s", idx, target.c_str() + slash + 1);
            return z;
        }
    }
    // fx/hp etc → the effect chain's control labels (from param_mapping.md).
    if (target == "fx/hp")      return "HPCUT";
    if (target == "fx/lp")      return "LPCUT";
    if (target == "fx/lpres")   return "LPRES";
    if (target == "fx/reverb")  return "REVAMT";
    if (target == "fx/delay")   return "DELAYAMT";
    if (target == "fx/time")    return "TIME";
    if (target == "fx/formant") return "FORMANT";
    if (target == "fx/pitch")   return "SEMIS";
    // fx/bank (LOFI feature, 3-bank fx control surface): unlike the 7 knobs
    // above, this Faust zone is declared under its OWN literal name
    // (nentry("fx/bank", ...) in effects_runtime.dsp), not a renamed control
    // label -- so the mapping is a straight passthrough, not a rename.
    // WITNESSED BUG (live, real Pi 4): this mapping was MISSING entirely,
    // so even after ApcGrid::pushBankValuesToZones started writing
    // ParamStore's "fx/bank" target (see its own updated comment), this
    // function's fallthrough `return ""` meant the generic forEach-push
    // below silently never forwarded it into the real Faust zone at all --
    // bank-select buttons updated C++ state correctly but the DSP's own
    // bank-crossfade never saw the change, staying pinned on Dub forever.
    if (target == "fx/bank")    return "fx/bank";
    // fx/monitorfold has NO Faust zone anymore -- the SHIFT-held fold is a
    // native block-rate mix (see the prevLoopSum/foldGain code in worker()
    // and aloop.dsp's top-of-file comment for why), so it's read directly via
    // g_params->get("fx/monitorfold") rather than pushed into any Faust zone.
    return "";   // commands (cmd/*) and fx/pitchbend*, fx/microrepeat_div are handled separately, not a plain 1:1 Faust zone
}

// Pin the CURRENT thread to `core` and set SCHED_FIFO at `prio`. This is the
// Linux equivalent of the bare-metal per-core assignment (MIGRATION-MAP).
bool setRealtimeSelf(int core, int prio) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        fprintf(stderr, "[audio] warning: could not pin to core %d\n", core);
    sched_param sp{};
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "[audio] warning: SCHED_FIFO prio %d failed (need rtprio limit)\n", prio);
        return false;
    }
    return true;
}
} // namespace

// The worker: open the PCM bridged to the f_uac2 gadget, then loop per block:
//   read capture -> DSP (loopMachine::update, wired via the ported source) ->
//   Lv2Host::runBlock (home + user effects, in-process) -> write playback.
// Here we implement the RT scaffolding + a clean passthrough; the DSP and host
// calls slot in where marked once the ported source + host impl are linked (their
// interfaces are already fixed, so this file does not change when they land).
static void* worker(void*) {
    setRealtimeSelf(g_cfg.homeFxCore, g_cfg.rtPriority);
    const int N = g_cfg.blockSize;
    const int ch = g_cfg.channels;   // DSP channel count (mono = 1), the Faust I/O.

    // The f_uac2 USB gadget presents a STEREO wire (c_chmask/p_chmask = 0x3), the
    // same as the looper's UAC2 (stereo wire, mono internally). So the ALSA PCM is
    // opened with `wireCh` channels and we deinterleave capture -> mono for Faust,
    // then duplicate mono -> both wire channels on playback. If the device is truly
    // mono (channels config == the wire), wireCh collapses to ch and this is a
    // straight copy. See ADR-008 / f_uac2-gadget.sh.
    const int wireCh = (ch < 2) ? 2 : ch;   // USB wire is stereo; DSP is mono

    // Instrument device sample buffer is int32_t, NOT int16_t. WITNESSED live:
    // the M-Audio AIR 192|4 (and most class-compliant USB audio interfaces)
    // only supports S32_LE (24-bit data left-justified in a 32-bit word,
    // confirmed via /proc/asound/card0/stream0: "Format: S32_LE, Bits: 24") —
    // there is no S16_LE fallback on this hardware. The prior code hardcoded
    // SND_PCM_FORMAT_S16_LE and an int16_t buffer without checking whether the
    // format request actually succeeded; ALSA silently negotiated S32_LE
    // anyway (hw_params still succeeded — only the specific format request was
    // ignored) while the code kept treating the wire as 16-bit, writing
    // half-width garbage into what the hardware read as 32-bit words — this
    // is what produced loud static once the instrument device was finally
    // opened at all (ADR-015). The OTG gadget, by contrast, genuinely IS
    // S16_LE (src/usb/f_uac2-gadget.sh sets c_ssize/p_ssize=2 ourselves), so
    // the two devices need separate wire buffers in their own native formats.
    std::vector<int32_t> buf((size_t)N * wireCh, 0);       // instrument device (S32_LE)
    std::vector<int16_t> otgBuf((size_t)N * wireCh, 0);    // OTG gadget mirror (S16_LE)
    std::vector<float> fin((size_t)N, 0.0f), fout((size_t)N, 0.0f);  // mono DSP buffers
    // Previous block's RAW loop-engine output (before fx), for the SHIFT-held
    // monitor-fold (native mix, not a Faust graph cycle -- a Faust-level
    // attempt at this via the `~` recursion operator was tried and WITNESSED
    // to silently break basic dry passthrough live on hardware; reverted, see
    // git history commit 90083c4 -> 3b8bd5e). Mirrors looper's real
    // one-block-lag fold exactly (loopMachine.cpp:709-741:
    // m_input_buffer[i] += m_output_buffer[i]*fg, where m_output_buffer is
    // the loop's RAW played-back content, not the fully-effected wet mix --
    // folding the wet signal back in would compound effects every block the
    // fold is held). Feeding `prevLoopSum` into `fin` BEFORE
    // faustHome.compute() means loop.dsp's record path (which runs first
    // inside the Faust graph) sees the folded signal, without needing any
    // Faust-side recursion at all.
    std::vector<float> prevLoopSum((size_t)N, 0.0f);
    // Previous block's post-glitch (microrepeat), pre-filter tap -- same
    // one-block-lag native-mix technique as prevLoopSum, applied so glitch
    // content becomes recordable into a new loop AND affects already-playing
    // loops on the next pass through (matching looper's "stutter becomes
    // BOTH the audible output and the record source", loopMachine.cpp:806-833).
    // Always folded in (not SHIFT-gated) since glitch is a momentary
    // performance effect the user explicitly engages via its own latch
    // button, unlike the fold which is deliberately SHIFT-held-only.
    // Previous block's FULLY-EFFECTED mix output (pitch/delay/reverb/
    // microrepeat/filters, aloop.dsp's 4th process() output "recordTap"),
    // same one-block-lag native-mix technique as prevLoopSum (and the now-
    // removed prevGlitchTap, which this supersedes -- see below)
    // above. User-confirmed requirement: recording must ALWAYS capture the
    // fully-effected signal, not raw pre-fx input, unconditionally (not just
    // under SHIFT). Feeds loop.dsp's record-only input directly (REPLACING
    // the old glitchIn wiring -- prevFiltOut already contains post-glitch
    // content one block later since microStage is upstream of filterStage in
    // effects_runtime.dsp, making a separate glitch term redundant). Never
    // added into `fin`/dry -- only passed as its own dedicated Faust input --
    // so it structurally cannot re-enter `fx` on any later block (the exact
    // whine bug class dafa945 fixed for glitch specifically, now would be a
    // worse surface covering the whole fx chain if done wrong).
    std::vector<float> prevFiltOut((size_t)N, 0.0f);

    // Instantiate the Faust home stack (loop engine + effects). Its compute()
    // runs the whole home DSP per block — the loop record/play + the effects.
    //
    // WITNESSED live on a real Pi 4 (gdb + a real core dump, built with -g -O0
    // via a temporary debug CI job): AloopLoopDsp is 336,326,896 bytes (~320
    // MiB) — the 20 loopers' MAXLEN=48000*60 (60s) delay-line buffers add up
    // fast. As a STACK-LOCAL variable inside this thread's entry function, no
    // pthread stack size (musl's small default, or an explicit 8 MiB — both
    // tried and both crashed identically) could ever be large enough; the
    // SIGSEGV at setRealtimeSelf's very first local-variable stack write was
    // this frame simply being un-mapped from the moment the thread's stack
    // pointer moved past its real (small) allocation to make room for a
    // 320 MiB local later in the same function. Heap-allocate it instead —
    // this is a one-time allocation at thread startup, never in the per-block
    // RT hot path, so it carries none of the "no allocation in the audio
    // callback" real-time risk the rest of this file is written to avoid.
#ifdef ALOOP_HAVE_FAUST_LOOP
    auto faustHomePtr = std::make_unique<AloopLoopDsp>();
    AloopLoopDsp& faustHome = *faustHomePtr;
    faustHome.init((int)g_cfg.sampleRate);
    FaustUI fui; faustHome.buildUserInterface(&fui);
    // aloop.dsp's process() now takes 4 inputs: (in, prevFiltIn, clearAll,
    // speedMul) -- prevFiltIn is the previous block's FULLY-EFFECTED mix
    // output, routed to loop.dsp's DEDICATED record-only input (see
    // loop.dsp's oneLooper + aloop.dsp's top-of-file RECORD-ALWAYS-EFFECTED
    // comment) so it can never re-enter `fx`. clearAll/speedMul are the
    // momentary global commands (hardware CLEAR_ALL, HALFSPEED/DOUBLESPEED),
    // pushed as plain constant-per-block signal inputs INSTEAD of Faust UI
    // zones -- see loop.dsp's ROOT CAUSE comment above oneLooper: a UI
    // control (button()/hslider()) passed as an argument into oneLooper,
    // which par() instantiates 20 times, gets re-elaborated (UI declaration
    // included) at each of the 20 call sites, silently producing 20
    // DUPLICATE zones even when the declaration text is hoisted outside the
    // par/vgroup -- WITNESSED via the generated C++ (build/loop.cpp):
    // `grep -c '"speed"'`/`'"clear"'` both returned 20, each occurrence
    // still inside its own "looper N" vgroup, meaning the 382e775 "fix" was
    // cosmetic in the .dsp source and never actually collapsed to one zone,
    // which is why half/double-speed and clear-all kept only affecting one
    // of 20 loopers even after that commit. A signal input threaded through
    // par() is just a wire (no UI primitive to duplicate), so every looper
    // now genuinely reads the identical sample-accurate value every block.
    // clearBuf/speedBuf are filled with a CONSTANT value across the block
    // each iteration below (these are momentary step commands, not
    // audio-rate signals -- no interpolation needed, matching the previous
    // hslider/button zones' own block-constant behavior). speedBuf now
    // carries TRUE varispeed's combined effSpeed = manualSpeedMul *
    // linkSpeedRatio (see the "Varispeed Link sync" block below), matching
    // looper's `effectiveRate = m_playRate * g_globalSpeedMul`
    // (loopClipUpdate.cpp:76) -- one signal is all loop.dsp's read
    // accumulator needs, so the two factors are combined here rather than
    // threading a 5th process() input.
    std::vector<float> clearBuf((size_t)N, 0.0f);
    std::vector<float> speedBuf((size_t)N, 1.0f);
    // masterPhaseBuf: the PHRASE-LOCK shared clock (5th process() signal
    // input, same block-constant-per-block technique as clearBuf/speedBuf).
    // User's standing requirement: "our loops must stay perfectly in
    // phrase... there is no looper sync at all right now, they're
    // independent." Every looper's dsp/loop.dsp oneLooper anchors its own
    // recordStartPhaseOffset to THIS shared value at its own finishEdge, so
    // position becomes a pure function of (masterPhase - offset) rather than
    // an independently-integrated accumulator -- two loopers anchored to the
    // same masterPhase can never drift apart from each other. See the
    // "Master phase clock" block below for how this value is computed.
    std::vector<float> masterPhaseBuf((size_t)N, 0.0f);
    // masterLenBuf: the shared phrase length itself (6th process() signal
    // input), needed by dsp/loop.dsp's NEW arm-quantization grid-tick
    // detector (gridStep = masterLen/16) -- distinct from masterPhase
    // (position within the phrase) and each looper's own wrapLen (which can
    // differ per looper); this is the ONE shared length every looper's
    // grid-tick check must reference identically. Block-constant per block,
    // same technique as clearBuf/speedBuf (this is a slow-changing value,
    // only updated at FINISH/clear, not audio-rate).
    std::vector<float> masterLenBuf((size_t)N, 0.0f);
    // sidechainEnvBuf: 7th process()-level signal input (LOFI feature,
    // sidechain-pump). Block-constant per block, same technique as
    // clearBuf/speedBuf/masterLenBuf -- computed each block from the PREVIOUS
    // block's looperLevel[] telemetry (one-block-lag, same staleness class
    // already accepted for prevFiltIn/prevLoopSum's own one-block-lag fold;
    // see the "Sidechain envelope" computation below, right before compute()).
    std::vector<float> sidechainEnvBuf((size_t)N, 0.0f);
    float* fins[7]  = { fin.data(), prevFiltOut.data(), clearBuf.data(), speedBuf.data(), masterPhaseBuf.data(), masterLenBuf.data(), sidechainEnvBuf.data() };
    // WITNESSED live (real Pi 4, reported same day as this feature shipped):
    // continuous xrun growth (~10-25/sec, even fully idle) traced to THIS
    // loop originally calling g_params->get(z) with a freshly-snprintf'd
    // "looperN/sidechainsrc" std::string, 20 times, EVERY block -- unlike
    // every other g_params->get() call in this file (all fixed small-count,
    // literal-keyed), this one scaled with looper count and ran unconditionally
    // forever, the first genuinely RT-unsafe (unbounded string-construct +
    // hash-map-lookup) hot path this codebase has had.
    //
    // Fixed via LAZY one-time-per-slot resolution, NOT a single up-front
    // resolve at worker() startup: runMidiLoop's ApcGrid::bindAll (which
    // creates these slots) runs on a SEPARATE thread (main.cpp spawns
    // midiThread then calls audio.start() with no synchronization between
    // them), so resolving all 20 slots once here, before bindAll has
    // necessarily run, would permanently cache -1 (unbound) for any slot not
    // yet registered -- silently and PERMANENTLY breaking sidechain-pump,
    // not just delaying it a few blocks. Each element starts at -1 (unbound
    // sentinel) and is re-attempted via getSlot() every block ONLY while
    // still -1 (branch-predictable, cheap) -- once bindAll registers it, the
    // very next block resolves it and caches for the process's remaining
    // lifetime (slots are never unbound after startup), giving both RT-safety
    // and correctness: no allocation/map-lookup once resolved, no permanent
    // false-unbound race before bindAll runs.
    int sidechainSrcSlot[AudioThread::Telemetry::kLoopers];
    for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) sidechainSrcSlot[lp] = -1;
    // aloop.dsp's process() now outputs 4 signals: (wet mix, rawGlitchTap,
    // rawLoopSum, recordTap) -- native taps so the SHIFT-fold, the
    // glitch-loop-routing fold, AND the always-effected record path can each
    // feed next block's input without compounding effects every block
    // they're held (see aloop.dsp's top-of-file comment). recordTap is
    // numerically identical to fout (output 1) -- duplicated as its own
    // output solely so it can be snapshotted into prevFiltOut without
    // touching the live audible fout buffer.
    std::vector<float> rawGlitchTap((size_t)N, 0.0f);
    std::vector<float> rawLoopSum((size_t)N, 0.0f);
    std::vector<float> rawFiltTap((size_t)N, 0.0f);
    float* fouts[4] = { fout.data(), rawGlitchTap.data(), rawLoopSum.data(), rawFiltTap.data() };
#endif

#ifdef ALOOP_HAVE_FAUST_LOOP
    // Sampler (src/dsp/sampler/sampler.h): heap-allocated for the same reason
    // as faustHome above (its per-key drum + chromatic buffers total ~5.3MB,
    // large enough to be worth keeping off the thread stack even though it's
    // well under the 320MB AloopLoopDsp threshold that made heap allocation
    // mandatory there). Published via g_sampler so the MIDI/control thread's
    // note-65/66/keybed dispatch (apc_grid.cpp) can push events into it.
    auto samplerPtr = std::make_unique<Sampler>();
    g_sampler = samplerPtr.get();
    std::vector<int32_t> samplerBuf((size_t)N, 0);   // s32 scratch for captureBlock/renderInto
    // captureFin: a SEPARATE snapshot of dry input + SHIFT/glitch-folded
    // loop content, EXCLUDING the sampler's own renderInto playback voices
    // -- see the sampler-capture block below for why this must be distinct
    // from `fin` (which by capture time also carries this block's own
    // just-mixed-in sample playback, a genuine self-recording risk a first
    // draft of this fix introduced and caught before it shipped).
    std::vector<float> captureFin((size_t)N, 0.0f);
#endif

    // The user's swappable effect(s): an in-process LV2 host loading any bundle
    // from /effects/user, pinned to the free core (Core 3). Runs AFTER the home
    // stack, in the same block (no graph — zero added latency, ADR-002). A bad
    // user plugin is caught by the host's watchdog and bypassed.
    Lv2Host userFx;
    userFx.loadDir(g_cfg.userDir, g_cfg.userFxCore);   // honor aloop.conf [effects] user_dir + core
    userFx.connect(N, ch);

#ifdef ALOOP_HAVE_ALSA
    snd_pcm_t *cap = nullptr, *play = nullptr;
    // TWO distinct devices, matching looper's split exactly (ADR-015):
    //   - instrumentDevice (default hw:0,0, e.g. the M-Audio AIR 192|4): the
    //     REAL tight-latency capture+playback path a musician plugs an
    //     instrument/mic into and actually hears. `cap`/`play` below are
    //     ALWAYS this device — never the OTG gadget.
    //   - audioDevice (the f_uac2 OTG gadget, hw:UAC2Gadget,0): a best-effort
    //     MIRROR of the same processed output, opened separately below
    //     (`otgPlay`) and written non-blocking so an absent/slow/non-streaming
    //     OTG host can never stall or desync the instrument device's
    //     real-time path (looper: AudioOutputUSB is the graph's real output;
    //     AudioOutputOTG is a passive tap on the same ring with its own,
    //     looser-latency read cursor).
    const int kAlsaOpenRetries = 30;
    const char* wireDev = g_cfg.instrumentDevice.c_str();
    for (int attempt = 0; attempt < kAlsaOpenRetries; attempt++) {
        if (snd_pcm_open(&cap,  wireDev, SND_PCM_STREAM_CAPTURE,  0) == 0 &&
            snd_pcm_open(&play, wireDev, SND_PCM_STREAM_PLAYBACK, 0) == 0) break;
        if (cap)  { snd_pcm_close(cap);  cap  = nullptr; }
        if (play) { snd_pcm_close(play); play = nullptr; }
        if (attempt == 0) fprintf(stderr, "[audio] ALSA open of %s failed — is the instrument USB audio interface plugged in? retrying...\n", wireDev);
        struct timespec ts{1, 0}; nanosleep(&ts, nullptr);
    }
    if (!cap || !play) {
        fprintf(stderr, "[audio] ALSA still unavailable after %ds — is the instrument USB audio interface plugged in? audio stays down until restart\n", kAlsaOpenRetries);
    } else {
        // Explicit hw_params targeting the REAL block_size (N frames/period), not
        // ALSA's snd_pcm_set_params() convenience call — that call picks whatever
        // period/buffer satisfies a requested LATENCY (previously 20ms), which both
        // ignores block_size entirely and, combined with opening "default" (routed
        // through the dmix/dsnoop plugin's own large fixed period), was the actual
        // cause of the "massive latency vs looper" symptom: the wire path was
        // running at ~20ms+ per direction instead of the intended N/sampleRate
        // (1.33ms at the default 64-sample block).
        //
        // WITNESSED live on a real Pi 4: an initial 2-period buffer (256 frames
        // total, the ALSA minimum) produced 690 xruns within seconds — too tight
        // for a USB gadget PCM, where each read/write also rides USB's own
        // transfer-scheduling jitter on top of this thread's SCHED_FIFO jitter
        // (unlike looper's bare-metal Circle build, which has no OS/USB-stack
        // contention at all). 4 periods (256 total *frames of headroom* — i.e.
        // 4*N frames of buffer against the same N-frame period/wakeup size) keeps
        // the same per-period 1.33ms granularity (the actual latency-determining
        // number) while giving the ring enough slack to absorb that jitter
        // without underrunning constantly.
        auto configurePcm = [&](snd_pcm_t* pcm) -> bool {
            snd_pcm_hw_params_t* hw;
            snd_pcm_hw_params_alloca(&hw);
            snd_pcm_hw_params_any(pcm, hw);
            snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
            // The instrument device is S32_LE (24-bit data left-justified in a
            // 32-bit word — most class-compliant USB audio interfaces, including
            // the M-Audio AIR 192|4, have no S16_LE mode at all). WITNESSED live:
            // requesting S16_LE here previously succeeded at the snd_pcm_hw_params()
            // call (no error returned) while the device silently negotiated S32_LE
            // anyway — the return value of set_format() itself was never checked,
            // so the mismatch went undetected until it produced loud static on
            // real hardware. Now explicit, and the buffer type (int32_t `buf`,
            // declared above) matches.
            if (snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE) < 0)
                fprintf(stderr, "[audio] warning: instrument device rejected S32_LE format request\n");
            snd_pcm_hw_params_set_channels(pcm, hw, wireCh);
            unsigned int rate = (unsigned int)g_cfg.sampleRate;
            snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
            snd_pcm_uframes_t period = (snd_pcm_uframes_t)N;
            snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
            snd_pcm_uframes_t bufSize = period * 4;
            snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufSize);
            if (snd_pcm_hw_params(pcm, hw) < 0) return false;
            // Verify the format actually negotiated matches what buf's element
            // type assumes — a silent mismatch here is exactly the loud-static bug.
            snd_pcm_format_t negotiatedFmt;
            if (snd_pcm_hw_params_get_format(hw, &negotiatedFmt) == 0 && negotiatedFmt != SND_PCM_FORMAT_S32_LE)
                fprintf(stderr, "[audio] warning: instrument device negotiated format %s, not S32_LE — audio will be corrupted (buf is int32_t)\n",
                        snd_pcm_format_name(negotiatedFmt));
            if (period != (snd_pcm_uframes_t)N)
                fprintf(stderr, "[audio] warning: device would not grant period=%d frames, got %lu — latency will not match block_size\n",
                        N, (unsigned long)period);
            // sw_params: the hw_params default start_threshold for a PLAYBACK
            // stream is the full buffer_size — WITNESSED live on a real Pi 4:
            // /proc/asound/.../pcm0p/sub0/status stayed stuck in "PREPARED"
            // forever (never auto-started), because this loop only ever writes
            // one N-frame period per snd_pcm_writei() call and immediately
            // blocks on the next capture read, so the ring never reached a full
            // buffer_size of queued frames to cross that default threshold —
            // meanwhile CAPTURE (which starts as soon as ANY data is available,
            // not gated on a full buffer) ran fine, so the two streams silently
            // desynced and playback undor-ran on every single write. Lowering
            // start_threshold to exactly one period means playback triggers on
            // the very first snd_pcm_writei(), matching how capture already
            // behaves.
            snd_pcm_sw_params_t* sw;
            snd_pcm_sw_params_alloca(&sw);
            snd_pcm_sw_params_current(pcm, sw);
            snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
            snd_pcm_sw_params_set_avail_min(pcm, sw, period);
            if (snd_pcm_sw_params(pcm, sw) < 0)
                fprintf(stderr, "[audio] warning: sw_params (start_threshold) rejected — playback may not auto-start\n");
            return true;
        };
        if (!configurePcm(cap) || !configurePcm(play))
            fprintf(stderr, "[audio] warning: explicit hw_params rejected by %s — falling back to driver defaults (higher latency)\n", wireDev);
        snd_pcm_prepare(cap);
        snd_pcm_prepare(play);

        // OTG gadget mirror output: opened NONBLOCK so a missing/non-streaming
        // host on the other end of the OTG cable (the common case — see
        // ADR-014's idle-USB-audio-class finding) never blocks this thread. A
        // failed open here is silent-degrade-only: the instrument-device path
        // above is already fully functional without it, matching looper's
        // "OTG is a passive mirror, never the graph's real output" design.
        snd_pcm_t* otgPlay = nullptr;
        bool otgReady = false;
        if (snd_pcm_open(&otgPlay, g_cfg.audioDevice.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0) {
            snd_pcm_hw_params_t* ohw;
            snd_pcm_hw_params_alloca(&ohw);
            snd_pcm_hw_params_any(otgPlay, ohw);
            snd_pcm_hw_params_set_access(otgPlay, ohw, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(otgPlay, ohw, SND_PCM_FORMAT_S16_LE);
            snd_pcm_hw_params_set_channels(otgPlay, ohw, wireCh);
            unsigned int otgRate = (unsigned int)g_cfg.sampleRate;
            snd_pcm_hw_params_set_rate_near(otgPlay, ohw, &otgRate, nullptr);
            // Looser latency target than the instrument device on purpose
            // (looper: OTG_LAG_TARGET=384 vs the real path's 96, 4x headroom) —
            // the OTG side doesn't need tight timing, just enough buffer that
            // its own USB-gadget scheduling jitter doesn't underrun constantly
            // (WITNESSED: a period matching block_size alone produced hundreds
            // of xruns/sec on the gadget path even with a real host attached).
            snd_pcm_uframes_t otgPeriod = (snd_pcm_uframes_t)N * 4;
            snd_pcm_hw_params_set_period_size_near(otgPlay, ohw, &otgPeriod, nullptr);
            snd_pcm_uframes_t otgBufFrames = otgPeriod * 4;
            snd_pcm_hw_params_set_buffer_size_near(otgPlay, ohw, &otgBufFrames);
            if (snd_pcm_hw_params(otgPlay, ohw) == 0) {
                snd_pcm_sw_params_t* osw;
                snd_pcm_sw_params_alloca(&osw);
                snd_pcm_sw_params_current(otgPlay, osw);
                snd_pcm_sw_params_set_start_threshold(otgPlay, osw, otgPeriod);
                snd_pcm_sw_params_set_avail_min(otgPlay, osw, otgPeriod);
                snd_pcm_sw_params(otgPlay, osw);
                snd_pcm_prepare(otgPlay);
                otgReady = true;
            }
        }
        if (!otgReady) {
            fprintf(stderr, "[audio] OTG gadget mirror (%s) unavailable — instrument-device audio is unaffected, gadget mirror stays off until it appears\n", g_cfg.audioDevice.c_str());
            if (otgPlay) { snd_pcm_close(otgPlay); otgPlay = nullptr; }
        }

        while (g_running.load()) {
            snd_pcm_sframes_t r = snd_pcm_readi(cap, buf.data(), N);
            if (r < 0) { g_telem.xruns++; snd_pcm_recover(cap, (int)r, 1); continue; }

            // === run the Faust home stack (loop engine + effects) this block ===
            // s16 -> float, compute(), float -> s16. The Faust program does the
            // record/play loop + the effects in one pass. The user-FX LV2 (Core 3)
            // runs after via the in-process host (host.runBlock), joined this block.
#ifdef ALOOP_HAVE_FAUST_LOOP
            // Apply the remappable controls: for each bound target the MIDI map
            // set, push its current value into the matching Faust zone. Done once
            // per block from the atomic store — no locks, no alloc (the name→zone
            // strings resolve cheaply; a production build caches name→float* once).
            if (g_params) {
                g_params->forEach([&](const std::string& target, int){
                    std::string zone = targetToZone(target);
                    if (!zone.empty()) fui.set(zone.c_str(), g_params->get(target));
                });
                // Global commands (cmd/*) are NOT per-looper Faust zones — they drive
                // the engine-wide clear/speed process() SIGNAL INPUTS directly (the
                // hardware's CLEARALL + momentary HALFSPEED/DOUBLESPEED). Held (value
                // 1) = active; released = neutral. Double wins if both are somehow
                // held. Filled as a CONSTANT across the whole block into
                // clearBuf/speedBuf (fins[2]/fins[3]) rather than via fui.set() --
                // see fins[] comment above for why: a UI zone here would be
                // duplicated 20x by loop.dsp's par() replication (WITNESSED via
                // generated C++ even after 382e775's earlier attempted fix), a
                // plain signal input cannot be.
                bool clearAllHeld = g_params->get("cmd/clearall") > 0.5f;
                std::fill(clearBuf.begin(), clearBuf.end(), clearAllHeld ? 1.0f : 0.0f);
                // CLEAR_ALL also resets the locally-established phrase length
                // (cmd/master_len, set by apc_grid.cpp's applyRecPlayCycle) so
                // the NEXT standalone recording re-establishes a fresh phrase
                // from scratch -- matches looper's LOOP_COMMAND_CLEAR_ALL
                // resetting masterLoopBlocks to 0 (loopMachine.cpp:325-330,
                // 411-412) so a cleared rig's first new loop defines a new
                // grid rather than inheriting the previous session's length.
                if (clearAllHeld) {
                    g_params->setByName("cmd/master_len", 0.0f);
                    g_params->setByName("cmd/recorded_bpm", 0.0f);   // see apc_grid.cpp's "TRUE varispeed" comment
                }
                // manualSpeedMul: PURELY the manual half/double-speed button
                // state (1.0/0.5/2.0) -- confirmed via reading looper's real
                // g_globalSpeedMul (loopMachine.cpp:530-545): it is set ONLY
                // by these two buttons, never touched by Link/tempo code.
                // Combined with the Link-driven ratio (g_linkSpeedRatio,
                // computed in the "Varispeed Link sync" block below, which
                // runs AFTER this) into the single effSpeed signal pushed to
                // loop.dsp's read accumulator -- see speedBuf's declaration
                // comment above for why this is one combined signal.
                float manualSpeedMul = 1.0f;
                if (g_params->get("cmd/halfspeed")   > 0.5f) manualSpeedMul = 0.5f;
                if (g_params->get("cmd/doublespeed") > 0.5f) manualSpeedMul = 2.0f;
                g_manualSpeedMul = manualSpeedMul;
                // NOTE: LOOP_IMMEDIATE / SET_LOOP_START / CLEAR_LOOP_START (mark-point
                // restart) are NOT wired — they need an addressable read head, which
                // the Faust feedback-delay looper does not have (a preserve-on-hold
                // rwtable playhead is a read-modify-write Faust rejects; see loop.dsp
                // header + docs/COMMAND-SURFACE.md). Deliberate model difference.
                // Global STOP-ALL (hardware LOOP_COMMAND_STOP 0x03): clear every
                // looper's play checkbox so all playback stops. Per-looper stop is
                // already covered by binding a control to looper<i>/play (=0 stops
                // that one); this is the single all-tracks command. Edge-triggered
                // on the held value so it doesn't fight a user re-arming a looper.
                if (g_params->get("cmd/stopall") > 0.5f) {
                    char z[32];
                    for (int lp = 0; lp < 20; lp++) {
                        snprintf(z, sizeof z, "looper%2d/play", lp);
                        fui.set(z, 0.0f);
                    }
                }
                // APC live-pitch (CC1 mod-wheel / CC52 absolute, apc_grid.cpp): a
                // performance offset ON TOP of the static SEMIS knob (fx/pitch), so
                // add rather than overwrite — releasing the mod-wheel (engaged=0)
                // must fall back to the static knob value, not silently zero it.
                float staticSemis = g_params->get("fx/pitch");
                if (g_params->get("fx/pitchbend_engaged") > 0.5f) {
                    fui.set("SEMIS", staticSemis + g_params->get("fx/pitchbend"));
                    fui.set("ENGAGED", 1.0f);
                } else {
                    fui.set("SEMIS", staticSemis);
                }
                // Microrepeat latch (apc_grid.cpp notes 82-86) -> the microStage's
                // DIV zone (dsp/effects_runtime.dsp; was hardcoded 0, never wired).
                fui.set("DIV", g_params->get("fx/microrepeat_div"));
            }
            // Per-looper STATE telemetry (the GET_STATE 0x30 equivalent): read each
            // looper's rec/play/vol back from the Faust zones into the atomic
            // snapshot the control thread serves on udp/4445. Cheap (60 map lookups
            // once/block); a production build would cache the float* on first block.
            {
                char z[32];
                for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) {
                    snprintf(z, sizeof z, "looper%2d/rec",  lp); g_telem.looperRec[lp]  = fui.get(z) > 0.5f;
                    snprintf(z, sizeof z, "looper%2d/play", lp); g_telem.looperPlay[lp] = fui.get(z) > 0.5f;
                    snprintf(z, sizeof z, "looper%2d/vol",  lp); g_telem.looperVol[lp]  = fui.get(z, 1.0f);
                    snprintf(z, sizeof z, "looper%2d/level", lp); g_telem.looperLevel[lp] = fui.get(z, 0.0f);
                    // writeIdx telemetry (ARM-QUANTIZATION compensation, see
                    // dsp/loop.dsp's writeIdxMeter comment): the TRUE elapsed
                    // sample count since the real (grid-quantized) arm
                    // instant, read at the control thread's own poll rate --
                    // apc_grid.cpp reads this at the FINISH press to compute
                    // rawSamples precisely instead of estimating from
                    // wall-clock press-to-press timing (which would be
                    // biased by however long ARM-quantization's grid-tick
                    // wait took).
                    snprintf(z, sizeof z, "looper%2d/writeidx", lp); g_telem.looperWriteIdx[lp] = fui.get(z, 0.0f);
                }
            }
            // REMOVED (dsp/loop.dsp's readposdiag/wraplendiag no longer
            // exist): the diagnostic hbargraphs this log line read never
            // worked correctly across two prior attempts (always -1.0
            // "not found") and were removed from loop.dsp itself alongside
            // the wrapLen-latching fix for the total-playback-silence
            // regression -- see loop.dsp's oneLooper comment for why.
            // monitorMode telemetry (apcKey25.cpp:361's p.monitorMode = m_shift):
            // read directly from ParamStore now (no more MONITORFOLD Faust
            // zone -- the fold is a native block-rate mix, see prevLoopSum
            // above) so a dev host can still observe SHIFT/monitor-fold state live.
            g_telem.monitorMode = g_params && g_params->get("fx/monitorfold") > 0.5f;
            // Varispeed Link sync: when synced, set every looper's loop length from
            // the Link tempo (a musical phrase = a whole number of beats). A tempo
            // change resizes the loops so they stay locked to the session — the
            // same behavior as the original looper's masterLoopBlocks recompute.
            bool linkDrivingLength = false;
            if (g_link) {
                LinkSnapshot ls = g_link->audioRead();
                // publish the live Link state into the telemetry snapshot (atomic
                // plain-old-data; the control thread reads it for udp/4445).
                g_telem.linkSynced = ls.synced;
                g_telem.bpm = ls.bpm;
                if (ls.synced && ls.bpm > 1.0) {
                    linkDrivingLength = true;
                    // one bar (4 beats) as the phrase, rounded to whole blocks.
                    double beatsPerBar = 4.0;
                    double samplesPerBeat = (g_cfg.sampleRate * 60.0) / ls.bpm;
                    double lenSamples = samplesPerBeat * beatsPerBar;
                    char z[32];
                    for (int lp = 0; lp < 20; lp++) {
                        snprintf(z, sizeof z, "looper%2d/len", lp);
                        fui.set(z, (float)lenSamples);
                    }
                    // Microrepeat's MLB (masterLoopBlocks, effects_runtime.dsp): the
                    // same phrase length expressed in DSP blocks, so a repeat slice
                    // (lenSamples/DIV) stays grid-aligned with the loop.
                    fui.set("MLB", (float)(lenSamples / N));
                }
            }
            if (!linkDrivingLength && g_params) {
                // WITNESSED live + confirmed via cross-codebase research
                // against ../looper (loopClip.cpp:64-66,219,243): looper's
                // masterLoopBlocks/microrepeat grid NEVER requires an
                // Ableton Link session -- it's established locally from
                // the first recorded loop's own duration. Forcing MLB=0
                // here whenever Link isn't synced (or absent entirely) left
                // microrepeat/glitch completely inert on any standalone
                // (no-Link) session -- exactly the reported "glitch buttons
                // didn't work ... nothing happened". WITNESSED via
                // cross-codebase research (a second pass, after the first
                // fix): the original fix nested this fallback INSIDE
                // `if (g_link)`, so it never ran at all when g_link itself
                // was null (Link object never constructed/unavailable), not
                // just when it existed-but-unsynced -- moved out to run
                // unconditionally whenever Link isn't the active length
                // driver, covering both cases. apc_grid.cpp's
                // applyRecPlayCycle publishes the locally-established phrase
                // length to "cmd/master_len" (0 = none established yet,
                // matching looper's masterLoopBlocks==0 empty-rig case).
                float masterLen = g_params->get("cmd/master_len", 0.0f);
                fui.set("MLB", masterLen > 0.0f ? (masterLen / (float)N) : 0.0f);
            }
            // TRUE varispeed: Link-tempo-driven READ RATE (distinct from the
            // loop-LENGTH resize above). Ports looper's real per-clip formula
            // (Looper.h:301-305's setMasterBlocks: `m_playRate =
            // m_nativeBlocks / newMaster`, recomputed ABSOLUTELY every tempo
            // change, never accumulated -- confirmed via reading
            // C:\dev\looper\Looper.h and loopMachine.cpp:637-663 this
            // session) -- equivalently `recordedBpm / currentLinkBpm`, since
            // both nativeBlocks and newMaster are themselves derived from a
            // BPM via the same block-length formula. aloop shares ONE master
            // phrase across the whole rig (unlike looper's per-clip
            // tracking, see g_manualSpeedMul's declaration comment), so this
            // is a SINGLE ratio applied to every looper, not a per-looper
            // value -- architecturally correct given aloop's existing
            // shared-phrase design, not a simplification of looper's.
            // "cmd/recorded_bpm" is written by apc_grid.cpp's
            // applyRecPlayCycle at the SAME moment cmd/master_len is first
            // established (deriveTempoBpm(masterLenSamples/sampleRate)),
            // exactly mirroring looper's m_nativeBlocks being locked in at
            // _finishRecording and never changing while the clip lives.
            // linkSpeedRatio stays 1.0 (no-op) whenever there's no recorded
            // phrase yet OR Link isn't actively driving length this block --
            // an unsynced/no-Link session must not silently alter pitch.
            float linkSpeedRatio = 1.0f;
            if (linkDrivingLength && g_params && g_link) {
                float recordedBpm = g_params->get("cmd/recorded_bpm", 0.0f);
                LinkSnapshot ls2 = g_link->audioRead();
                if (recordedBpm > 1.0f && ls2.bpm > 1.0) {
                    linkSpeedRatio = recordedBpm / (float)ls2.bpm;
                }
            }
            // effSpeed = manualSpeedMul * linkSpeedRatio, matching looper's
            // effectiveRate = m_playRate * g_globalSpeedMul exactly (see
            // speedBuf's declaration comment above). Filled as a CONSTANT
            // across the block -- both factors are momentary/slow-changing
            // (button state, once-per-tempo-change ratio), not audio-rate.
            {
                float effSpeed = g_manualSpeedMul * linkSpeedRatio;
                std::fill(speedBuf.begin(), speedBuf.end(), effSpeed);
                g_telem.effSpeed = effSpeed;
            }
            // MASTER PHASE CLOCK (5th process() signal input, see
            // masterPhaseBuf's own declaration comment above): a single
            // sample-accurate counter, incremented by N every block,
            // wrapping at the current shared phrase length (cmd/master_len,
            // the SAME value dsp/loop.dsp's per-looper wrapLen is ultimately
            // derived from at each looper's own finishEdge) -- this is what
            // every looper's recordStartPhaseOffset anchors to, guaranteeing
            // two loopers can never drift apart from each other regardless
            // of glitch/repeat/varispeed engagement (see dsp/loop.dsp's
            // oneLooper comment for the full Faust-side reasoning).
            // Genuinely internal (not read back from Faust) -- computed
            // ONCE here, pushed in as a plain wire, exactly like
            // clearBuf/speedBuf.
            //
            // Link resync: when a real Link session is active and driving
            // the phrase length (linkDrivingLength, computed above from the
            // SAME g_link->audioRead() this block already has), periodically
            // correct any accumulated float/scheduling drift by re-deriving
            // the phase from Link's own beatPhaseMicroBeats -- mirrors
            // ../looper's own periodic re-anchor (loopClipUpdate.cpp:283-296,
            // confirmed via cross-codebase research this session) that snaps
            // even a self-advancing accumulator back to the master-derived
            // absolute value at each phrase boundary. Standalone (no Link
            // peers), masterPhase is purely the free-running block counter --
            // still fully functional (aloop remains phrase-locked to ITSELF
            // even with no Link session), just without an external reference
            // to correct against.
            {
                static double masterPhaseSamples = 0.0;
                float masterLen = g_params ? g_params->get("cmd/master_len", 0.0f) : 0.0f;
                if (masterLen > 0.0f) {
                    if (linkDrivingLength && g_link) {
                        LinkSnapshot ls3 = g_link->audioRead();
                        if (ls3.phaseValid && ls3.quantumMicroBeats > 0) {
                            // Link's phase is a fraction of ITS OWN quantum
                            // (beats); convert to a fraction of OUR phrase
                            // (samples) and resync -- this is the periodic
                            // drift-correction snap, applied every block a
                            // valid Link phase is available (cheap, and
                            // exactly matches looper's own "every phrase
                            // boundary" cadence closely enough at block
                            // granularity).
                            double linkPhaseFrac = (double)ls3.beatPhaseMicroBeats / (double)ls3.quantumMicroBeats;
                            if (linkPhaseFrac < 0.0) linkPhaseFrac = 0.0;
                            if (linkPhaseFrac >= 1.0) linkPhaseFrac = 0.0;
                            masterPhaseSamples = linkPhaseFrac * (double)masterLen;
                        } else {
                            masterPhaseSamples += (double)N;
                        }
                    } else {
                        masterPhaseSamples += (double)N;
                    }
                    masterPhaseSamples = std::fmod(masterPhaseSamples, (double)masterLen);
                    if (masterPhaseSamples < 0.0) masterPhaseSamples += masterLen;
                } else {
                    masterPhaseSamples = 0.0;   // no phrase established yet -- hold at 0
                }
                // WITNESSED live: "loops now sound bitcrushed" -- ROOT CAUSE:
                // std::fill() previously pushed the SAME block-start value
                // into every one of the N samples in masterPhaseBuf, exactly
                // like the genuinely block-constant clearBuf/speedBuf
                // (correct for those, since they're momentary/slow-changing
                // commands, not per-sample position data). But
                // dsp/loop.dsp's absPos formula treats masterPhase as this
                // looper's actual READ POSITION at effSpeed==1.0 -- holding
                // it constant for a whole 64-sample block meant readIdx0/
                // readIdx1 never advanced WITHIN a block, only jumping 64
                // samples at each block boundary: a stepped/aliased readback
                // pattern, audibly indistinguishable from bitcrushing. Fix:
                // ramp masterPhaseBuf smoothly WITHIN the block (each sample
                // i gets masterPhaseSamples + i, wrapped at masterLen), so
                // absPos genuinely advances one sample per sample exactly
                // like the old self-integrating readPos did -- masterPhase
                // is a real per-sample POSITION signal now, not a
                // momentary/step control like clearBuf/speedBuf.
                if (masterLen > 0.0f) {
                    for (int i = 0; i < N; i++) {
                        double p = masterPhaseSamples + (double)i;
                        p = std::fmod(p, (double)masterLen);
                        if (p < 0.0) p += masterLen;
                        masterPhaseBuf[(size_t)i] = (float)p;
                    }
                } else {
                    std::fill(masterPhaseBuf.begin(), masterPhaseBuf.end(), 0.0f);
                }
                // masterLenBuf: block-constant, feeds dsp/loop.dsp's new
                // arm-quantization gridStep calculation (see masterLenBuf's
                // own declaration comment above).
                std::fill(masterLenBuf.begin(), masterLenBuf.end(), masterLen);
            }
            // Deinterleave the stereo wire -> mono DSP input: average the wire
            // channels (mono content is identical L/R; a stereo source is summed to
            // mono, matching the looper's mono internal path). wireCh==1 degenerates
            // to a straight copy. `buf` is S32_LE (24-bit data left-justified in the
            // top of a 32-bit word — confirmed via /proc/asound stream0 "Bits: 24"),
            // so normalize by INT32_MAX-equivalent range (2147483648.0), NOT 32768 —
            // using the s16 divisor here was the earlier "loud static" bug: it
            // treated 32-bit samples as if they were 16-bit magnitude, producing
            // values ~65536x too large before Faust even saw them.
            float inPeak = 0.0f;
            for (int i = 0; i < N; i++) {
                float acc = 0.0f;
                for (int c = 0; c < wireCh; c++) acc += (float)buf[(size_t)i * wireCh + c];
                fin[i] = (acc / wireCh) / 2147483648.0f;
                float a = fin[i] < 0 ? -fin[i] : fin[i];
                if (a > inPeak) inPeak = a;
            }
            g_telem.inPeak = inPeak;
            // Snapshot dry input for the sampler CAPTURE path (see below) --
            // taken BEFORE renderInto's playback voices are mixed into `fin`,
            // so a sample recording can never contain this block's own
            // just-triggered sample playback, exactly matching sampler.h's
            // documented no-self-recording invariant. The SHIFT/glitch fold
            // is applied to this buffer too, in the SAME fold loop below, so
            // captureFin ends up as "dry input + folded loop content, no
            // sampler voices" -- distinct from `fin` (dry input + sampler
            // voices + folded loop content, the correct DSP-facing signal).
            captureFin = fin;
            // Sampler PLAYBACK mix-in (sampler.h's own load-bearing invariant,
            // verbatim from ../looper): voices are mixed in BEFORE the loop
            // engine + effects chain, so a played-back sample gets all
            // effects and is recordable by a loop under SHIFT fold, matching
            // looper's renderInto-before-the-chain ordering exactly. Capture
            // (recording INTO a sample slot) is handled SEPARATELY below,
            // after the SHIFT/glitch fold -- see that comment for why.
            for (int i = 0; i < N; i++) samplerBuf[(size_t)i] = (int32_t)(fin[i] * 32768.0f);
            g_sampler->renderInto(samplerBuf.data(), N);
            for (int i = 0; i < N; i++) fin[i] = (float)samplerBuf[(size_t)i] / 32768.0f;
            // SHIFT-held monitor-fold (native mix, see prevLoopSum's declaration
            // comment above for why this isn't done as a Faust graph cycle):
            // fold the PREVIOUS block's RAW loop-engine output into this
            // block's input BEFORE faustHome.compute() runs, so loop.dsp's
            // record path (which runs first inside that call) sees the
            // folded signal -- matching looper's m_input_buffer += fold
            // mutation persisting into every subsequent block. Also mirrors
            // looper's per-sample-interpolated ramp (MONITOR_GATE_STEP=1/16
            // per block) with a simple one-pole smoother toward the held/
            // released target, avoiding a hard step at the fold boundary.
            if (g_params) {
                static float foldGain = 0.0f;
                float foldTarget = g_params->get("fx/monitorfold") > 0.5f ? 1.0f : 0.0f;
                const float kFoldStep = 1.0f / 16.0f;   // reach target over ~16 blocks, matching looper's own step
                // GLITCH-HELD loop-routing fold: same native one-block-lag
                // mechanism as the SHIFT-fold immediately above (reusing the
                // exact same prevLoopSum buffer, per user's explicit request
                // not to invent a new signal-flow mechanism), but gated by
                // "glitch engaged" (fx/microrepeat_div > 0, the 5 microrepeat
                // latch notes 82-86 via apc_grid.cpp's onMicrorepeatOn/Off)
                // instead of SHIFT-held. User-confirmed requirement: "glitch
                // should replace normal loop output while held" -- i.e. while
                // microrepeat is engaged, loop content must route INTO fx
                // (hitting microStage) and the NORMAL direct/raw loop output
                // must stop being separately audible, not play alongside it.
                static float glitchFoldGain = 0.0f;
                float glitchFoldTarget = g_params->get("fx/microrepeat_div") > 0.5f ? 1.0f : 0.0f;
                for (int i = 0; i < N; i++) {
                    if (foldGain < foldTarget)      { foldGain += kFoldStep / N; if (foldGain > foldTarget) foldGain = foldTarget; }
                    else if (foldGain > foldTarget) { foldGain -= kFoldStep / N; if (foldGain < foldTarget) foldGain = foldTarget; }
                    if (glitchFoldGain < glitchFoldTarget)      { glitchFoldGain += kFoldStep / N; if (glitchFoldGain > glitchFoldTarget) glitchFoldGain = glitchFoldTarget; }
                    else if (glitchFoldGain > glitchFoldTarget) { glitchFoldGain -= kFoldStep / N; if (glitchFoldGain < glitchFoldTarget) glitchFoldGain = glitchFoldTarget; }
                    // SHIFT and glitch can be held simultaneously -- combine by
                    // summing the two fold contributions into `fin`, clamped to
                    // 1.0 (matching a single hold's ramp ceiling) so a
                    // simultaneous hold folds loop content in at most once at
                    // full gain, never double-strength. This is the SAME
                    // `fin[i] +=` term as before, just fed by the max of the two
                    // gains rather than foldGain alone, so a listener holding
                    // BOTH controls hears exactly what holding either alone
                    // would (loop content routed through fx once), not two
                    // stacked copies.
                    float combinedFold = foldGain + glitchFoldGain;
                    if (combinedFold > 1.0f) combinedFold = 1.0f;
                    fin[i] += prevLoopSum[i] * combinedFold;
                    // Apply the IDENTICAL fold to captureFin (dry input, no
                    // sampler voices) so the sampler's capture path sees the
                    // same SHIFT/glitch-folded loop content as the DSP's own
                    // `fin` does, without also picking up this block's
                    // sampler playback -- see captureFin's declaration
                    // comment and the sampler-capture block below.
                    captureFin[i] += prevLoopSum[i] * combinedFold;
                }
                // Push foldGain into aloop.dsp's MONITORFOLD zone (a real,
                // live-written zone again -- see dsp/aloop.dsp's REGRESSION
                // FOUND AND FIXED comment) so the direct raw-loopSum term
                // there fades out exactly as the native SHIFT-fold fades in,
                // instead of ever summing both at once. This write MUST
                // happen every block (not just when g_params exists but only
                // conditionally) or the zone reverts to a dead one exactly
                // like the bug this fixes.
                fui.set("MONITORFOLD", foldGain);
                // Push glitchFoldGain into aloop.dsp's new GLITCHFOLD zone
                // (mirrors MONITORFOLD exactly) so the direct raw-loopSum
                // term ALSO fades out while glitch is held, complementing
                // the same fx-routed loop content the fin[] fold above just
                // added -- same crossfade shape as SHIFT, gated by glitch
                // engagement instead.
                fui.set("GLITCHFOLD", glitchFoldGain);
            }
            // Sampler CAPTURE (recording INTO a sample slot): user's explicit
            // request this session -- "shift should route loops into the
            // sample recording, for drums and all-key samples to be able to
            // record from loopers the same way that input and effects get
            // recorded into samplers, since loopers play into the input
            // channel its a surprise it doesnt already do this." ROOT CAUSE
            // (confirmed via investigation this session): captureBlock used
            // to run BEFORE the SHIFT/glitch fold, using dry `fin[]` alone --
            // so a sample recording could never contain loop content, folded
            // or not, regardless of SHIFT state. FIRST FIX ATTEMPT (caught
            // and corrected before shipping): simply moving captureBlock to
            // run on `fin[]` AFTER the fold reintroduced a genuine
            // self-recording bug, since `fin[]` by this point ALSO contains
            // THIS block's own renderInto-mixed sample playback voices -- a
            // sample recorded while another sample/drum hit is playing would
            // record itself. REAL FIX: capture from `captureFin` instead, a
            // SEPARATE buffer snapshotted from dry input BEFORE renderInto
            // ever touches it, with the SAME SHIFT/glitch fold applied to it
            // independently (see captureFin's declaration and the fold loop
            // above) -- dry input + folded loop content, deliberately
            // excluding this block's own sampler playback. No lag/whine risk
            // (unlike the loop-engine's own `fin[] +=` recursion class this
            // file's history warns about): captureBlock is a pure
            // capture-into-buffer, never fed back into any Faust input, so
            // it structurally cannot re-enter the effects chain later.
            for (int i = 0; i < N; i++) samplerBuf[(size_t)i] = (int32_t)(captureFin[i] * 32768.0f);
            g_sampler->captureBlock(samplerBuf.data(), N);
            // Recording: ALWAYS captures the fully-effected mix, via a
            // dedicated record-only Faust input, never by folding into
            // `fin`. WITNESSED-BROKEN prior approach (this file's history,
            // originally for glitch alone): feeding a post-fx tap into `fin`
            // made it become `dry` for `loop` next block, which then flows
            // through `fx` AGAIN every block -- stages re-processing their
            // own prior output produced a fast, aliased whine, confirmed
            // structural, not stage-specific. Fixed by giving loop.dsp's
            // process() a SECOND input (prevFiltIn, see fins[1] above) that
            // ONLY the record-capture term consumes (dsp/loop.dsp's
            // oneLooper: record = prevFiltIn * recN) -- prevFiltOut never
            // joins the dry/live path, so it can never flow back through
            // `fx` on this or any later block. This REPLACES the old
            // glitch-only prevGlitchTap wiring: prevFiltOut already contains
            // post-glitch content one block later (microStage feeds both
            // filterStage and rawGlitchTap in effects_runtime.dsp, so
            // microStage's output is upstream of and baked into filtOut),
            // so a separate glitch term would have double-counted glitch
            // content once both taps were live. prevFiltOut is passed
            // directly as fins[1] to compute() below (no fin[i] += needed,
            // unlike the SHIFT-fold above, since it targets a separate Faust
            // input, not `fin` itself).
            // Sidechain envelope (LOFI feature, 7th process() input): the
            // multi-source max/peak-combined ducking envelope. Sources are
            // designated via ApcGrid::onSidechainLooperToggle (guitar-fx hold
            // + looper press), which writes looperN/sidechainsrc into
            // ParamStore. Slot indices are resolved lazily (see
            // sidechainSrcSlot's own declaration comment above this loop) --
            // getSlot() is only ever called for a still-unresolved (-1) slot,
            // so once bindAll (a separate startup-only, non-hot-path thread
            // event) registers all 20 names, every slot resolves within one
            // block and the map-lookup path never runs again. Combined with
            // g_telem.looperLevel[] (this PREVIOUS block's own peak-envelope
            // telemetry, populated further up in this same loop) via max(),
            // matching the confirmed multi-source design: any active source
            // pumping loud enough ducks every non-source looper, sources
            // combine by whichever is currently louder, never summed (which
            // would double-duck when multiple sources hit simultaneously).
            // One-block-lag (this block's envelope reflects last block's
            // levels) -- the same staleness class already accepted for
            // prevFiltIn/prevLoopSum's own one-block-lag folds above, and
            // inaudible at audio rates for an envelope follower's own natural
            // smoothing time constant.
            {
                float sidechainEnv = 0.0f;
                if (g_params) {
                    for (int lp = 0; lp < AudioThread::Telemetry::kLoopers; lp++) {
                        if (sidechainSrcSlot[lp] < 0) {
                            char z[32];
                            snprintf(z, sizeof z, "looper%d/sidechainsrc", lp);
                            sidechainSrcSlot[lp] = g_params->getSlot(z);
                        }
                        if (g_params->getBySlot(sidechainSrcSlot[lp]) > 0.5f) {
                            float lvl = g_telem.looperLevel[lp];
                            if (lvl > sidechainEnv) sidechainEnv = lvl;
                        }
                    }
                }
                std::fill(sidechainEnvBuf.begin(), sidechainEnvBuf.end(), sidechainEnv);
            }
            // Time the DSP work vs the block budget → home-core busy % telemetry.
            timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            faustHome.compute(N, fins, fouts);
            // the user LV2(s) process the home-stack OUTPUT (fout) on the free
            // core, in the same block — zero added latency, no graph (ADR-002).
            // process() runs the plugin chain in place; passthrough if none loaded.
            userFx.process(fout.data(), N);
            // Snapshot this block's RAW loop output (rawLoopSum, aloop.dsp's
            // second process() output) for NEXT block's fold-in (see
            // prevLoopSum's declaration comment) -- deliberately NOT fout
            // (the fx-processed signal), so folded loop content doesn't
            // accumulate a compounding extra pass through the effects chain
            // every block the fold is held.
            prevLoopSum = rawLoopSum;
            // Snapshot this block's fully-effected mix output (rawFiltTap,
            // aloop.dsp's 4th process() output) for NEXT block's
            // always-effected record fold-in (see prevFiltOut's declaration
            // comment above). Deliberately a SEPARATE tap from fout (never
            // read fout here) even though they're numerically identical --
            // keeps the live audible path and the record-tap snapshot
            // structurally independent, matching the same discipline as
            // prevLoopSum above.
            prevFiltOut = rawFiltTap;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            // busy fraction = work / block-period. Smoothed (EWMA) so the readout is
            // stable. Stored on the home-FX core index; a spike toward 100% warns of
            // an impending xrun (RT-TUNING). Plain float store — read by control thd.
            {
                double workNs = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
                double periodNs = (double)N / g_cfg.sampleRate * 1e9;
                double pct = periodNs > 0 ? (workNs / periodNs) * 100.0 : 0.0;
                float& slot = g_telem.coreBusyPct[g_cfg.homeFxCore & 3];
                slot = slot * 0.9f + (float)pct * 0.1f;   // EWMA
            }
            // Interleave the mono DSP output onto every wire channel (mono
            // duplicated to L and R so a stereo host hears it centered). wireCh==1
            // degenerates to a straight copy. Two separate conversions since the
            // instrument device (S32_LE) and the OTG gadget (S16_LE, our own
            // configfs c_ssize/p_ssize=2) have genuinely different wire formats —
            // see the capture-side comment above for why a shared 16-bit buffer
            // caused loud static once the instrument device was opened at all.
            float outPeak = 0.0f;
            for (int i = 0; i < N; i++) {
                float v32 = fout[i] * 2147483648.0f;
                int32_t s32 = (int32_t)(v32 > 2147483647.0f ? 2147483647 : (v32 < -2147483648.0f ? -2147483648.0f : v32));
                float v16 = fout[i] * 32768.0f;
                int16_t s16 = (int16_t)(v16 > 32767 ? 32767 : (v16 < -32768 ? -32768 : v16));
                for (int c = 0; c < wireCh; c++) {
                    buf[(size_t)i * wireCh + c] = s32;
                    otgBuf[(size_t)i * wireCh + c] = s16;
                }
                float a = fout[i] < 0 ? -fout[i] : fout[i];
                if (a > outPeak) outPeak = a;
            }
            g_telem.outPeak = outPeak;
#endif

            snd_pcm_sframes_t w = snd_pcm_writei(play, buf.data(), N);
            if (w < 0) { g_telem.xruns++; snd_pcm_recover(play, (int)w, 1); }

            // OTG mirror: best-effort, never allowed to affect the instrument
            // device's timing above. -EAGAIN (nonblock, ring still has enough
            // queued) is expected and silently skipped — it just means this
            // block's mirror copy is dropped, not a real error. Any other
            // negative return is a genuine device-level problem (unplugged,
            // reset); recover once, and if the device is gone for good the
            // next block's -EAGAIN/error keeps getting silently absorbed here
            // rather than ever blocking or crashing the RT path.
            if (otgReady) {
                snd_pcm_sframes_t ow = snd_pcm_writei(otgPlay, otgBuf.data(), N);
                if (ow < 0 && ow != -EAGAIN) snd_pcm_recover(otgPlay, (int)ow, 1);
            }
        }
        if (otgPlay) snd_pcm_close(otgPlay);
        snd_pcm_close(cap); snd_pcm_close(play);
    }
#else
    // No ALSA in this build (review build). The RT thread + timing still run.
    while (g_running.load()) { /* block loop no-op */ }
#endif
    return nullptr;
}

bool AudioThread::start(const AudioConfig& cfg, ParamStore* params, LinkBridge* link) {
    cfg_ = cfg; g_cfg = cfg; g_params = params; g_link = link;
    g_running.store(true);
    // A real, reproducible SIGSEGV was WITNESSED live on a real Pi 4 here.
    // Root-caused via a debug build (-g -O0, unstripped) + gdb against a real
    // core dump: worker() declared `AloopLoopDsp faustHome;` as a stack-local
    // (sizeof(AloopLoopDsp) == 336,326,896 bytes, ~320 MiB — 20 loopers' worth
    // of 60s delay-line buffers) — no thread stack size, default or an
    // earlier-tried explicit 8 MiB, could ever hold that. Fixed by
    // heap-allocating it inside worker() instead (std::make_unique). The
    // default (nullptr) pthread attr here is correct as-is; do not add a large
    // explicit stack size in its place, since the fix is heap allocation, not
    // stack sizing.
    if (pthread_create(&g_worker, nullptr, worker, nullptr) != 0) {
        fprintf(stderr, "[audio] fatal: could not create audio thread\n");
        return false;
    }
    return true;
}

void AudioThread::stop() {
    g_running.store(false);
    pthread_join(g_worker, nullptr);
}

AudioThread::Telemetry AudioThread::snapshotTelemetry() const { return g_telem; }
Sampler* AudioThread::sampler() const { return g_sampler; }
bool AudioThread::setRealtime(int core, int prio) { return setRealtimeSelf(core, prio); }
void AudioThread::workerLoop() {}   // (the free function `worker` is the body)

} // namespace aloop
