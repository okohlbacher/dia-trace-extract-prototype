#!/usr/bin/env python3
"""Plot scripts/tolerance_screen.py results for several instruments on one figure.

usage: plot_tolerance_screen.py OUT_PREFIX LABEL=TSV[:diann_k] [LABEL=TSV[:diann_k] ...]
  diann_k: DIA-NN's optimised accuracy in multiples of the same sigma (drawn as a marker).
"""
import sys
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

out = sys.argv[1]
runs = []
for arg in sys.argv[2:]:
    label, rest = arg.split("=", 1)
    path, _, dk = rest.partition(":")
    runs.append((label, pd.read_csv(path, sep="\t"), float(dk) if dk else None))

panels = [("coelution", "fragment co-elution (median mean pairwise r;\ndropped fragment = 0)"),
          ("dropouts", "fragments absent at the apex"),
          ("offpeak", "signal outside ±3 windows of the apex\n(interference / noise admitted)"),
          ("cosine", "library cosine at the apex (median)"),
          ("ms1_coelution", "MS1 M+0 vs summed fragments (median r)")]
colors = ["#1f6feb", "#d9480f", "#2a9d8f"]
fig, axs = plt.subplots(1, len(panels), figsize=(4.0 * len(panels), 4.2))
for ax, (col, title) in zip(axs, panels):
    for (label, df, dk), c in zip(runs, colors):
        y = df[col] * (100 if col in ("dropouts", "offpeak") else 1)
        ax.plot(df.k, y, marker="o", ms=4, lw=1.6, color=c, label=label)
        best = df.k.iloc[int(np.nanargmin(y) if col in ("dropouts", "offpeak") else np.nanargmax(y))]
        ax.axvline(6, color="#9aa4b2", lw=0.8, ls=":")
        if dk: ax.axvline(dk, color=c, lw=0.8, ls="--", alpha=0.7)
        for k, v, ppm in zip(df.k, y, df.ms2_ppm):
            if k in (1, 2, 4, 6, 8, 12): ax.annotate(f"{ppm:.0f}" if ppm >= 10 else f"{ppm:.1f}", (k, v), textcoords="offset points", xytext=(0, 6), fontsize=6.5, color=c, ha="center")
    ax.set_xscale("log", base=2); ax.set_xticks([1, 1.5, 2, 3, 4, 6, 8, 12]); ax.set_xticklabels(["1", "1.5", "2", "3", "4", "6", "8", "12"])
    ax.set_xlabel("tolerance (multiples of the measured width σ)"); ax.set_title(title, fontsize=9, loc="left"); ax.grid(alpha=0.3, lw=0.5)
    if col in ("dropouts", "offpeak"): ax.set_ylabel("%")
axs[0].legend(fontsize=8, frameon=False, loc="lower right")
fig.suptitle("Trace quality vs mass tolerance (MS2 and MS1 at the same k; numbers = MS2 half width in ppm; dotted = 6σ, dashed = DIA-NN's accuracy)",
             fontsize=9.5, x=0.01, ha="left")
fig.tight_layout()
for ext in ("png", "pdf"): fig.savefig(f"{out}.{ext}", dpi=170)
for label, df, dk in runs:
    print(f"== {label}" + (f" (DIA-NN at {dk:.1f} sigma)" if dk else ""))
    print(df[["k", "ms2_ppm", "ms1_ppm", "n", "coelution", "dropouts", "cosine", "offpeak", "ms1_coelution"]].round(4).to_string(index=False))
