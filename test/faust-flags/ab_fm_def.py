#!/usr/bin/env python3
import argparse
import itertools
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

SAMPLE_RATE = 48000
BLOCK_SIZE = 64
SHIPPED_FLAGS = ["-vec", "-fun", "-dfs", "-vs", "32", "-ct", "0"]
CANDIDATE_FLAGS = SHIPPED_FLAGS + ["-fm", "def"]

REPO_ROOT = Path(__file__).resolve().parents[2]
FAUST_DIR = REPO_ROOT / "effects" / "home" / "faust"
WORKER = Path(__file__).with_name("_render_worker.py")

FILTERS_HPCUT_SWEEP = [0.0, 0.15, 0.5, 0.85, 1.0]
FILTERS_LPCUT_SWEEP = [0.0, 0.15, 0.5, 0.85, 1.0]
FILTERS_LPRES_SWEEP = [0.0, 0.5, 1.0]
COMPRESSOR_AMOUNT_SWEEP = [0.0, 0.25, 0.5, 0.75, 1.0]
PITCH_SEMITONE_SWEEP = [-12.0, -7.0, -1.0, 0.0, 1.0, 7.0, 12.0]


def build_test_signal(duration_s: float) -> np.ndarray:
    n = int(duration_s * SAMPLE_RATE)
    t = np.arange(n) / SAMPLE_RATE
    sweep = np.sin(2 * np.pi * (20.0 + (20000.0 - 20.0) * t / duration_s) * t)
    noise = np.random.default_rng(0).uniform(-1.0, 1.0, n)
    dc_burst = np.full(SAMPLE_RATE // 10, 0.7)
    silence = np.zeros(SAMPLE_RATE // 10)
    signal = np.concatenate([silence, sweep, noise, dc_burst])
    return signal.astype(np.float64).reshape(1, -1)


class RenderOutcome:
    def __init__(self, audio=None, crashed_signal=None, error=None):
        self.audio = audio
        self.crashed_signal = crashed_signal
        self.error = error

    @property
    def ok(self):
        return self.audio is not None


def render_dsp_isolated(code: str, flags: list, input_audio: np.ndarray, work_dir: Path) -> RenderOutcome:
    code_path = work_dir / "dsp_code.txt"
    input_path = work_dir / "input.npy"
    output_path = work_dir / "output.npy"
    code_path.write_text(code)
    np.save(input_path, input_audio)

    proc = subprocess.run(
        [
            sys.executable,
            str(WORKER),
            str(code_path),
            json.dumps(flags),
            str(input_path),
            str(output_path),
            str(SAMPLE_RATE),
            str(BLOCK_SIZE),
        ],
        capture_output=True,
        text=True,
    )

    if proc.returncode < 0:
        return RenderOutcome(crashed_signal=-proc.returncode)
    if proc.returncode != 0:
        return RenderOutcome(error=proc.stderr.strip()[-2000:])

    output_npy = output_path.with_suffix(".npy")
    audio = np.load(output_npy)
    return RenderOutcome(audio=audio)


def compare_flag_sets(case_name: str, code: str, input_audio: np.ndarray, work_dir: Path) -> dict:
    baseline = render_dsp_isolated(code, SHIPPED_FLAGS, input_audio, work_dir)
    candidate = render_dsp_isolated(code, CANDIDATE_FLAGS, input_audio, work_dir)

    result = {
        "name": case_name,
        "baseline_ok": baseline.ok,
        "candidate_ok": candidate.ok,
        "candidate_crashed_signal": candidate.crashed_signal,
        "candidate_error": candidate.error,
    }

    if baseline.ok and candidate.ok:
        n = min(baseline.audio.shape[1], candidate.audio.shape[1])
        diff = np.abs(baseline.audio[:, :n] - candidate.audio[:, :n])
        result["max_abs_diff"] = float(diff.max())
        result["mean_abs_diff"] = float(diff.mean())
        result["bit_identical"] = bool(diff.max() == 0.0)
        result["samples"] = n
    else:
        result["max_abs_diff"] = None
        result["mean_abs_diff"] = None
        result["bit_identical"] = None
        result["samples"] = None

    return result


def substitute_once(src: str, old: str, new: str) -> str:
    count = src.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly 1 occurrence of {old!r}, found {count}")
    return src.replace(old, new)


def filters_dsp_variant(hpcut: float, lpcut: float, lpres: float) -> str:
    src = (FAUST_DIR / "filters.dsp").read_text()
    src = substitute_once(src, "HPCUT = 0.0;", f'HPCUT = hslider("HPCUT", {hpcut}, 0.0, 1.0, 0.001);')
    src = substitute_once(src, "LPCUT = 1.0;", f'LPCUT = hslider("LPCUT", {lpcut}, 0.0, 1.0, 0.001);')
    src = substitute_once(src, "LPRES = 0.0;", f'LPRES = hslider("LPRES", {lpres}, 0.0, 1.0, 0.001);')
    return src


def compressor_dsp_variant(amount: float) -> str:
    src = (FAUST_DIR / "compressor.dsp").read_text()
    return substitute_once(
        src,
        'COMPRESSAMT = hslider("COMPRESSAMT", 0.0, 0.0, 1.0, 0.01);',
        f'COMPRESSAMT = hslider("COMPRESSAMT", {amount}, 0.0, 1.0, 0.01);',
    )


def pitch_scale_formula(semis: float) -> str:
    return (
        'import("stdfaust.lib");\n'
        f'SEMIS = hslider("SEMIS", {semis}, -12.0, 12.0, 0.001);\n'
        "scale = pow(2.0, SEMIS / 12.0);\n"
        "process = _ * scale;\n"
    )


def run_all_cases(input_audio: np.ndarray, work_dir: Path) -> list:
    results = []

    for hpcut, lpcut, lpres in itertools.product(
        FILTERS_HPCUT_SWEEP, FILTERS_LPCUT_SWEEP, FILTERS_LPRES_SWEEP
    ):
        code = filters_dsp_variant(hpcut, lpcut, lpres)
        name = f"filters.dsp HPCUT={hpcut} LPCUT={lpcut} LPRES={lpres}"
        results.append(compare_flag_sets(name, code, input_audio, work_dir))
        print(f"  ran: {name}", flush=True)

    for amount in COMPRESSOR_AMOUNT_SWEEP:
        code = compressor_dsp_variant(amount)
        name = f"compressor.dsp COMPRESSAMT={amount}"
        results.append(compare_flag_sets(name, code, input_audio, work_dir))
        print(f"  ran: {name}", flush=True)

    for semis in PITCH_SEMITONE_SWEEP:
        code = pitch_scale_formula(semis)
        name = f"pitch.dsp pow(2,SEMIS/12) SEMIS={semis}"
        results.append(compare_flag_sets(name, code, input_audio, work_dir))
        print(f"  ran: {name}", flush=True)

    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=2.0)
    parser.add_argument("--out", type=Path, default=Path(__file__).with_name("results.json"))
    args = parser.parse_args()

    input_audio = build_test_signal(args.duration)

    with tempfile.TemporaryDirectory(prefix="ab_fm_def_") as tmp:
        results = run_all_cases(input_audio, Path(tmp))

    args.out.write_text(json.dumps(results, indent=2))

    crashed = [r for r in results if r["candidate_crashed_signal"]]
    diffs = [r for r in results if r["bit_identical"] is False]
    identical = [r for r in results if r["bit_identical"] is True]

    print(f"\n{len(results)} cases run.")
    print(f"  {len(crashed)} crashed the candidate (-fm def) render.")
    print(f"  {len(diffs)} produced a numeric difference (not bit-identical).")
    print(f"  {len(identical)} were bit-identical.")
    print()
    for r in results:
        if r["candidate_crashed_signal"]:
            tag = f"CRASHED (signal {r['candidate_crashed_signal']})"
        elif not r["candidate_ok"]:
            tag = f"CANDIDATE ERROR: {r['candidate_error']}"
        elif r["bit_identical"]:
            tag = "IDENTICAL"
        else:
            tag = f"DIFF max={r['max_abs_diff']:.3e}"
        print(f"  {r['name']:55s} {tag}")

    print(f"\nFull results written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
