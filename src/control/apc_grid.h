#ifndef ALOOP_APC_GRID_H
#define ALOOP_APC_GRID_H

#include <cstdint>
#include "midi.h"

namespace aloop {

class Sampler;
class AudioThread;
class Lv2Host;

constexpr int kApcRows = 5;
constexpr int kApcCols = 8;
constexpr int kLooperCount = 20;
constexpr int kPresetCount = 10;
constexpr int kTransposeVoices = 6;
constexpr int kResonodeVoices = 4;
constexpr unsigned kHoldEraseMs = 1000;
constexpr int kSampleRate = 48000;
constexpr int kBlockSize  = 64;
constexpr int kMaxLoopSamples = 48000 * 60;
constexpr int kApcBtnShift = 0x62;

enum class FxBank : uint8_t { Dub = 0, Guitar = 1, LofiFx = 2 };
constexpr int kFxBankCount = 3;
constexpr int kApcBtnDubFx    = 67;
constexpr int kApcBtnGuitarFx = 68;
constexpr int kApcBtnLofiFx   = 69;
constexpr int kFxKnobCount = 7;

enum class FxKnobKind : uint8_t { FaustZone, Lv2Control, SamplerAttackMs, SamplerReleaseMs,
                                   SamplerGranPatchWeight };
struct FxKnobTarget {
    FxKnobKind kind;
    const char* name;
};

inline int gridLooperIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 2 || col > 5) return -1;
    int idx = row * 4 + (col - 2);
    return (idx >= 0 && idx < kLooperCount) ? idx : -1;
}
inline int gridPresetIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 0 || col > 1) return -1;
    int idx = row * 2 + col;
    return (idx >= 0 && idx < kPresetCount) ? idx : -1;
}

class ApcGrid {
public:
    static void bindAll(ParamStore& ps);

    void onPadPress(int note, unsigned now_ms, ParamStore& ps, class LinkBridge* link = nullptr, class AudioThread* audio = nullptr);
    void onPadRelease(int note, unsigned now_ms, ParamStore& ps, class LinkBridge* link = nullptr, class AudioThread* audio = nullptr);
    void pollHolds(unsigned now_ms, ParamStore& ps, class LinkBridge* link = nullptr, class AudioThread* audio = nullptr);

    void onModWheel(uint8_t data2, ParamStore& ps);
    void onAbsolutePitch(uint8_t data2, ParamStore& ps);

    void onLiveEngageToggle(ParamStore& ps);
    bool liveEngaged() const { return m_liveEngaged; }

    void onKeybedNoteOn(int note, int vel, ParamStore& ps, Sampler* sampler);
    void onKeybedNoteOff(int note, ParamStore& ps, Sampler* sampler);

    void onSamplerBtn65Press(Sampler* sampler);
    void onSamplerBtn65Release(Sampler* sampler);
    void onSamplerBtn66Press();
    void onSamplerBtn66Release(Sampler* sampler);
    bool drumRecordMode() const { return m_drumRecordMode; }

    void onStopImmediate(ParamStore& ps, class LinkBridge* link = nullptr);

    void onClearAll(bool held, ParamStore& ps, class LinkBridge* link = nullptr);

    void onMicrorepeatOn(int note, ParamStore& ps);
    void onMicrorepeatOff(int note, ParamStore& ps);

    void onShiftPress(ParamStore& ps);
    void onShiftRelease(ParamStore& ps);
    bool shiftHeld() const { return m_shift; }

    void onFormantCC(uint8_t data2, ParamStore& ps);

    void onDubFxPress(unsigned now_ms, ParamStore& ps);
    void onLofiFxPress(unsigned now_ms, ParamStore& ps, Sampler* sampler, class AudioThread* audio = nullptr);
    void onLofiFxRelease(unsigned now_ms, ParamStore& ps, Sampler* sampler);
    bool granulatorLatched() const { return m_granulatorLatched; }
    bool resonodeEngaged() const { return m_resonodeEngaged; }
    bool resonodeLatched() const { return m_resonodeLatched; }
    void onGuitarFxPress(unsigned now_ms, ParamStore& ps);
    void onGuitarFxRelease(ParamStore& ps);
    void onFxKnobCC(int ccNumber, uint8_t data2, ParamStore& ps, Sampler* sampler, Lv2Host* homeFx);
    FxBank activeBank() const { return m_activeBank; }
    bool bankFlashActive() const { return m_bankFlashReleaseAt != 0; }
    FxBank bankFlashWhich() const { return m_bankFlashWhich; }

    void onSidechainLooperToggle(int looper, ParamStore& ps);
    bool looperIsSidechainSource(int looper) const { return m_looperIsSidechainSource[looper]; }
    bool guitarFxHeldConsumedByLooper() const { return m_guitarFxConsumedByLooperPress; }

    bool looperHasContent(int looper) const { return m_looperHasContent[looper]; }
    bool looperPlaying(int looper) const { return m_looperPlaying[looper]; }
    bool looperRecording(int looper) const { return m_looperRecording[looper]; }
    bool presetUsed(int preset) const { return m_presetUsed[preset]; }
    uint8_t microrepeatDiv() const { return m_microRepeatDiv; }

private:
    bool m_looperHeld[kLooperCount] = {};
    unsigned m_looperHoldStart[kLooperCount] = {};
    bool m_looperErased[kLooperCount] = {};
    unsigned m_looperEraseReleaseAt[kLooperCount] = {};
    unsigned m_looperFinishReqReleaseAt[kLooperCount] = {};
    bool m_looperArmedOnPress[kLooperCount] = {};
    bool m_looperPlaying[kLooperCount] = {};
    bool m_looperHasContent[kLooperCount] = {};
    bool m_looperRecording[kLooperCount] = {};
    float m_looperFinishTargetPending[kLooperCount] = {};
    unsigned m_recordStartMs[kLooperCount] = {};
    bool m_looperShiftHeldDuringTake[kLooperCount] = {};
    bool m_lastPublishedPlaying = false;
    bool    m_lastSeenRemotePlaying = false;
    bool    m_remoteStartPending    = false;
    int64_t m_lastRemotePhaseMicroBeats = 0;

    bool m_presetHeld[kPresetCount] = {};
    unsigned m_presetHoldStart[kPresetCount] = {};
    bool m_presetCaptured[kPresetCount] = {};
    bool m_presetUsed[kPresetCount] = {};
    uint32_t m_presetMask[kPresetCount] = {};

    uint8_t m_microRepeatDiv = 0;
    bool m_shift = false;
    bool m_liveEngaged = false;
    bool m_drumRecordMode = false;

    int m_transposeVoiceNote[kTransposeVoices] = {-1, -1, -1, -1, -1, -1};
    uint32_t m_transposeVoiceOrder[kTransposeVoices] = {};
    uint32_t m_transposeVoiceCounter = 0;
    int allocateTransposeVoice(int note);
    void releaseTransposeVoice(int note, ParamStore& ps);
    long m_masterLenSamples = 0;

    float m_fxBankValues[kFxBankCount][kFxKnobCount] = {
        {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    };
    FxBank m_activeBank = FxBank::Dub;
    unsigned m_bankFlashReleaseAt = 0;
    FxBank m_bankFlashWhich = FxBank::Dub;
    static constexpr unsigned kBankFlashMs = 150;

    bool m_granulatorHeld = false;
    bool m_granulatorLatched = false;
    FxBank m_bankBeforeGranulatorHold = FxBank::Dub;
    void applyGranulatorMorph(Sampler* sampler);

    bool m_resonodeEngaged = false;
    bool m_resonodeLatched = false;
    int m_resonodeVoiceNote[kResonodeVoices] = {-1, -1, -1, -1};
    uint32_t m_resonodeVoiceOrder[kResonodeVoices] = {};
    uint32_t m_resonodeVoiceCounter = 0;
    int allocateResonodeVoice(int note);
    void releaseResonodeVoice(int note, ParamStore& ps);
    void releaseAllResonodeVoices(ParamStore& ps);
    void applyResonodePatchMorph(ParamStore& ps);
    void toggleResonodeEngage(ParamStore& ps, AudioThread* audio);

    bool m_guitarFxHeld = false;
    bool m_guitarFxConsumedByLooperPress = false;
    bool m_looperIsSidechainSource[kLooperCount] = {};

    void applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps, class LinkBridge* link, class AudioThread* audio = nullptr);
    void capturePreset(int p, ParamStore& ps);
    void applyPreset(int p, ParamStore& ps);
    void forgetLooperFromPresets(int looper);
    void publishTransport(class LinkBridge* link);
    void applyRemoteTransport(ParamStore& ps, class LinkBridge* link);
};

}
#endif
