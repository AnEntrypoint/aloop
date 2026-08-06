const kFinCount = 32;
const kInIndex = 0;
const kPrevFiltInIndex = 1;
const kClearAllIndex = 2;
const kEffSpeedIndex = 3;
const kMasterPhaseIndex = 4;
const kMasterLenIndex = 5;
const kSidechainEnvIndex = 6;
const kFreeXposeIndex = 7;
const kXposeVoiceCount = 6;
const kXposeBaseIndex = 8;
const kResonodeVoiceCount = 4;
const kResonodeBaseIndex = 20;
const kFoldStepPerSample = 1.0 / 1024.0;

class AloopControlProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.clearAll = 0.0;
    this.effSpeed = 1.0;
    this.masterLenSamples = 0.0;
    this.sidechainEnv = 0.0;
    this.freeXpose = 0.0;
    this.foldGain = 0.0;
    this.glitchFoldGain = 0.0;
    this.foldTarget = 0.0;
    this.glitchFoldTarget = 0.0;
    this.masterPhaseSamples = 0.0;
    this.xposeNote = new Float32Array(kXposeVoiceCount);
    this.xposeGate = new Float32Array(kXposeVoiceCount);
    this.resonodeNote = new Float32Array(kResonodeVoiceCount);
    this.resonodeGate = new Float32Array(kResonodeVoiceCount);
    this.resonodeVel = new Float32Array(kResonodeVoiceCount).fill(1.0);
    this.port.onmessage = (event) => this.applyControlMessage(event.data);
  }

  applyControlMessage(msg) {
    switch (msg.target) {
      case "clearAll": this.clearAll = msg.value; return;
      case "effSpeed": this.effSpeed = msg.value; return;
      case "masterLenSamples": this.masterLenSamples = msg.value; return;
      case "masterPhaseSamples": this.masterPhaseSamples = msg.value; return;
      case "sidechainEnv": this.sidechainEnv = msg.value; return;
      case "foldTarget": this.foldTarget = msg.value; return;
      case "glitchFoldTarget": this.glitchFoldTarget = msg.value; return;
      case "freeXpose": this.freeXpose = msg.value; return;
      case "xposeVoice": this.xposeNote[msg.voice] = msg.note; this.xposeGate[msg.voice] = msg.gate; return;
      case "resonodeVoice": this.resonodeNote[msg.voice] = msg.note; this.resonodeGate[msg.voice] = msg.gate; this.resonodeVel[msg.voice] = msg.vel; return;
      default: return;
    }
  }

  rampFold(target, current) {
    if (current < target) {
      current += kFoldStepPerSample;
      if (current > target) current = target;
    } else if (current > target) {
      current -= kFoldStepPerSample;
      if (current < target) current = target;
    }
    return current;
  }

  process(inputs, outputs) {
    const mic = inputs[0][0] || new Float32Array(128);
    const delayedLoopSum = inputs[1][0] || new Float32Array(mic.length);
    const delayedRecordTap = inputs[2][0] || new Float32Array(mic.length);
    const out = outputs[0];
    const n = out[kInIndex].length;

    const masterLen = this.masterLenSamples;
    const masterPhaseOut = out[kMasterPhaseIndex];
    if (masterLen > 0.0) {
      if (masterLen >= n) {
        let p = this.masterPhaseSamples;
        for (let i = 0; i < n; i++) {
          masterPhaseOut[i] = p;
          p += 1.0;
          if (p >= masterLen) p -= masterLen;
        }
        this.masterPhaseSamples = p;
      } else {
        for (let i = 0; i < n; i++) {
          let p = (this.masterPhaseSamples + i) % masterLen;
          if (p < 0.0) p += masterLen;
          masterPhaseOut[i] = p;
        }
        this.masterPhaseSamples = (this.masterPhaseSamples + n) % masterLen;
      }
    } else {
      masterPhaseOut.fill(0.0);
      this.masterPhaseSamples = 0.0;
    }

    const inOut = out[kInIndex];
    const prevFiltInOut = out[kPrevFiltInIndex];
    for (let i = 0; i < n; i++) {
      this.foldGain = this.rampFold(this.foldTarget, this.foldGain);
      this.glitchFoldGain = this.rampFold(this.glitchFoldTarget, this.glitchFoldGain);
      let combinedFold = this.foldGain + this.glitchFoldGain;
      if (combinedFold > 1.0) combinedFold = 1.0;
      inOut[i] = mic[i] + delayedLoopSum[i] * combinedFold;
      prevFiltInOut[i] = delayedRecordTap[i];
    }

    out[kClearAllIndex].fill(this.clearAll);
    out[kEffSpeedIndex].fill(this.effSpeed);
    out[kMasterLenIndex].fill(this.masterLenSamples);
    out[kSidechainEnvIndex].fill(this.sidechainEnv);
    out[kFreeXposeIndex].fill(this.freeXpose);
    for (let v = 0; v < kXposeVoiceCount; v++) {
      out[kXposeBaseIndex + v * 2].fill(this.xposeNote[v]);
      out[kXposeBaseIndex + v * 2 + 1].fill(this.xposeGate[v]);
    }
    for (let v = 0; v < kResonodeVoiceCount; v++) {
      out[kResonodeBaseIndex + v * 3].fill(this.resonodeNote[v]);
      out[kResonodeBaseIndex + v * 3 + 1].fill(this.resonodeGate[v]);
      out[kResonodeBaseIndex + v * 3 + 2].fill(this.resonodeVel[v]);
    }
    return true;
  }
}

AloopControlProcessor.kFinCount = kFinCount;
registerProcessor("aloop-control-processor", AloopControlProcessor);
