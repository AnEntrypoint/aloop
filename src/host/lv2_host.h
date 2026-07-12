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
    std::string uri;            // the plugin URI from the .ttl
    void*       soHandle = nullptr;   // dlopen handle to the .so
    void*       instance = nullptr;   // the LV2 instance (LV2_Handle)

    // Port wiring. LV2 audio ports are connected to our block buffers; control
    // ports are connected to values driven from the MIDI/param snapshot.
    std::vector<float*> audioIn;
    std::vector<float*> audioOut;
    std::vector<float*> control;      // pointers into the control-value store

    // Health: a plugin that faults or overruns is disabled by the watchdog and
    // skipped (bypassed) so the home chain + audio keep running (ADR-002).
    bool enabled = true;
    uint64_t faultCount = 0;

    int coreAffinity = -1;      // which core this plugin's run() executes on
};

// The host: owns the loaded plugins and drives them in-process, per block.
class Lv2Host {
public:
    // Discover + load all LV2 bundles under `dir` (e.g. /effects/user). Returns
    // the number loaded. Malformed/unloadable bundles are logged and skipped,
    // never fatal — a bad user plugin must not stop the device from booting.
    int loadDir(const std::string& dir);

    // Load a single bundle by path (used for the fixed home-FX bundle).
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

    bool readTtl(const std::string& bundlePath, Lv2Plugin& out);   // minimal .ttl parse / lilv
    bool dlopenPlugin(Lv2Plugin& p);
    void instantiate(Lv2Plugin& p, double sampleRate);
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
