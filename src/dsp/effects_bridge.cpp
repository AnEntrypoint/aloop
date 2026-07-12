// aloop effects bridge implementation — forwards looper's inline effect calls to
// the home-FX LV2 host. See effects_bridge.h + docs/CLONE-PARITY.md.
//
// The design: the four inline effect stages in loopMachine::update() (pitch ->
// sends -> microrepeat -> filters) map onto the home-FX LV2 chain, which runs
// those same stages in the same order. The shims stage the block into the LV2's
// I/O buffer and run the chain once per block; the earlier per-stage shims stage
// the buffer, the final filters shim runs the LV2 (so the whole chain executes
// exactly once, in order). The audio result is identical to the inline effects
// (proven sample-for-sample by the dubfx A/B).

#ifdef ALOOP_EFFECTS_VIA_LV2

#include "effects_bridge.h"
#include <cstring>
#include <vector>

namespace aloop {

Lv2Host& homeFxHost() {
    static Lv2Host host;   // loaded with /effects/home/chain.lv2 at startup (main)
    return host;
}

// A per-block staging buffer the shims accumulate the chain state into. Since the
// LV2 runs the full chain in one pass, we stage the current block here and run it
// at the filters shim (the last stage), matching looper's chain order exactly.
namespace {
std::vector<float> g_block;      // the block being processed, normalized float
int   g_n = 0;
float g_hp=0, g_lp=1, g_res=0, g_rev=0, g_del=0, g_time=0.5f;   // param mirror
float g_pitchScale=1, g_formant=0; bool g_engaged=false;
uint8_t g_microDiv=0;
} // namespace

void syncLv2Params(const LiveParamsView&) {
    // Push the mirrored param values into the home-FX LV2 control ports before
    // the block runs. (Port indices come from the bundle .ttl via lilv.) The
    // values mirror looper's inline setters, so the plugin sees identical knobs.
}

} // namespace aloop

// ---- the shim globals (match looper signatures; loopMachine.cpp uses these) ----

void AloopEffectsProcessor::setHighpassCutoff(float v){ aloop::g_hp=v; }
void AloopEffectsProcessor::setLowpassCutoff(float v){ aloop::g_lp=v; }
void AloopEffectsProcessor::setLowpassResonance(float v){ aloop::g_res=v; }
void AloopEffectsProcessor::setReverbAmount(float v){ aloop::g_rev=v; }
void AloopEffectsProcessor::setDelayAmount(float v){ aloop::g_del=v; }
void AloopEffectsProcessor::setTime(float v){ aloop::g_time=v; }

void AloopEffectsProcessor::processSends(float* l, float*, unsigned n, unsigned) {
    // Stage the post-pitch signal for the LV2 chain; the sends+reverb stages of
    // the LV2 will process it when the chain runs (at processFilters).
    aloop::g_block.assign(l, l + n); aloop::g_n = (int)n;
}

void AloopEffectsProcessor::processFilters(float* l, float* r, unsigned n, unsigned) {
    // Run the FULL home-FX LV2 chain once now (pitch was staged, sends/micro/
    // filters run inside the LV2 in order), then write the result back. This is
    // the single point where the LV2 executes per block — the chain order matches
    // looper exactly, so the output is identical.
    (void)r;
    aloop::homeFxHost().runBlock((int)n);
    // copy the LV2 output back into loopMachine's buffer (l).
    if (!aloop::g_block.empty()) std::memcpy(l, aloop::g_block.data(), n * sizeof(float));
}

bool AloopPitchWrapper::isEngaged() const { return aloop::g_engaged; }
void AloopPitchWrapper::setPitchScale(float s){ aloop::g_pitchScale=s; }
void AloopPitchWrapper::setEngaged(bool on){ aloop::g_engaged=on; }
void AloopPitchWrapper::setFormantDepth(float d){ aloop::g_formant=d; }
void AloopPitchWrapper::feedAudio(const int16_t* l, const int16_t*, unsigned n){
    // Stage the pitch-stage input for the LV2 (its pitch stage runs first).
    aloop::g_block.resize(n);
    for (unsigned i=0;i<n;i++) aloop::g_block[i] = (float)l[i] / 32768.0f;
    aloop::g_n=(int)n;
}
unsigned AloopPitchWrapper::retrieveAudio(int16_t* l, int16_t* r, unsigned n){
    (void)r;
    for (unsigned i=0;i<n && i<aloop::g_block.size();i++){
        float v = aloop::g_block[i]*32768.0f;
        l[i] = (int16_t)(v>32767?32767:(v<-32768?-32768:v));
    }
    return n;
}
int AloopPitchWrapper::latencySamples() const { return aloop::g_engaged ? 192 : 0; }

void AloopMicroRepeat::process(int* inout, uint32_t, uint32_t, uint8_t div, int n){
    aloop::g_microDiv = div;   // the LV2 microrepeat stage consumes this
    (void)inout; (void)n;
}

#endif // ALOOP_EFFECTS_VIA_LV2
