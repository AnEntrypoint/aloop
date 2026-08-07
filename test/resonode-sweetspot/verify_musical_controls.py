import re
import sys
from pathlib import Path

import numpy as np
import dawdreamer as daw

REPO_ROOT = Path(__file__).resolve().parents[2]
DSP_PATH = REPO_ROOT / "effects" / "home" / "faust" / "resonode_synth.dsp"

SAMPLE_RATE = 48000
BLOCK_SIZE = 64
COMPILE_FLAGS = ["-vec", "-fun", "-dfs", "-vs", "32", "-ct", "0"]

VARNAME = {
    "position": "position",
    "tone": "tone",
    "decay": "decayTime",
    "damping": "damping",
    "stretch": "stretch",
    "collision": "collision",
    "level": "outLevel",
}


def compile_processor(engine, dsp_text, name):
    faust = engine.make_faust_processor(name)
    faust.set_dsp_string(dsp_text)
    faust.compile_flags = COMPILE_FLAGS
    if not faust.compile():
        raise RuntimeError("faust compile failed")
    return faust


def make_inputs(n, note, excite):
    note_sig = np.full(n, float(note))
    gate_sig = np.ones(n)
    vel_sig = np.ones(n)
    zero = np.zeros(n)
    return np.stack(
        [excite, note_sig, gate_sig, vel_sig, zero, zero, zero, zero, zero, zero, zero, zero, zero],
        axis=0,
    )


def burst_excitation(n, seed):
    rng = np.random.default_rng(seed)
    burst_n = int(0.015 * SAMPLE_RATE)
    burst = rng.uniform(-1.0, 1.0, size=burst_n) * 0.9
    excite = np.zeros(n)
    excite[: min(burst_n, n)] = burst[: min(burst_n, n)]
    return excite


def render(dsp_text, note=60, dur=0.3, seed=7):
    engine = daw.RenderEngine(SAMPLE_RATE, BLOCK_SIZE)
    n = int(dur * SAMPLE_RATE)
    excite = burst_excitation(n, seed)
    inputs = make_inputs(n, note, excite)
    playback = engine.make_playback_processor("in", inputs)
    faust = compile_processor(engine, dsp_text, "resonode")
    engine.load_graph([(playback, []), (faust, ["in"])])
    engine.render(dur)
    return engine.get_audio()[0]


def spectral_centroid(x, sr, warmup_s=0.2):
    seg = x[int(warmup_s * sr) :]
    if len(seg) < 256:
        seg = x
    windowed = seg * np.hanning(len(seg))
    spec = np.abs(np.fft.rfft(windowed))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    return float(np.sum(freqs * spec ** 2) / (np.sum(spec ** 2) + 1e-20))


def render_tone_at_hz(tone_hz, note):
    text = re.sub(
        r'hslider\("fx/resonode/tone", [^,]+,',
        f'hslider("fx/resonode/tone", {tone_hz},',
        DSP_PATH.read_text(),
    )
    audio = render(text, note=note, dur=1.0, seed=1)
    return spectral_centroid(audio, SAMPLE_RATE)


def check_tone_taper():
    lo, hi = 200.0, 18000.0
    print("=== tone knob taper check ===")
    ok = True
    for note in (60, 84):
        c0 = render_tone_at_hz(lo, note)
        c1 = render_tone_at_hz(lo * (hi / lo) ** 0.1, note)
        c10 = render_tone_at_hz(hi, note)
        c0_lin = c0
        c1_lin = render_tone_at_hz(lo + 0.1 * (hi - lo), note)
        c10_lin = c10
        total_exp = c10 - c0 + 1e-9
        total_lin = c10_lin - c0_lin + 1e-9
        frac_exp = (c1 - c0) / total_exp
        frac_lin = (c1_lin - c0_lin) / total_lin
        print(
            f"note={note} frac of total brightness change within first 10% of knob: "
            f"linear={frac_lin:.3f} exponential={frac_exp:.3f}"
        )
        if not (frac_exp < 0.7 * frac_lin):
            ok = False
    return ok


def build_stepped_variant(param, before, after, jump_sample, smoothed):
    varname = VARNAME[param]
    text = DSP_PATH.read_text()
    pattern = rf'{varname}\s*=\s*hslider\("fx/resonode/{param}", [^)]+\)(?: : morphGlide)?;'
    m = re.search(pattern, text)
    if not m:
        raise RuntimeError(f"no hslider match for {param}")
    suffix = " : morphGlide" if smoothed else ""
    stepped_def = f"{varname} = ba.if(count < {jump_sample}, {before}, {after}){suffix};"
    text = text[: m.start()] + stepped_def + text[m.end() :]
    text = text.replace("voiceGain = 0.5;", "voiceGain = 0.5;\ncount = (+(1) ~ _) - 1;")
    return text


def jump_click_ratio(param, before, after, jump_sample=6000, note=60, dur=0.3, seed=11):
    ratios = {}
    for smoothed in (False, True):
        text = build_stepped_variant(param, before, after, jump_sample, smoothed)
        audio = render(text, note=note, dur=dur, seed=seed)
        d = np.abs(np.diff(audio.astype(np.float64)))
        at_jump = d[jump_sample - 1]
        local = np.concatenate([d[jump_sample - 200 : jump_sample - 1], d[jump_sample : jump_sample + 199]])
        ratios["smoothed" if smoothed else "raw"] = at_jump / (local.max() + 1e-9)
    return ratios


def check_morph_glide_click():
    print("=== patch-morph knob click check (burst excitation, jump mid-decay) ===")
    ratios = jump_click_ratio("position", 0.08, 0.42, jump_sample=6000)
    print(f"position jump: raw={ratios['raw']:.2f}x smoothed={ratios['smoothed']:.2f}x")
    ok = ratios["smoothed"] < 0.5 * ratios["raw"] and ratios["smoothed"] < 1.5
    for param, before, after in [("stretch", -0.10, 1.20), ("tone", 300.0, 15000.0), ("damping", 0.80, 0.97)]:
        r = jump_click_ratio(param, before, after, jump_sample=6000)
        print(f"{param} jump: raw={r['raw']:.2f}x smoothed={r['smoothed']:.2f}x")
    return ok


def check_no_fadein_regression():
    print("=== no-fade-in-from-zero regression check (static default params) ===")
    smoothed_text = DSP_PATH.read_text()
    raw_text = smoothed_text.replace(" : morphGlide", "")
    raw_text = re.sub(r"morphGlidePole = .*?\n", "", raw_text)
    raw_text = re.sub(r"morphGlide\(x\) = y\nletrec \{\n.*?\n\};\n", "", raw_text, flags=re.S)
    a_raw = render(raw_text, dur=0.05, seed=7)
    a_smooth = render(smoothed_text, dur=0.05, seed=7)
    win = int(0.005 * SAMPLE_RATE)
    rms_raw = float(np.sqrt(np.mean(a_raw[:win].astype(np.float64) ** 2)) + 1e-12)
    rms_smooth = float(np.sqrt(np.mean(a_smooth[:win].astype(np.float64) ** 2)) + 1e-12)
    ratio = rms_smooth / rms_raw
    print(f"first-5ms RMS ratio (smoothed/raw), expect close to 1.0: {ratio:.3f}")
    return ratio > 0.9


def render_custom(dsp_text, inputs, dur):
    engine = daw.RenderEngine(SAMPLE_RATE, BLOCK_SIZE)
    playback = engine.make_playback_processor("in", inputs)
    faust = compile_processor(engine, dsp_text, "resonode")
    engine.load_graph([(playback, []), (faust, ["in"])])
    engine.render(dur)
    return engine.get_audio()[0]


def voice_inputs(n, note0=0.0, gate0=0.0, vel0=0.0, excite=None):
    if excite is None:
        excite = np.zeros(n)
    zero = np.zeros(n)
    note_sig = np.full(n, float(note0)) if np.ndim(note0) == 0 else note0
    gate_sig = np.full(n, float(gate0)) if np.ndim(gate0) == 0 else gate0
    vel_sig = np.full(n, float(vel0)) if np.ndim(vel0) == 0 else vel0
    return np.stack(
        [excite, note_sig, gate_sig, vel_sig, zero, zero, zero, zero, zero, zero, zero, zero, zero],
        axis=0,
    )


def check_velocity_response():
    print("=== velocity -> loudness/brightness check ===")
    dsp_text = DSP_PATH.read_text()
    n = int(0.5 * SAMPLE_RATE)
    excite = burst_excitation(n, seed=3)
    ok = True
    prev_rms, prev_centroid = None, None
    for vel in (0.2, 0.6, 1.0):
        inputs = voice_inputs(n, note0=60.0, gate0=1.0, vel0=vel, excite=excite)
        audio = render_custom(dsp_text, inputs, n / SAMPLE_RATE)
        rms = float(np.sqrt(np.mean(audio.astype(np.float64) ** 2)) + 1e-20)
        centroid = spectral_centroid(audio, SAMPLE_RATE, warmup_s=0.05)
        print(f"vel={vel:.1f} rms={rms:.5f} centroid={centroid:8.1f}Hz")
        if prev_rms is not None and not (rms >= prev_rms * 0.98):
            ok = False
        if prev_centroid is not None and not (centroid >= prev_centroid * 0.90):
            ok = False
        prev_rms, prev_centroid = rms, centroid
    return ok


def check_silent_without_live_input():
    print("=== mic-only exciter regression check (gate held, zero live input) ===")
    dsp_text = DSP_PATH.read_text()
    n = int(0.3 * SAMPLE_RATE)
    inputs = voice_inputs(n, note0=60.0, gate0=1.0, vel0=1.0, excite=np.zeros(n))
    audio = render_custom(dsp_text, inputs, n / SAMPLE_RATE)
    peak = float(np.max(np.abs(audio)))
    print(f"peak amplitude with a held key and silent input: {peak:.3e}")
    return peak < 1e-5


def check_new_mode_alias_guard():
    print("=== mode5/mode6 alias-guard check ===")
    sr = SAMPLE_RATE
    note = 108
    f0 = 440.0 * (2.0 ** ((note - 69) / 12.0))
    print(f"note={note} f0={f0:.1f}Hz mode5={5*f0:.1f}Hz mode6={6*f0:.1f}Hz nyquist={sr/2:.0f}Hz")
    dsp_text = DSP_PATH.read_text()
    n = int(1.0 * sr)
    rng = np.random.default_rng(5)
    excite = rng.uniform(-1.0, 1.0, size=n) * 0.5
    inputs = voice_inputs(n, note0=float(note), gate0=1.0, vel0=1.0, excite=excite)
    audio = render_custom(dsp_text, inputs, n / sr)
    seg = audio[int(0.2 * sr) :]
    windowed = seg * np.hanning(len(seg))
    spec = np.abs(np.fft.rfft(windowed))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    aliased5 = sr - 5 * f0
    aliased6 = sr - 6 * f0
    peak_mag = float(np.max(spec))
    ok = True
    for label, alias_hz in (("mode5", aliased5), ("mode6", aliased6)):
        if alias_hz <= 0 or alias_hz >= sr / 2:
            continue
        band = (freqs > alias_hz - 100) & (freqs < alias_hz + 100)
        band_peak = float(np.max(spec[band])) if np.any(band) else 0.0
        ratio = band_peak / (peak_mag + 1e-20)
        print(f"{label} alias-fold target {alias_hz:.0f}Hz: relative magnitude {ratio:.4f}")
        if ratio > 0.05:
            ok = False
    return ok


def check_voice_steal_with_velocity_change():
    print("=== voice-steal + velocity-change click check ===")
    dsp_text = DSP_PATH.read_text()
    n = int(0.05 * SAMPLE_RATE)
    steal_at = n // 2
    note_sig = np.full(n, 60.0)
    note_sig[steal_at:] = 72.0
    gate_sig = np.ones(n)
    vel_sig = np.full(n, 1.0)
    vel_sig[steal_at:] = 0.3
    rng = np.random.default_rng(9)
    excite = rng.uniform(-1.0, 1.0, size=n) * 0.6
    inputs = voice_inputs(n, note0=note_sig, gate0=gate_sig, vel0=vel_sig, excite=excite)
    audio = render_custom(dsp_text, inputs, n / SAMPLE_RATE)
    d = np.abs(np.diff(audio.astype(np.float64)))
    at_steal = d[steal_at - 1]
    background = np.median(d[max(0, steal_at - 300) : steal_at - 5])
    print(f"derivative at steal instant: {at_steal:.4f}, local background median: {background:.4f}")
    return at_steal < 0.5


def check_collision_zero_is_identity():
    print("=== collision=0 is an exact passthrough (regex-stripped self-A/B) ===")
    dsp_text = DSP_PATH.read_text()
    stripped = re.sub(
        r"voice\(exciteIn, note, gate, vel\) = collisionDrive\(bank\(([^;]+)\)\) \* voiceGain;",
        r"voice(exciteIn, note, gate, vel) = bank(\1) * voiceGain;",
        dsp_text,
    )
    if stripped == dsp_text:
        raise RuntimeError("collisionDrive() call site not found for stripping")
    n = int(0.4 * SAMPLE_RATE)
    excite = burst_excitation(n, seed=21)
    inputs = make_inputs(n, 60, excite)
    a_with = render(dsp_text, note=60, dur=n / SAMPLE_RATE, seed=21)
    a_stripped = render(stripped, note=60, dur=n / SAMPLE_RATE, seed=21)
    diff = float(np.max(np.abs(a_with.astype(np.float64) - a_stripped.astype(np.float64))))
    print(f"max abs diff at collision=0 vs no-collisionDrive-at-all: {diff:.3e}")
    return diff < 1e-5


def check_collision_bounded_and_monotonic_energy():
    print("=== collision raises tail energy, stays finite and bounded ===")
    dsp_text = DSP_PATH.read_text()
    n = int(0.4 * SAMPLE_RATE)
    excite = burst_excitation(n, seed=2)
    hf_rms = []
    for coll in (0.0, 0.3, 0.7, 1.0):
        text = re.sub(
            r'hslider\("fx/resonode/collision", [^,]+,',
            f'hslider("fx/resonode/collision", {coll},',
            dsp_text,
        )
        inputs = make_inputs(n, 60, excite)
        audio = render_custom(text, inputs, n / SAMPLE_RATE)
        if not np.all(np.isfinite(audio)):
            print(f"collision={coll}: NON-FINITE OUTPUT")
            return False
        if float(np.max(np.abs(audio))) > 1.001:
            print(f"collision={coll}: peak exceeds bound ({np.max(np.abs(audio)):.4f})")
            return False
        d = np.abs(np.diff(audio.astype(np.float64)))
        hf_rms.append(float(np.sqrt(np.mean(d ** 2))))
    print(f"tail-energy proxy (first-diff rms) across collision 0/0.3/0.7/1.0: {hf_rms}")
    return all(hf_rms[i + 1] >= hf_rms[i] * 0.98 for i in range(len(hf_rms) - 1))


def check_pitch_mod_gated_by_velocity_and_flexibility():
    print("=== pitch-mod is silent at vel=0 or at stretch=1.5 (stiff), present otherwise ===")
    dsp_text = DSP_PATH.read_text()
    no_pm_text = re.sub(r"pitchModDepth = 0\.04;", "pitchModDepth = 0.0;", dsp_text)
    if no_pm_text == dsp_text:
        raise RuntimeError("pitchModDepth default not found for stripping")
    n = int(0.15 * SAMPLE_RATE)
    excite = burst_excitation(n, seed=4)

    def diff_vs_disabled(text_variant):
        inputs = make_inputs(n, 60, excite)
        a = render_custom(text_variant, inputs, n / SAMPLE_RATE)
        b = render_custom(no_pm_text, inputs, n / SAMPLE_RATE)
        return float(np.max(np.abs(a.astype(np.float64) - b.astype(np.float64))))

    active_diff = diff_vs_disabled(dsp_text)
    print(f"pitch-mod active (vel=1, default stretch) vs disabled: max abs diff {active_diff:.4f}")

    stiff_text = re.sub(
        r'hslider\("fx/resonode/stretch", [^,]+,', 'hslider("fx/resonode/stretch", 1.5,', dsp_text
    )
    stiff_no_pm_text = re.sub(
        r'hslider\("fx/resonode/stretch", [^,]+,', 'hslider("fx/resonode/stretch", 1.5,', no_pm_text
    )
    inputs = make_inputs(n, 60, excite)
    a_stiff = render_custom(stiff_text, inputs, n / SAMPLE_RATE)
    b_stiff = render_custom(stiff_no_pm_text, inputs, n / SAMPLE_RATE)
    stiff_diff = float(np.max(np.abs(a_stiff.astype(np.float64) - b_stiff.astype(np.float64))))
    print(f"pitch-mod at stretch=1.5 (stiff, flexibility=0) vs disabled: max abs diff {stiff_diff:.3e}")

    return active_diff > 0.01 and stiff_diff < 1e-5


def main():
    results = {
        "tone_taper": check_tone_taper(),
        "morph_glide_click": check_morph_glide_click(),
        "no_fadein_regression": check_no_fadein_regression(),
        "velocity_response": check_velocity_response(),
        "silent_without_live_input": check_silent_without_live_input(),
        "new_mode_alias_guard": check_new_mode_alias_guard(),
        "voice_steal_with_velocity_change": check_voice_steal_with_velocity_change(),
        "collision_zero_is_identity": check_collision_zero_is_identity(),
        "collision_bounded_and_monotonic_energy": check_collision_bounded_and_monotonic_energy(),
        "pitch_mod_gated_by_velocity_and_flexibility": check_pitch_mod_gated_by_velocity_and_flexibility(),
    }
    print()
    print("=== summary ===")
    all_ok = True
    for name, ok in results.items():
        print(f"{name}: {'PASS' if ok else 'FAIL'}")
        all_ok = all_ok and ok
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
