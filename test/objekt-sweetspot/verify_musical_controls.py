import re
import sys
from pathlib import Path

import numpy as np
import dawdreamer as daw

REPO_ROOT = Path(__file__).resolve().parents[2]
DSP_PATH = REPO_ROOT / "effects" / "home" / "faust" / "objekt_synth.dsp"

SAMPLE_RATE = 48000
BLOCK_SIZE = 64
COMPILE_FLAGS = ["-vec", "-fun", "-dfs", "-vs", "32", "-ct", "0"]

VARNAME = {
    "position": "position",
    "tone": "tone",
    "decay": "objDecay",
    "damping": "damping",
    "stretch": "stretch",
    "level": "objLevel",
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
    zero = np.zeros(n)
    return np.stack([excite, note_sig, gate_sig, zero, zero, zero, zero, zero, zero], axis=0)


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
    faust = compile_processor(engine, dsp_text, "objekt")
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
        r'hslider\("fx/objekt/tone", [^,]+,',
        f'hslider("fx/objekt/tone", {tone_hz},',
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
    pattern = rf'{varname}\s*=\s*hslider\("fx/objekt/{param}", [^)]+\)(?: : morphGlide)?;'
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


def main():
    results = {
        "tone_taper": check_tone_taper(),
        "morph_glide_click": check_morph_glide_click(),
        "no_fadein_regression": check_no_fadein_regression(),
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
