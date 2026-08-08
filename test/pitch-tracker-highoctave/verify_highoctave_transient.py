import sys
from pathlib import Path

import numpy as np
import dawdreamer as daw

REPO_ROOT = Path(__file__).resolve().parents[2]
DSP_PATH = REPO_ROOT / "effects" / "home" / "faust" / "multitranspose.dsp"

SAMPLE_RATE = 48000
BLOCK_SIZE = 64
COMPILE_FLAGS = ["-vec", "-fun", "-dfs", "-vs", "32", "-ct", "0"]


def compile_processor(engine, dsp_text, name):
    faust = engine.make_faust_processor(name)
    faust.set_dsp_string(dsp_text)
    faust.compile_flags = COMPILE_FLAGS
    if not faust.compile():
        raise RuntimeError("faust compile failed")
    return faust


def sine_transient(n, freq_hz, attack_samples=64, amp=0.9):
    t = np.arange(n) / SAMPLE_RATE
    tone = amp * np.sin(2 * np.pi * freq_hz * t)
    env = np.ones(n)
    ramp = np.linspace(0.0, 1.0, attack_samples)
    env[:attack_samples] = ramp
    return tone * env


def make_inputs(n, dry, free, target_note, gate):
    zero = np.zeros(n)
    ones = np.ones(n)
    return np.stack(
        [
            dry,
            zero,
            free * ones,
            target_note * ones,
            gate * ones,
            zero, zero, zero, zero, zero, zero, zero, zero, zero, zero,
        ],
        axis=0,
    )


def render(dsp_text, freq_hz, target_note, dur=0.3, seed=1):
    engine = daw.RenderEngine(SAMPLE_RATE, BLOCK_SIZE)
    n = int(dur * SAMPLE_RATE)
    dry = sine_transient(n, freq_hz)
    inputs = make_inputs(n, dry, 0.0, target_note, 1.0)
    playback = engine.make_playback_processor("in", inputs)
    faust = compile_processor(engine, dsp_text, "multitranspose")
    engine.load_graph([(playback, []), (faust, ["in"])])
    engine.render(dur)
    return engine.get_audio()[0], dry


def instantaneous_freq_via_zero_crossings(x, sr, win_samples=512, hop=64):
    n = len(x)
    freqs = []
    times = []
    for start in range(0, n - win_samples, hop):
        seg = x[start:start + win_samples]
        signs = np.sign(seg)
        signs[signs == 0] = 1
        crossings = np.sum(np.abs(np.diff(signs)) > 0)
        dur_s = win_samples / sr
        f = crossings / (2.0 * dur_s)
        freqs.append(f)
        times.append((start + win_samples / 2) / sr)
    return np.array(times), np.array(freqs)


def cents_error(measured_hz, target_hz):
    if measured_hz <= 0 or target_hz <= 0:
        return float("nan")
    return 1200.0 * np.log2(measured_hz / target_hz)


def check_high_octave_transient(freq_hz, target_note_offset_semis=0):
    print(f"=== high-octave transient check: input={freq_hz}Hz ===")
    text = DSP_PATH.read_text()
    target_note = 69.0 + 12.0 * np.log2(freq_hz / 440.0) + target_note_offset_semis
    audio, dry = render(text, freq_hz, target_note, dur=0.3)
    times, freqs = instantaneous_freq_via_zero_crossings(audio, SAMPLE_RATE)
    target_hz = freq_hz * (2.0 ** (target_note_offset_semis / 12.0))
    early_mask = times < 0.10
    late_mask = times > 0.20
    early_errs = [cents_error(f, target_hz) for f in freqs[early_mask] if f > 0]
    late_errs = [cents_error(f, target_hz) for f in freqs[late_mask] if f > 0]
    early_max_abs = max((abs(e) for e in early_errs), default=float("nan"))
    late_max_abs = max((abs(e) for e in late_errs), default=float("nan"))
    early_signed_mean = np.mean(early_errs) if early_errs else float("nan")
    print(f"  target_hz={target_hz:.1f} early(<100ms) max|cents|={early_max_abs:.1f} "
          f"mean_signed_cents={early_signed_mean:.1f} late(>200ms) max|cents|={late_max_abs:.1f}")
    return early_signed_mean, early_max_abs, late_max_abs


def main():
    print("multitranspose.dsp high-octave transient pitch-tracking check")
    print(f"DSP: {DSP_PATH}")
    results = []
    for freq_hz in (880.0, 1046.5, 1318.5):
        early_signed, early_max, late_max = check_high_octave_transient(freq_hz)
        results.append((freq_hz, early_signed, early_max, late_max))

    print("\n=== summary ===")
    upslide_detected = False
    for freq_hz, early_signed, early_max, late_max in results:
        direction = "UP-SLIDE (starts flat, rises)" if early_signed < -20 else \
                    "DOWN-SLIDE (starts sharp, falls)" if early_signed > 20 else "converged fast"
        print(f"  {freq_hz:.1f}Hz: early_signed_cents={early_signed:.1f} ({direction}), "
              f"late_max_cents={late_max:.1f}")
        if early_signed < -20:
            upslide_detected = True

    if upslide_detected:
        print("\nCONFIRMED: high-octave transients show an up-slide convergence pattern "
              "(detNote reads low right after onset, shiftAmount reads high, output starts "
              "flat and rises into target pitch) -- matches the user-reported symptom.")
        sys.exit(1)
    else:
        print("\nNo significant up-slide detected at tested octaves within the checked window.")
        sys.exit(0)


if __name__ == "__main__":
    main()
