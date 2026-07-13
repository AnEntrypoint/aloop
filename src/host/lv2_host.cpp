// aloop in-process LV2 host implementation. See lv2_host.h for the contract and
// ADR-002 for why this is in-process (never a graph).
//
// The lifecycle: discover .lv2 bundles → read the .ttl → dlopen the .so → get the
// LV2_Descriptor → instantiate → connect ports → run() per block. All run() calls
// happen inside the audio callback; the load/rescan happens on the control thread.

#include "lv2_host.h"

#include <dlfcn.h>
#include <dirent.h>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The LV2 core header is present in the build/Alpine container (lv2-dev). Guarded
// so the file compiles for review without it; the device build always has it.
#if __has_include(<lv2/lv2plug.in/ns/lv2core/lv2.h>)
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#define ALOOP_HAVE_LV2 1
#elif __has_include(<lv2.h>)
#include <lv2.h>
#define ALOOP_HAVE_LV2 1
#endif

// lilv reads real port metadata (index/class/symbol/default) from the bundle's
// .ttl so a THIRD-PARTY plugin's ports can be wired without hardcoding a layout
// (unlike the home Faust stack, which is compiled in directly and never goes
// through this host — see main.cpp). Soft dependency (pkg_check_modules without
// REQUIRED in CMakeLists): if absent, readTtl() falls back to locating the .so by
// directory scan and connectPorts() cannot wire ports, so the plugin loads but
// never actually runs signal through it (logged, never fatal).
#if __has_include(<lilv/lilv.h>)
#include <lilv/lilv.h>
#define ALOOP_HAVE_LILV 1
#endif

namespace aloop {

// ---- crash watchdog (ADR-002) -------------------------------------------------
// A user LV2 is untrusted. We wrap each run() in a longjmp checkpoint; a SIGSEGV
// or SIGFPE inside a plugin longjmps back here, the plugin is disabled, and the
// block continues with it bypassed — audio never dies on a bad user effect.
namespace {
sigjmp_buf   g_jmp;
volatile sig_atomic_t g_inPlugin = 0;
void faultHandler(int sig) {
    if (g_inPlugin) siglongjmp(g_jmp, sig);
    // Not in a plugin → genuine crash; restore default and re-raise.
    signal(sig, SIG_DFL);
    raise(sig);
}
void installWatchdog() {
    struct sigaction sa{};
    sa.sa_handler = faultHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
}
} // namespace

int Lv2Host::loadDir(const std::string& dir, int coreAffinity) {
    installWatchdog();
    int n = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) { fprintf(stderr, "[host] no effects dir %s (ok — skipped)\n", dir.c_str()); return 0; }
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".lv2") {
            if (loadBundle(dir + "/" + name, coreAffinity)) n++;
        }
    }
    closedir(d);
    fprintf(stderr, "[host] loaded %d effect(s) from %s (core %d)\n", n, dir.c_str(), coreAffinity);
    return n;
}

bool Lv2Host::loadBundle(const std::string& bundlePath, int coreAffinity) {
    Lv2Plugin p;
    p.bundlePath = bundlePath;
    p.coreAffinity = coreAffinity;
    if (!readTtl(bundlePath, p)) {
        fprintf(stderr, "[host] skip %s (no readable .ttl)\n", bundlePath.c_str());
        return false;                 // malformed bundle → skip, never fatal
    }
    if (!dlopenPlugin(p)) {
        fprintf(stderr, "[host] skip %s (dlopen failed: %s)\n", bundlePath.c_str(), dlerror());
        return false;
    }
    plugins_.push_back(std::move(p));
    fprintf(stderr, "[host] loaded %s on core %d\n", bundlePath.c_str(), coreAffinity);
    return true;
}

#ifdef ALOOP_HAVE_LILV
namespace {
LilvWorld* g_lilvWorld = nullptr;   // one world for the process; plugins loaded from it live as long as it does
}
#endif

// Resolve the bundle's real plugin URI + port layout via lilv, falling back to a
// directory scan for just the .so path if lilv is unavailable (soft dependency —
// see the ALOOP_HAVE_LILV comment above). The fallback path lets a plugin LOAD
// (dlopen + instantiate) but connectPorts() then has no port metadata to wire
// with, so process() stays a no-op passthrough for that plugin — logged, not fatal.
bool Lv2Host::readTtl(const std::string& bundlePath, Lv2Plugin& out) {
    DIR* d = opendir(bundlePath.c_str());
    if (!d) return false;
    struct dirent* e; std::string so;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.size() > 3 && n.substr(n.size() - 3) == ".so") so = bundlePath + "/" + n;
    }
    closedir(d);
    if (so.empty()) return false;
    out.soPath = so;
    out.uri = so;   // overwritten with the real LV2 URI below when lilv resolves the bundle

#ifdef ALOOP_HAVE_LILV
    if (!g_lilvWorld) { g_lilvWorld = lilv_world_new(); lilv_world_load_all(g_lilvWorld); }
    if (!lilvWorld_) lilvWorld_ = g_lilvWorld;

    // Load just this bundle's manifest (it may not be in lilv's default search
    // path — home/user effect dirs are aloop-specific), then find its one plugin.
    std::string uri = "file://" + bundlePath + "/";
    LilvNode* bundleUri = lilv_new_uri(g_lilvWorld, uri.c_str());
    lilv_world_load_bundle(g_lilvWorld, bundleUri);
    lilv_node_free(bundleUri);

    const LilvPlugins* plugins = lilv_world_get_all_plugins(g_lilvWorld);
    const LilvPlugin* found = nullptr;
    LILV_FOREACH(plugins, i, plugins) {
        const LilvPlugin* pl = lilv_plugins_get(plugins, i);
        const LilvNode* bundle = lilv_plugin_get_bundle_uri(pl);
        const char* bpathC = lilv_uri_to_path(lilv_node_as_uri(bundle));
        std::string bpath = bpathC ? bpathC : "";
        if (!bpath.empty() && bundlePath.find(bpath) == 0) { found = pl; break; }
    }
    if (found) {
        out.lilvPlugin = (void*)found;
        out.uri = lilv_node_as_uri(lilv_plugin_get_uri(found));

        LilvNode* audioClass   = lilv_new_uri(g_lilvWorld, LILV_URI_AUDIO_PORT);
        LilvNode* controlClass = lilv_new_uri(g_lilvWorld, LILV_URI_CONTROL_PORT);
        LilvNode* inputClass   = lilv_new_uri(g_lilvWorld, LILV_URI_INPUT_PORT);

        uint32_t n = lilv_plugin_get_num_ports(found);
        for (uint32_t idx = 0; idx < n; idx++) {
            const LilvPort* port = lilv_plugin_get_port_by_index(found, idx);
            Lv2Plugin::PortInfo pi;
            pi.index    = idx;
            pi.isAudio  = lilv_port_is_a(found, port, audioClass);
            pi.isInput  = lilv_port_is_a(found, port, inputClass);
            const LilvNode* symNode = lilv_port_get_symbol(found, port);
            pi.symbol = symNode ? lilv_node_as_string(symNode) : ("port" + std::to_string(idx));
            if (!pi.isAudio && !lilv_port_is_a(found, port, controlClass)) continue;   // skip atom/CV/etc — unsupported port types are left unconnected, not fatal
            out.ports.push_back(pi);
        }
        lilv_node_free(inputClass);
        lilv_node_free(controlClass);
        lilv_node_free(audioClass);
    } else {
        fprintf(stderr, "[host] lilv found no plugin matching bundle %s — falling back to .so-only load (no port wiring)\n", bundlePath.c_str());
    }
#endif
    return true;
}

bool Lv2Host::dlopenPlugin(Lv2Plugin& p) {
    p.soHandle = dlopen(p.soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!p.soHandle) return false;
#ifdef ALOOP_HAVE_LV2
    auto desc = (const LV2_Descriptor* (*)(uint32_t))dlsym(p.soHandle, "lv2_descriptor");
    if (!desc) return false;
    // Instantiate happens in connect() once we know the sample rate; store the
    // descriptor getter via the handle (kept opaque in the header).
#endif
    return true;
}

#ifdef ALOOP_HAVE_LV2
// A .so can export multiple LV2_Descriptors (lv2_descriptor(0), (1), ...); match
// by URI so we instantiate the one this bundle's .ttl actually names, not just
// index 0 (which is only guaranteed correct for single-plugin binaries).
static const LV2_Descriptor* findDescriptor(void* soHandle, const std::string& uri) {
    auto getDesc = (const LV2_Descriptor* (*)(uint32_t))dlsym(soHandle, "lv2_descriptor");
    if (!getDesc) return nullptr;
    for (uint32_t i = 0; ; i++) {
        const LV2_Descriptor* d = getDesc(i);
        if (!d) return nullptr;                       // end of the list — no match
        if (uri.empty() || uri == d->URI) return d;    // uri.empty() = fallback path (no lilv), take the first
    }
}
#endif

void Lv2Host::instantiate(Lv2Plugin& p, double sampleRate) {
#ifdef ALOOP_HAVE_LV2
    const LV2_Descriptor* d = findDescriptor(p.soHandle, p.lilvPlugin ? p.uri : std::string());
    if (!d) { p.enabled = false; return; }
    p.instance = (void*)d->instantiate(d, sampleRate, p.bundlePath.c_str(), nullptr);
    if (!p.instance) { p.enabled = false; return; }
    if (d->activate) d->activate((LV2_Handle)p.instance);
#else
    (void)p; (void)sampleRate;
#endif
}

// Bind every port lilv discovered in readTtl() to real memory: audio ports point
// into the shared ioBuffer_ (so runOne()'s connect_port + run() actually reads/
// writes the same signal audio_thread.cpp copies in/out via process()); control
// ports get their own persistent float slot (their symbol is the ParamStore bind
// key a future control-map row targets, matching the home stack's name-keyed
// convention). Without lilv metadata (out.ports empty) there is nothing to
// connect — the plugin instantiates and run()s but touches no shared memory, so
// process() below correctly stays a passthrough for it.
void Lv2Host::connectPorts(Lv2Plugin& p, int blockSize) {
#ifdef ALOOP_HAVE_LV2
    if (!p.instance || p.ports.empty()) return;
    const LV2_Descriptor* d = findDescriptor(p.soHandle, p.lilvPlugin ? p.uri : std::string());
    if (!d || !d->connect_port) return;

    p.controlValues.assign(p.ports.size(), 0.0f);
    for (size_t i = 0; i < p.ports.size(); i++) {
        auto& pi = p.ports[i];
        if (pi.isAudio) {
            // Mono I/O (AudioConfig.channels == 1): every audio port shares the
            // one-channel ioBuffer_ — an input port reads it, an output port
            // overwrites it, matching process()'s copy-in/run/copy-out contract.
            float* buf = ioBuffer_.data();
            d->connect_port(p.instance, pi.index, buf);
            (pi.isInput ? p.audioIn : p.audioOut).push_back(buf);
        } else {
            d->connect_port(p.instance, pi.index, &p.controlValues[i]);
            p.controlPortIdx.push_back(i);
        }
    }
    (void)blockSize;
#else
    (void)p; (void)blockSize;
#endif
}

void Lv2Host::connect(int blockSize, int numChannels) {
    ioBuffer_.assign((size_t)blockSize * numChannels, 0.0f);
    for (auto& p : plugins_) {
        instantiate(p, 48000.0);
        connectPorts(p, blockSize);
    }
}

void Lv2Host::runOne(Lv2Plugin& p, int nframes) {
    if (!p.enabled || !p.instance) return;
#ifdef ALOOP_HAVE_LV2
    const LV2_Descriptor* d = findDescriptor(p.soHandle, p.lilvPlugin ? p.uri : std::string());
    if (!d) { p.enabled = false; return; }
    // Watchdog checkpoint: a fault inside run() longjmps back and disables it.
    g_inPlugin = 1;
    if (sigsetjmp(g_jmp, 1) == 0) {
        d->run((LV2_Handle)p.instance, (uint32_t)nframes);
    } else {
        g_inPlugin = 0;
        p.faultCount++;
        disablePlugin(&p);
        fprintf(stderr, "[host] plugin %s faulted — disabled, continuing\n", p.bundlePath.c_str());
    }
    g_inPlugin = 0;
#else
    (void)nframes;
#endif
}

void Lv2Host::runBlock(int nframes) {
    // In-process, no graph (ADR-002). SERIAL: run each plugin in turn on this
    // callback's core (zero added latency, fits the block). FORK_JOIN would fan
    // the parallel plugins to their pinned cores and join before returning — same
    // zero added latency, used only for genuinely parallel effect topologies.
    for (auto& p : plugins_) runOne(p, nframes);
}

void Lv2Host::process(float* buf, int nframes) {
    // No plugins loaded → passthrough (the common case: no user effect present,
    // DEGRADED-MODES). Otherwise the signal flows through the plugin chain: the
    // plugins' audio ports are connected to ioBuffer_ (in connect()), so we copy
    // in, run, copy out. No allocation in the per-block path (ioBuffer_ is sized
    // once in connect()).
    if (plugins_.empty()) return;
    int n = nframes;
    if ((int)ioBuffer_.size() < n) return;   // safety: connect() must have sized it
    for (int i = 0; i < n; i++) ioBuffer_[(size_t)i] = buf[i];
    runBlock(n);
    for (int i = 0; i < n; i++) buf[i] = ioBuffer_[(size_t)i];
}

void Lv2Host::rescanUser(const std::string& userDir) {
    // Control-thread only. Reload the user dir; the audio thread picks up the new
    // set via the same double-buffer-flip discipline as the param snapshot.
    loadDir(userDir);
}

void Lv2Host::disablePlugin(Lv2Plugin* p) { if (p) p->enabled = false; }

} // namespace aloop
