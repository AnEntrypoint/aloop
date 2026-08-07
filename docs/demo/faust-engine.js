const FAUSTWASM_INDEX_URL = "https://unpkg.com/@grame/faustwasm@latest/dist/esm/index.js";
const LIBFAUST_MODULE_URL = "https://unpkg.com/@grame/faustwasm@latest/libfaust-wasm/libfaust-wasm.js";

const LOOPER_COUNT = 20;
const TRANSPOSE_VOICE_COUNT = 6;
const RESONODE_VOICE_COUNT = 4;
const MAX_LOOP_SAMPLES = 48000 * 60;
const RENDER_QUANTUM_DELAY_SECONDS = 128 / 48000;
const HOLD_ERASE_MS = 1000;
const GRANULATOR_TAP_MS = 1000;

const DUB_TARGETS = ["fx/reverb", "fx/delay", "fx/time", "fx/hp", "fx/lpres", "fx/lp", "fx/pitch"];
const GUITAR_TARGETS = ["fx2/FLANGEAMT", "fx2/TREMOLOAMT", "fx2/BANKSPEED", "fx2/PHASERAMT", null, null, "fx2/COMPRESSAMT"];
const RESONODE_PATCHES = [
  { position: 0.08, decay: 0.15, damping: 0.8, stretch: -0.1, collision: 0.55 },
  { position: 0.08, decay: 7.0, damping: 0.97, stretch: 1.2, collision: 0.15 },
  { position: 0.08, decay: 7.0, damping: 0.97, stretch: -0.1, collision: 0.0 },
  { position: 0.6, decay: 1.2, damping: 0.35, stretch: 0.05, collision: 0.25 },
];
const RESONODE_DIRECT_RANGES = [
  { zone: "fx/resonode/tone", lo: 200.0, hi: 18000.0, logTaper: true },
  { zone: "fx/resonode/level", lo: 0.0, hi: 1.5, logTaper: false },
];

function looperGroupSegment(index) {
  return "looper" + String(index).padStart(2, " ");
}

function deriveTempoQuant(seconds) {
  if (seconds <= 0) return { bpm: 120.0, beats: 16.0 };
  const candidates = [1, 2, 4, 8, 16, 32, 64, 128];
  let best = { bpm: 120.0, beats: 16.0 };
  let bestDist = Infinity;
  let bestInWindow = false;
  for (const beats of candidates) {
    const bpm = (60.0 * beats) / seconds;
    const inWindow = bpm >= 80.0 && bpm <= 160.0;
    const dist = Math.abs(bpm - 120.0);
    const better = inWindow && !bestInWindow;
    const tieBreak = inWindow === bestInWindow && dist < bestDist;
    if (better || tieBreak) {
      best = { bpm, beats };
      bestDist = dist;
      bestInWindow = inWindow;
    }
  }
  return best;
}

class ParamBridge {
  constructor(node) {
    this.node = node;
    this.suffixToPath = new Map();
    this.knownPaths = [];
    const rawParams = node.getParams ? node.getParams() : [];
    for (const entry of rawParams) {
      const path = typeof entry === "string" ? entry : entry.address || entry.path;
      if (path) this.knownPaths.push(path);
    }
    this.outputValues = new Map();
    node.port.addEventListener("message", (event) => {
      const data = event.data;
      if (data && data.type === "out-param") this.outputValues.set(data.path, data.value);
    });
    node.port.start();
  }

  resolve(suffix) {
    const exact = this.suffixToPath.get(suffix);
    if (exact) return exact;
    if (this.knownPaths.includes(suffix)) {
      this.suffixToPath.set(suffix, suffix);
      return suffix;
    }
    for (const path of this.knownPaths) {
      if (path.endsWith(suffix)) {
        this.suffixToPath.set(suffix, path);
        return path;
      }
    }
    for (const path of this.outputValues.keys()) {
      if (path.endsWith(suffix)) {
        this.suffixToPath.set(suffix, path);
        return path;
      }
    }
    return suffix;
  }

  set(suffix, value) {
    this.node.setParamValue(this.resolve(suffix), value);
  }

  get(suffix) {
    const resolved = this.resolve(suffix);
    if (this.outputValues.has(resolved)) return this.outputValues.get(resolved);
    try {
      const v = this.node.getParamValue(resolved);
      return typeof v === "number" && !Number.isNaN(v) ? v : 0.0;
    } catch (err) {
      return 0.0;
    }
  }
}

async function fetchText(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error("fetch failed " + url + " " + res.status);
  return res.text();
}

async function mirrorRepoTreeIntoCompilerFs(compiler, repoRootUrl, fs) {
  const dspRootFiles = ["aloop.dsp", "loop.dsp", "effects_runtime.dsp"];
  const homeFaustFiles = [
    "filters.dsp", "delay.dsp", "reverb.dsp", "microrepeat.dsp",
    "multitranspose.dsp", "resonode_synth.dsp", "guitar_lofi_fx.dsp",
    "flanger.dsp", "tremolo.dsp", "phaser.dsp", "compressor.dsp",
    "bitcrush.dsp", "vinyl.dsp", "flutter.dsp", "samplerate.dsp",
    "chain.dsp",
  ];
  fs.mkdirTree("/aloop-src/dsp");
  fs.mkdirTree("/aloop-src/effects/home/faust");
  for (const name of dspRootFiles) {
    const text = await fetchText(repoRootUrl + "../../dsp/" + name);
    fs.writeFile("/aloop-src/dsp/" + name, text);
  }
  for (const name of homeFaustFiles) {
    const text = await fetchText(repoRootUrl + "../../effects/home/faust/" + name);
    fs.writeFile("/aloop-src/effects/home/faust/" + name, text);
  }
  const pitchStub = await fetchText(repoRootUrl + "../demo/faust/pitch_stub_browser.dsp");
  fs.writeFile("/aloop-src/effects/home/faust/pitch.dsp", pitchStub);
}

export class AloopFaustEngine {
  constructor(repoRootUrl) {
    this.repoRootUrl = repoRootUrl;
    this.ready = false;
    this.audioContext = null;
    this.homeBridge = null;
    this.guitarBridge = null;
    this.controlNode = null;
    this.mic = null;

    this.looperHeld = new Array(LOOPER_COUNT).fill(false);
    this.looperHoldStart = new Array(LOOPER_COUNT).fill(0);
    this.looperErased = new Array(LOOPER_COUNT).fill(false);
    this.looperArmedOnPress = new Array(LOOPER_COUNT).fill(false);
    this.looperPlaying = new Array(LOOPER_COUNT).fill(false);
    this.looperHasContent = new Array(LOOPER_COUNT).fill(false);
    this.looperRecording = new Array(LOOPER_COUNT).fill(false);
    this.looperFinishTargetPending = new Array(LOOPER_COUNT).fill(0);
    this.looperShiftHeldDuringTake = new Array(LOOPER_COUNT).fill(false);
    this.looperIsSidechainSource = new Array(LOOPER_COUNT).fill(false);
    this.masterLenSamples = 0;
    this.recordedBpm = 0;

    this.shiftHeld = false;
    this.liveEngaged = false;
    this.guitarFxHeld = false;
    this.activeBank = "dub";
    this.fxBankValues = {
      dub: [0, 0, 0.5, 0, 0, 1, 0],
      guitar: [0, 0, 0.5, 0, 0, 0, 0],
      lofi: [0, 0, 0, 0, 0, 0, 0],
    };

    this.granulatorHeld = false;
    this.granulatorLatched = false;
    this.granulatorPressStartMs = 0;
    this.resonodeEngaged = false;
    this.bankBeforeGranulatorHold = "dub";

    this.transposeVoiceNote = new Array(TRANSPOSE_VOICE_COUNT).fill(-1);
    this.transposeVoiceOrder = new Array(TRANSPOSE_VOICE_COUNT).fill(0);
    this.transposeVoiceCounter = 0;
    this.resonodeVoiceNote = new Array(RESONODE_VOICE_COUNT).fill(-1);
    this.resonodeVoiceOrder = new Array(RESONODE_VOICE_COUNT).fill(0);
    this.resonodeVoiceCounter = 0;
  }

  async start() {
    if (this.ready) return;
    const faustwasmModule = await import(FAUSTWASM_INDEX_URL);
    const { instantiateFaustModuleFromFile, LibFaust, FaustCompiler, FaustMonoDspGenerator } = faustwasmModule;

    this.audioContext = new (window.AudioContext || window.webkitAudioContext)();
    this.mic = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } });

    const faustModule = await instantiateFaustModuleFromFile(LIBFAUST_MODULE_URL);
    const libFaust = new LibFaust(faustModule);
    const compiler = new FaustCompiler(libFaust);
    await mirrorRepoTreeIntoCompilerFs(compiler, this.repoRootUrl, compiler.fs());

    const includeArgs = "-I /aloop-src -I /aloop-src/dsp -I /aloop-src/effects/home/faust";

    const homeSource = compiler.fs().readFile("/aloop-src/dsp/aloop.dsp", { encoding: "utf8" });
    const homeGenerator = new FaustMonoDspGenerator();
    await homeGenerator.compile(compiler, "aloop_home", homeSource, includeArgs);
    const homeNode = await homeGenerator.createNode(this.audioContext);
    homeNode.channelCount = 32;
    homeNode.channelCountMode = "explicit";
    homeNode.channelInterpretation = "discrete";
    this.homeBridge = new ParamBridge(homeNode);
    this.homeNode = homeNode;

    const guitarSource = compiler.fs().readFile("/aloop-src/effects/home/faust/guitar_lofi_fx.dsp", { encoding: "utf8" });
    const guitarGenerator = new FaustMonoDspGenerator();
    await guitarGenerator.compile(compiler, "aloop_core3", guitarSource, includeArgs);
    const guitarNode = await guitarGenerator.createNode(this.audioContext);
    this.guitarBridge = new ParamBridge(guitarNode);
    this.guitarNode = guitarNode;

    await this.audioContext.audioWorklet.addModule(this.repoRootUrl + "../demo/faust/control-processor.js");
    const controlNode = new AudioWorkletNode(this.audioContext, "aloop-control-processor", {
      numberOfInputs: 3,
      numberOfOutputs: 1,
      outputChannelCount: [32],
      channelCountMode: "explicit",
      channelInterpretation: "discrete",
    });
    this.controlNode = controlNode;

    const micSource = this.audioContext.createMediaStreamSource(this.mic);
    micSource.connect(controlNode, 0, 0);

    const homeOutputSplitter = this.audioContext.createChannelSplitter(3);
    homeNode.connect(homeOutputSplitter);

    const loopSumDelay = this.audioContext.createDelay(1.0);
    loopSumDelay.delayTime.value = RENDER_QUANTUM_DELAY_SECONDS;
    homeOutputSplitter.connect(loopSumDelay, 1, 0);
    loopSumDelay.connect(controlNode, 0, 1);

    const recordTapDelay = this.audioContext.createDelay(1.0);
    recordTapDelay.delayTime.value = RENDER_QUANTUM_DELAY_SECONDS;
    homeOutputSplitter.connect(recordTapDelay, 2, 0);
    recordTapDelay.connect(controlNode, 0, 2);

    controlNode.connect(homeNode);

    homeOutputSplitter.connect(guitarNode, 0, 0);
    guitarNode.connect(this.audioContext.destination);

    this.controlNode.port.postMessage({ target: "masterPhaseSamples", value: 0 });
    this.ready = true;
  }

  sendControl(target, value) {
    this.controlNode.port.postMessage({ target, value });
  }

  setLooperField(index, field, value) {
    this.homeBridge.set(looperGroupSegment(index) + "/" + field, value);
  }

  writeIdxOf(index) {
    return this.homeBridge.get(looperGroupSegment(index) + "/writeidx");
  }
  levelOf(index) {
    return this.homeBridge.get(looperGroupSegment(index) + "/level");
  }

  applyRecPlayCycle(index) {
    if (this.looperRecording[index]) {
      this.setLooperField(index, "rec", 0.0);
      this.looperHasContent[index] = true;
      this.looperPlaying[index] = true;
      this.setLooperField(index, "play", 1.0);
      const latencyBias = 64 + (this.looperShiftHeldDuringTake[index] ? 64 : 0);
      this.setLooperField(index, "latencybias", latencyBias);

      if (this.masterLenSamples === 0) {
        let lenSamples = this.writeIdxOf(index);
        if (!(lenSamples >= 64)) lenSamples = 64;
        if (lenSamples > MAX_LOOP_SAMPLES) lenSamples = MAX_LOOP_SAMPLES;
        this.masterLenSamples = lenSamples;
        const solved = deriveTempoQuant(this.masterLenSamples / 48000.0);
        this.recordedBpm = solved.bpm;
        this.sendControl("masterLenSamples", this.masterLenSamples);
        this.setLooperField(index, "finishtarget", this.masterLenSamples);
        this.setLooperField(index, "finishreq", 1.0);
        setTimeout(() => this.setLooperField(index, "finishreq", 0.0), 50);
        this.looperRecording[index] = false;
      } else {
        const rawSamples = this.writeIdxOf(index);
        const log2Ratio = Math.log2(rawSamples / this.masterLenSamples);
        let lowerExp = Math.floor(log2Ratio);
        if (lowerExp < -4) lowerExp = -4;
        let lowerCand = this.masterLenSamples * Math.pow(2, lowerExp);
        let upperCand = this.masterLenSamples * Math.pow(2, lowerExp + 1);
        if (upperCand > MAX_LOOP_SAMPLES) upperCand = MAX_LOOP_SAMPLES;
        if (lowerCand > upperCand) lowerCand = upperCand;
        let bestLen;
        if (upperCand <= lowerCand) {
          bestLen = lowerCand;
        } else {
          const midpoint = Math.sqrt(lowerCand * upperCand);
          bestLen = rawSamples >= midpoint ? upperCand : lowerCand;
        }
        let quantized = Math.round(bestLen);
        if (quantized < 64) quantized = 64;
        if (quantized > MAX_LOOP_SAMPLES) quantized = MAX_LOOP_SAMPLES;
        this.setLooperField(index, "finishtarget", quantized);
        this.setLooperField(index, "finishreq", 1.0);
        setTimeout(() => this.setLooperField(index, "finishreq", 0.0), 50);
        this.looperFinishTargetPending[index] = quantized;
      }
    } else if (!this.looperHasContent[index]) {
      this.setLooperField(index, "rec", 1.0);
      this.looperRecording[index] = true;
      this.looperShiftHeldDuringTake[index] = this.shiftHeld;
    } else if (this.looperPlaying[index]) {
      this.setLooperField(index, "play", 0.0);
      this.looperPlaying[index] = false;
    } else {
      this.setLooperField(index, "play", 1.0);
      this.looperPlaying[index] = true;
    }
  }

  padDown(index, nowMs) {
    if (this.guitarFxHeld) {
      this.looperIsSidechainSource[index] = !this.looperIsSidechainSource[index];
      this.setLooperField(index, "sidechainsrc", this.looperIsSidechainSource[index] ? 1.0 : 0.0);
      return;
    }
    if (this.looperHeld[index]) return;
    this.looperHeld[index] = true;
    this.looperErased[index] = false;
    this.looperHoldStart[index] = nowMs;
    if (!this.looperHasContent[index] || this.looperRecording[index]) {
      this.applyRecPlayCycle(index);
      this.looperArmedOnPress[index] = true;
    } else {
      this.looperArmedOnPress[index] = false;
    }
  }

  padUp(index) {
    if (this.looperArmedOnPress[index]) {
      this.looperArmedOnPress[index] = false;
      this.looperHeld[index] = false;
      return;
    }
    if (this.looperHeld[index] && !this.looperErased[index]) {
      this.applyRecPlayCycle(index);
    }
    this.looperHeld[index] = false;
  }

  eraseLooper(index) {
    this.setLooperField(index, "erase", 1.0);
    setTimeout(() => this.setLooperField(index, "erase", 0.0), 50);
    if (this.looperRecording[index]) {
      this.setLooperField(index, "rec", 0.0);
      this.looperRecording[index] = false;
    }
    this.looperErased[index] = true;
    this.looperArmedOnPress[index] = false;
    this.looperHasContent[index] = false;
    this.looperPlaying[index] = false;
    this.setLooperField(index, "play", 0.0);
    this.looperIsSidechainSource[index] = false;
    this.setLooperField(index, "sidechainsrc", 0.0);
    if (!this.looperHasContent.some(Boolean)) {
      this.masterLenSamples = 0;
      this.recordedBpm = 0;
      this.sendControl("masterLenSamples", 0);
    }
  }

  pollHolds(nowMs) {
    if (this.granulatorHeld && !this.resonodeEngaged && nowMs - this.granulatorPressStartMs >= GRANULATOR_TAP_MS) {
      this.resonodeEngaged = true;
      this.homeBridge.set("fx/resonode/engaged", 1.0);
    }
    for (let i = 0; i < LOOPER_COUNT; i++) {
      if (this.looperFinishTargetPending[i] <= 0) continue;
      if (this.writeIdxOf(i) >= this.looperFinishTargetPending[i]) {
        this.looperRecording[i] = false;
        this.looperFinishTargetPending[i] = 0;
      }
    }
    for (let i = 0; i < LOOPER_COUNT; i++) {
      if (!this.looperHeld[i] || this.looperErased[i]) continue;
      if (nowMs - this.looperHoldStart[i] < HOLD_ERASE_MS) continue;
      this.eraseLooper(i);
    }
    this.updateSidechainEnv();
  }

  clearAll() {
    this.sendControl("clearAll", 1.0);
    setTimeout(() => this.sendControl("clearAll", 0.0), 50);
    for (let i = 0; i < LOOPER_COUNT; i++) {
      this.looperHeld[i] = false;
      this.looperErased[i] = false;
      this.looperArmedOnPress[i] = false;
      this.looperPlaying[i] = false;
      this.looperHasContent[i] = false;
      this.looperRecording[i] = false;
      this.looperIsSidechainSource[i] = false;
      this.setLooperField(i, "sidechainsrc", 0.0);
      this.setLooperField(i, "play", 0.0);
      this.setLooperField(i, "rec", 0.0);
      this.setLooperField(i, "finishreq", 0.0);
    }
    this.masterLenSamples = 0;
    this.recordedBpm = 0;
    this.sendControl("masterLenSamples", 0);
    this.releaseAllResonodeVoices();
    for (let v = 0; v < TRANSPOSE_VOICE_COUNT; v++) this.releaseTransposeVoiceSlot(v);
  }

  stopAll() {
    for (let i = 0; i < LOOPER_COUNT; i++) {
      if (this.looperRecording[i]) {
        this.setLooperField(i, "rec", 0.0);
        this.looperRecording[i] = false;
      }
      this.setLooperField(i, "play", 0.0);
      this.looperPlaying[i] = false;
    }
  }

  setShift(held) {
    this.shiftHeld = held;
    this.homeBridge.set("fx/monitorfold", held ? 1.0 : 0.0);
    this.sendControl("foldTarget", held ? 1.0 : 0.0);
    this.sendControl("freeXpose", held ? 1.0 : 0.0);
  }

  setBank(bank) {
    this.activeBank = bank;
  }

  setKnob(knobIndex, v01) {
    this.fxBankValues[this.activeBank][knobIndex] = v01;
    if (this.activeBank === "lofi" && knobIndex > 0) {
      if (this.resonodeEngaged) {
        if (knobIndex <= RESONODE_PATCHES.length) this.applyResonodePatchMorph();
        else this.applyResonodeDirectKnob(knobIndex, v01);
      }
      return;
    }
    if (this.activeBank === "lofi" && knobIndex === 0) {
      this.guitarBridge.set("fx2/BITCRUSHAMT", v01);
      return;
    }
    const targets = this.activeBank === "dub" ? DUB_TARGETS : GUITAR_TARGETS;
    const target = targets[knobIndex];
    if (!target) return;
    const value = target === "fx/reverb" ? v01 * 2.0 : v01;
    if (target.startsWith("fx2/")) this.guitarBridge.set(target, value);
    else this.homeBridge.set(target, value);
  }

  applyResonodePatchMorph() {
    const weights = this.fxBankValues.lofi.slice(1, 1 + RESONODE_PATCHES.length);
    const total = weights.reduce((a, b) => a + b, 0);
    let blend = RESONODE_PATCHES[0];
    if (total > 0.0001) {
      blend = { position: 0, decay: 0, damping: 0, stretch: 0, collision: 0 };
      for (let p = 0; p < RESONODE_PATCHES.length; p++) {
        const wn = weights[p] / total;
        blend.position += wn * RESONODE_PATCHES[p].position;
        blend.decay += wn * RESONODE_PATCHES[p].decay;
        blend.damping += wn * RESONODE_PATCHES[p].damping;
        blend.stretch += wn * RESONODE_PATCHES[p].stretch;
        blend.collision += wn * RESONODE_PATCHES[p].collision;
      }
    }
    this.homeBridge.set("fx/resonode/position", blend.position);
    this.homeBridge.set("fx/resonode/decay", blend.decay);
    this.homeBridge.set("fx/resonode/damping", blend.damping);
    this.homeBridge.set("fx/resonode/stretch", blend.stretch);
    this.homeBridge.set("fx/resonode/collision", blend.collision);
  }

  applyResonodeDirectKnob(knobIndex, v01) {
    const i = knobIndex - 1 - RESONODE_PATCHES.length;
    if (i < 0 || i >= RESONODE_DIRECT_RANGES.length) return;
    const r = RESONODE_DIRECT_RANGES[i];
    const v = r.logTaper ? r.lo * Math.pow(r.hi / r.lo, v01) : r.lo + v01 * (r.hi - r.lo);
    this.homeBridge.set(r.zone, v);
  }

  onDubFxPress() { this.setBank("dub"); }
  onGuitarFxPress() { this.guitarFxHeld = true; this.setBank("guitar"); }
  onGuitarFxRelease() { this.guitarFxHeld = false; }
  onLofiFxPress(nowMs) {
    if (this.granulatorHeld) return;
    this.bankBeforeGranulatorHold = this.activeBank;
    this.setBank("lofi");
    this.granulatorHeld = true;
    this.granulatorPressStartMs = nowMs;
    this.resonodeEngaged = false;
    this.granulatorLatched = !this.granulatorLatched;
  }
  onLofiFxRelease(_nowMs) {
    if (this.resonodeEngaged) {
      this.releaseAllResonodeVoices();
      this.resonodeEngaged = false;
      this.homeBridge.set("fx/resonode/engaged", 0.0);
    }
    this.granulatorHeld = false;
    this.setBank(this.bankBeforeGranulatorHold);
  }

  allocateTransposeVoice(note) {
    for (let v = 0; v < TRANSPOSE_VOICE_COUNT; v++) if (this.transposeVoiceNote[v] === note) return v;
    for (let v = 0; v < TRANSPOSE_VOICE_COUNT; v++) {
      if (this.transposeVoiceNote[v] >= 0) continue;
      this.transposeVoiceNote[v] = note;
      this.transposeVoiceOrder[v] = ++this.transposeVoiceCounter;
      return v;
    }
    let oldest = 0;
    for (let v = 1; v < TRANSPOSE_VOICE_COUNT; v++) if (this.transposeVoiceOrder[v] < this.transposeVoiceOrder[oldest]) oldest = v;
    this.transposeVoiceNote[oldest] = note;
    this.transposeVoiceOrder[oldest] = ++this.transposeVoiceCounter;
    return oldest;
  }
  releaseTransposeVoiceSlot(v) {
    this.transposeVoiceNote[v] = -1;
    this.sendControl("xposeVoice", { voice: v, note: 0, gate: 0 });
  }
  releaseTransposeVoiceByNote(note) {
    for (let v = 0; v < TRANSPOSE_VOICE_COUNT; v++) {
      if (this.transposeVoiceNote[v] !== note) continue;
      this.releaseTransposeVoiceSlot(v);
      return;
    }
  }

  allocateResonodeVoice(note) {
    for (let v = 0; v < RESONODE_VOICE_COUNT; v++) if (this.resonodeVoiceNote[v] === note) return v;
    for (let v = 0; v < RESONODE_VOICE_COUNT; v++) {
      if (this.resonodeVoiceNote[v] >= 0) continue;
      this.resonodeVoiceNote[v] = note;
      this.resonodeVoiceOrder[v] = ++this.resonodeVoiceCounter;
      return v;
    }
    let oldest = 0;
    for (let v = 1; v < RESONODE_VOICE_COUNT; v++) if (this.resonodeVoiceOrder[v] < this.resonodeVoiceOrder[oldest]) oldest = v;
    this.resonodeVoiceNote[oldest] = note;
    this.resonodeVoiceOrder[oldest] = ++this.resonodeVoiceCounter;
    return oldest;
  }
  releaseResonodeVoiceByNote(note) {
    for (let v = 0; v < RESONODE_VOICE_COUNT; v++) {
      if (this.resonodeVoiceNote[v] !== note) continue;
      this.resonodeVoiceNote[v] = -1;
      this.sendControl("resonodeVoice", { voice: v, note: 0, gate: 0, vel: 1 });
      return;
    }
  }
  releaseAllResonodeVoices() {
    for (let v = 0; v < RESONODE_VOICE_COUNT; v++) {
      this.resonodeVoiceNote[v] = -1;
      this.sendControl("resonodeVoice", { voice: v, note: 0, gate: 0, vel: 1 });
    }
  }

  keyDown(note, velocity127) {
    if (this.resonodeEngaged) {
      const v = this.allocateResonodeVoice(note);
      this.sendControl("resonodeVoice", { voice: v, note, gate: 1, vel: velocity127 / 127.0 });
      return;
    }
    this.liveEngaged = true;
    const v = this.allocateTransposeVoice(note);
    this.sendControl("xposeVoice", { voice: v, note, gate: 1 });
  }
  keyUp(note) {
    if (this.resonodeEngaged) {
      this.releaseResonodeVoiceByNote(note);
      return;
    }
    this.releaseTransposeVoiceByNote(note);
  }

  onLiveEngageToggle() {
    this.liveEngaged = !this.liveEngaged;
    if (!this.liveEngaged) {
      this.homeBridge.set("fx/pitchbend", 0.0);
      this.homeBridge.set("fx/pitchbend_engaged", 0.0);
      for (let v = 0; v < TRANSPOSE_VOICE_COUNT; v++) {
        if (this.transposeVoiceNote[v] < 0) continue;
        this.releaseTransposeVoiceSlot(v);
      }
    }
  }

  setModWheel(v01) {
    if (!this.liveEngaged) {
      this.homeBridge.set("fx/pitchbend_engaged", 0.0);
      this.homeBridge.set("fx/pitchbend", 0.0);
      return;
    }
    const data2 = Math.round(v01 * 127);
    if (data2 >= 59 && data2 <= 69) {
      this.homeBridge.set("fx/pitchbend_engaged", 0.0);
      this.homeBridge.set("fx/pitchbend", 0.0);
      return;
    }
    const semis = ((data2 - 64) * 12.0) / 63.0;
    this.homeBridge.set("fx/pitchbend", semis);
    this.homeBridge.set("fx/pitchbend_engaged", 1.0);
  }

  setFormant(v01) {
    const data2 = Math.round(v01 * 127);
    if (data2 >= 60 && data2 <= 68) {
      this.homeBridge.set("fx/formant", 0.0);
      return;
    }
    const range = this.shiftHeld ? 3.0 : 1.0;
    let v = ((data2 - 64) / 63.0) * range;
    if (v > 3.0) v = 3.0;
    if (v < -3.0) v = -3.0;
    this.homeBridge.set("fx/formant", v);
  }

  setMicrorepeatDiv(div) {
    this.homeBridge.set("fx/microrepeat_div", div);
    this.sendControl("glitchFoldTarget", div > 0 ? 1.0 : 0.0);
  }

  updateSidechainEnv() {
    let env = 0.0;
    for (let i = 0; i < LOOPER_COUNT; i++) {
      if (!this.looperIsSidechainSource[i]) continue;
      const lvl = this.levelOf(i);
      if (lvl > env) env = lvl;
    }
    this.sendControl("sidechainEnv", env);
  }
}
