import json
import sys
from pathlib import Path

import numpy as np

RESULTS_PATH = Path(__file__).resolve().parent / "sweep_results.jsonl"

FEATURE_KEYS = ["decayTimeMs", "transientRatio", "spectralCentroidHz", "lowFreqEnergyRatio", "inharmonicity"]

TARGET_DIRECTION = {
    "Percussive":  {"decayTimeMs": -1, "transientRatio": +1, "spectralCentroidHz":  0, "lowFreqEnergyRatio":  0, "inharmonicity": -1},
    "MetalGlass":  {"decayTimeMs": +1, "transientRatio": -1, "spectralCentroidHz": +1, "lowFreqEnergyRatio": -1, "inharmonicity": +1},
    "Strings":     {"decayTimeMs": +1, "transientRatio": -1, "spectralCentroidHz":  0, "lowFreqEnergyRatio":  0, "inharmonicity": -1},
    "DanceBass":   {"decayTimeMs": +1, "transientRatio": -1, "spectralCentroidHz": -1, "lowFreqEnergyRatio": +1, "inharmonicity": -1},
}


def load_rows():
    rows = []
    with RESULTS_PATH.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            if "features" in row:
                rows.append(row)
    return rows


def zscore_table(rows):
    mat = np.array([[r["features"][k] for k in FEATURE_KEYS] for r in rows], dtype=np.float64)
    mean = mat.mean(axis=0)
    std = mat.std(axis=0)
    std[std < 1e-9] = 1.0
    z = (mat - mean) / std
    return z, mean, std


def score(z_row, target):
    s = 0.0
    for i, k in enumerate(FEATURE_KEYS):
        d = target[k]
        if d == 0:
            s -= abs(z_row[i]) * 0.3
        else:
            s += d * z_row[i]
    return s


def main():
    rows = load_rows()
    if not rows:
        print("no successful rows found", file=sys.stderr)
        return 1
    z, mean, std = zscore_table(rows)

    print(f"{len(rows)} successful renders")
    print("feature ranges:")
    for i, k in enumerate(FEATURE_KEYS):
        vals = z[:, i] * std[i] + mean[i]
        print(f"  {k:22s} min={vals.min():10.3f} max={vals.max():10.3f} mean={vals.mean():10.3f}")
    print()

    chosen = {}
    for name, target in TARGET_DIRECTION.items():
        scores = np.array([score(z[i], target) for i in range(len(rows))])
        order = np.argsort(-scores)
        top = order[:5]
        print(f"=== {name} top 5 ===")
        for idx in top:
            r = rows[idx]
            print(f"  score={scores[idx]:7.3f} params={r['params']} feats={r['features']}")
        chosen[name] = rows[top[0]]
        print()

    print("=== FINAL PICKS (position, decay, damping, stretch) ===")
    for name, r in chosen.items():
        p = r["params"]
        print(f"{name}: {{ {p['position']:.3f}f, {p['decay']:.3f}f, {p['damping']:.3f}f, {p['stretch']:.3f}f }},")

    out_path = Path(__file__).resolve().parent / "chosen_patches.json"
    with out_path.open("w") as f:
        json.dump(chosen, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
