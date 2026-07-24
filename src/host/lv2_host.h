// aloop in-process LV2 host — the moddability + zero-latency engine.
//
// WHY this exists and why it looks like this (see docs/ARCHITECTURE.md, ADR-002):
//   - Effects are LV2 plugins so they can be swapped as files on flash.
//   - We host them IN-PROCESS (dlopen + call run() inside our own audio
//     callback), NEVER through a JACK/PipeWire graph, because a graph adds one
//     full audio period (~1.333 ms) of latency per hop and the whole point is
//     zero added latency.
//   - Two plugins can run on two cores: a serial chain fits one core inside the
//     block budget (default, zero latency), or genuinely-parallel effects can
//     fork-join across Core 1 and Core 3 within the same block (also zero added
//     latency). This header is the plumbing for that.
//
// This is a design-complete skeleton: the load/instantiate/run lifecycle and the
// crash-isolation contract are real; the LV2 symbol details are filled against
// <lv2/lv2plug.in/ns/lv2core/lv2.h> at build time (present in the CI/Alpine
// container, see .github/workflows/).

#ifndef ALOOP_LV2_HOST_H
#define ALOOP_LV2_HOST_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace aloop {

// One loaded LV2 effect: the dlopen handle, the instance, its port buffers, and
// the health state used by the crash watchdog.
struct Lv2Plugin {
    std::string bundlePath;     // path to the .lv2 directory on flash
    std::string uri;            // the plugin URI (its LV2 URI, read via lilv; the
                                 // dlopen path when lilv is unavailable)
    std::string soPath;         // the dlopen target (the bundle's .so)
    void*       soHandle = nullptr;   // dlopen handle to the .so
    void*       instance = nullptr;   // the LV2 instance (LV2_Handle)
    void*       lilvPlugin = nullptr; // const LilvPlugin* (opaque here; lilv.h only in the .cpp)
    // Cached LV2_Descriptor* (resolved once in dlopenPlugin/instantiate via
    // dlsym+URI-matching scan) so the audio-rate hot path (runOne(), called
    // every single block on Core 1/Core 3) never re-resolves it — a dlsym
    // lookup + linear descriptor-list scan on every block is pure per-block
    // overhead for a value that can never change after load. Opaque (const
    // LV2_Descriptor*, stored as void*) so this header stays includable
    // without <lv2.h> for the review build, matching lilvPlugin's own pattern.
    const void* descriptor = nullptr;

    // Port wiring, resolved from real port metadata (lilv on the device build):
    // each port's LV2 index + class (audio/control, in/out). Audio ports are
    // connected to our shared block buffer; control ports get their own
    // persistent float storage seeded from the port's declared default.
    struct PortInfo {
        uint32_t index = 0;
        std::string symbol;     // port symbol, e.g. "gain" — the ParamStore bind key
        bool isAudio = false;   // false = control port
        bool isInput = false;
    };
    std::vector<PortInfo> ports;
    std::vector<float*>   audioIn;     // pointers into the shared ioBuffer_
    std::vector<float*>   audioOut;
    std::vector<float>    controlValues;   // one slot per control port, index-aligned to `ports`
    std::vector<size_t>   controlPortIdx;  // controlValues[i] belongs to ports[controlPortIdx[i]]

    // Health: a plugin that faults or overruns is disabled by the watchdog and
    // skipped (bypassed) so the home chain + audio keep running (ADR-002).
    bool enabled = true;
    uint64_t faultCount = 0;

    int coreAffinity = -1;      // which core this plugin's run() executes on
};

// The host: owns the loaded plugins and drives them in-process, per block.
class Lv2Host {
public:
    // Discover + load all LV2 bundles under `dir` (e.g. /effects/home,
    // /effects/user), each pinned to `coreAffinity`. Returns the number loaded.
    // Loading BY DIRECTORY (any *.lv2 present) is deliberate: the faust2lv2 home
    // bundle is named after its .dsp (aloop.lv2), not a fixed "chain.lv2", so a
    // hardcoded filename would silently load nothing. Malformed/unloadable bundles
    // are logged and skipped, never fatal — a bad plugin must not stop the boot.
    int loadDir(const std::string& dir, int coreAffinity = 3);

    // Load a single bundle by explicit path (rarely needed; loadDir is preferred).
    bool loadBundle(const std::string& bundlePath, int coreAffinity);

    // Wire every plugin's ports to the shared block buffers + control store.
    // Called once after loading, before the audio thread starts.
    void connect(int blockSize, int numChannels);

    // Run all enabled plugins for one block, in the configured topology:
    //   - SERIAL: plugin[0].run(); plugin[1].run(); ...  (one core, zero latency)
    //   - FORK_JOIN: parallel plugins run on their pinned cores, joined this block
    // The audio callback calls exactly this, inside the block budget. It does NO
    // allocation, locking, or syscalls — RT-safe by contract.
    void runBlock(int nframes);

    // Process an EXTERNAL mono buffer in place: copy it into the host's I/O
    // buffer, run the plugin chain, copy the result back. This is what the audio
    // thread calls to run the user effect on the home-stack output — the plugin
    // audio ports are connected to the host's ioBuffer_, so the signal actually
    // flows through the user plugin. RT-safe (no alloc in the per-block path).
    void process(float* buf, int nframes);

    // Push a live control value into every loaded plugin's port matching `symbol`
    // (the LV2 port symbol == the ParamStore bind key, per connectPorts()'s own
    // documented convention). RT-safe: a flat linear scan over already-resolved
    // controlPortIdx entries, no allocation, no lookup structure — the same class
    // of cost the sidechain lookup fix elsewhere in this codebase established as
    // acceptable for small, fixed per-block call counts (here: 8 guitar/lofi-fx
    // knobs, called once per control-map update, not per audio block).
    void setControl(const std::string& symbol, float value);

    // Rescan the user dir for added/removed bundles (hot-swap). Called from the
    // CONTROL thread, never the audio thread; the audio thread sees the new set
    // via the same double-buffer-flip discipline as the param snapshot.
    void rescanUser(const std::string& userDir);

    enum class Topology { SERIAL, FORK_JOIN };
    void setTopology(Topology t) { topology_ = t; }

    // Crash isolation: the SIGSEGV handler calls this with the faulting plugin
    // (identified by the run() it was in) to disable it and continue degraded.
    void disablePlugin(Lv2Plugin* p);

private:
    std::vector<Lv2Plugin> plugins_;   // [0] = home-FX (Core 1), [1..] = user (Core 3)
    Topology topology_ = Topology::SERIAL;

    // The shared per-block audio buffers the plugin ports connect to.
    std::vector<float> ioBuffer_;
    void* lilvWorld_ = nullptr;   // LilvWorld*, opaque here (lilv.h only in the .cpp); owns lilvPlugin lifetimes

    bool readTtl(const std::string& bundlePath, Lv2Plugin& out);   // resolves soPath + uri + ports via lilv
    bool dlopenPlugin(Lv2Plugin& p);
    void instantiate(Lv2Plugin& p, double sampleRate);
    void connectPorts(Lv2Plugin& p, int blockSize);   // binds each port by real index/class (lilv-derived)
    void runOne(Lv2Plugin& p, int nframes);   // guarded run() (watchdog-wrapped)
};

// The crash watchdog contract (implemented in lv2_host.cpp):
//   - install a SIGSEGV/SIGFPE handler once at startup;
//   - a longjmp checkpoint is set around each plugin's run();
//   - on fault, the handler longjmps back, disablePlugin() is called, and the
//     block continues with that plugin bypassed. The device never crashes on a
//     bad user effect — it degrades to the home chain (ADR-002).

} // namespace aloop

#endif // ALOOP_LV2_HOST_H
