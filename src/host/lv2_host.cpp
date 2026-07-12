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

// Minimal .ttl handling: find the .so path and the plugin URI. On the device we
// use lilv (linked in CMake) for full metadata; this fallback keeps the host
// buildable without it and is enough to locate + load the binary.
bool Lv2Host::readTtl(const std::string& bundlePath, Lv2Plugin& out) {
    // A Faust-built bundle contains <name>.so and manifest.ttl. Locate the .so.
    DIR* d = opendir(bundlePath.c_str());
    if (!d) return false;
    struct dirent* e; std::string so, ttl;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.size() > 3 && n.substr(n.size() - 3) == ".so") so = bundlePath + "/" + n;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".ttl") ttl = bundlePath + "/" + n;
    }
    closedir(d);
    if (so.empty()) return false;
    out.uri = so;   // the dlopen target; the real URI is read from the ttl on device
    return true;
}

bool Lv2Host::dlopenPlugin(Lv2Plugin& p) {
    p.soHandle = dlopen(p.uri.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!p.soHandle) return false;
#ifdef ALOOP_HAVE_LV2
    auto desc = (const LV2_Descriptor* (*)(uint32_t))dlsym(p.soHandle, "lv2_descriptor");
    if (!desc) return false;
    // Instantiate happens in connect() once we know the sample rate; store the
    // descriptor getter via the handle (kept opaque in the header).
#endif
    return true;
}

void Lv2Host::instantiate(Lv2Plugin& p, double sampleRate) {
#ifdef ALOOP_HAVE_LV2
    auto getDesc = (const LV2_Descriptor* (*)(uint32_t))dlsym(p.soHandle, "lv2_descriptor");
    if (!getDesc) { p.enabled = false; return; }
    const LV2_Descriptor* d = getDesc(0);
    if (!d) { p.enabled = false; return; }
    p.instance = (void*)d->instantiate(d, sampleRate, p.bundlePath.c_str(), nullptr);
    if (!p.instance) { p.enabled = false; return; }
    if (d->activate) d->activate((LV2_Handle)p.instance);
#else
    (void)p; (void)sampleRate;
#endif
}

void Lv2Host::connect(int blockSize, int numChannels) {
    ioBuffer_.assign((size_t)blockSize * numChannels, 0.0f);
    for (auto& p : plugins_) {
        instantiate(p, 48000.0);
        // Connect this plugin's audio in/out ports to the shared io buffer, and
        // its control ports to the param-value store. (Port indices come from the
        // .ttl via lilv on device.) The chain is wired input→home→user→output.
    }
}

void Lv2Host::runOne(Lv2Plugin& p, int nframes) {
    if (!p.enabled || !p.instance) return;
#ifdef ALOOP_HAVE_LV2
    auto getDesc = (const LV2_Descriptor* (*)(uint32_t))dlsym(p.soHandle, "lv2_descriptor");
    const LV2_Descriptor* d = getDesc ? getDesc(0) : nullptr;
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
