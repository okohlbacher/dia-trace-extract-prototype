#!/usr/bin/env python3
"""Distribution of per-trace Pearson r for every engine pair (all traces of the given runs).

Reads the per-trace tables of compare_traces.py (cmpA.per_trace.tsv: ours A vs DIA-NN, ours A vs OpenSWATH,
DIA-NN vs OpenSWATH; cmpB.per_trace.tsv: ours B vs OpenSWATH) and draws, for MS2 and MS1, a histogram of r and
the ECDF of 1 - r on a log axis (the region near r = 1 is where the engines differ).

usage: plot_correlation_distributions.py OUT_PREFIX LABEL RUN_DIR [RUN_DIR ...]
"""
import sys
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

out, label, runs = sys.argv[1], sys.argv[2], sys.argv[3:]
PAIRS = [("A", "odia_vs_diann", "ours A (DIA-NN rules) vs DIA-NN", "#023e8a"),
         ("B", "odia_vs_openswath", "ours B (OSW rules) vs OpenSWATH", "#9c4a00"),
         ("A", "diann_vs_openswath", "DIA-NN vs OpenSWATH", "#6a4c93"),
         ("A", "odia_vs_openswath", "ours A (DIA-NN rules) vs OpenSWATH", "#2a9d8f")]
cols = ["pair", "ms_level", "pearson"]
data = {}
for cfg in ("A", "B"):
    parts = [pd.read_csv(f"{r}/cmp{cfg}.per_trace.tsv", sep="\t", usecols=cols) for r in runs]
    data[cfg] = pd.concat(parts, ignore_index=True)

LO = 0.90
BIN = 0.005
edges = np.round(np.arange(LO, 1.0 + BIN / 2, BIN), 10)   # bins [LO, LO+BIN), ..., [1-BIN, 1.00]
bins_table = []
fig, axs = plt.subplots(2, 2, figsize=(13, 7.6), gridspec_kw={"width_ratios": [1.5, 1]})
rows = []
for i, lvl in enumerate((2, 1)):
    width = 0.9 * BIN / len(PAIRS)                # the group of bars fills 90% of its bin, centred
    for j, (cfg, pair, name, col) in enumerate(PAIRS):
        d = data[cfg]
        r = d[(d.pair == pair) & (d.ms_level == lvl)].pearson.dropna().to_numpy()
        if not len(r): continue
        rows.append({"ms_level": lvl, "pair": name, "traces": len(r), "median": np.median(r),
                     "p10": np.percentile(r, 10), "p25": np.percentile(r, 25), "frac_r>0.99": np.mean(r > 0.99),
                     "frac_r>0.9": np.mean(r > 0.9), "frac_r<0.5": np.mean(r < 0.5)})
        # Histogram: grouped bars (one per pair): fraction of the pair's traces per bin; r < LO not drawn.
        h, _ = np.histogram(r[r >= LO], bins=edges)   # numpy: the last bin includes its right edge (r = 1)
        frac = h / len(r)
        centre = (edges[:-1] + edges[1:]) / 2
        xb = centre + (j - (len(PAIRS) - 1) / 2) * width
        axs[i, 0].bar(xb, frac, width=width, color=col, label=f"{name} (n={len(r):,}; {np.mean(r < LO):.1%} below {LO})")
        for xx, f in (zip(xb, frac) if len(frac) <= 4 else ()):   # value labels only for a few bins
            axs[i, 0].text(xx, f + 0.01, f"{f:.1%}", ha="center", va="bottom", fontsize=6.5, rotation=90)
        for k in range(len(h)):
            bins_table.append({"ms_level": lvl, "pair": name, "bin": f"[{edges[k]:.3f}, {edges[k + 1]:.3f}{']' if k == len(h) - 1 else ')'}",
                               "fraction": frac[k], "traces": int(h[k])})
        # Cumulative: fraction of traces with r <= x, linear axes (value at the left edge = fraction below 0.75).
        x = np.sort(r)
        axs[i, 1].plot(x, np.arange(1, len(x) + 1) / len(x), color=col, lw=1.6, label=name)
    axs[i, 0].set_xlim(LO, 1.0); axs[i, 0].set_ylim(0, None)
    axs[i, 0].set_xticks(edges, minor=True); axs[i, 0].set_xticks(np.round(np.arange(LO, 1.0 + 1e-9, 0.01), 2))
    axs[i, 0].set_xlabel(f"Pearson r per trace (bins of {BIN}, [a, b); minor ticks = bin edges; last bin includes 1.0)")
    axs[i, 0].set_ylabel("fraction of the pair's traces")
    axs[i, 0].set_title(f"MS{lvl}: histogram of r, zoom {LO}-1.00", fontsize=10, loc="left")
    axs[i, 1].set_xlim(LO, 1.0); axs[i, 1].set_ylim(0, 1)
    axs[i, 1].set_xlabel("Pearson r per trace"); axs[i, 1].set_ylabel("fraction of traces with r <= x")
    axs[i, 1].set_title(f"MS{lvl}: cumulative distribution (left edge = fraction below {LO})", fontsize=10, loc="left")
    for ax in axs[i]: ax.grid(alpha=0.25, lw=0.5)
    h, l = axs[i, 0].get_legend_handles_labels()
    axs[i, 1].legend(h, l, fontsize=7, loc="upper left", frameon=True, framealpha=0.9)   # the bars leave no room
fig.suptitle(f"{label}: per-trace Pearson r for every engine pair (runs {', '.join(r.rstrip('/').split('/')[-1] for r in runs)}; all traces)",
             fontsize=11, x=0.01, ha="left")
fig.tight_layout()
for ext in ("png", "pdf"): fig.savefig(f"{out}.{ext}", dpi=170)
summary = pd.DataFrame(rows)
summary.to_csv(f"{out}.tsv", sep="\t", index=False, float_format="%.6g")
bt = pd.DataFrame(bins_table)
bt.to_csv(f"{out}.bins.tsv", sep="\t", index=False, float_format="%.6g")
with pd.option_context("display.width", 200): print(bt.to_string(index=False))
with pd.option_context("display.width", 200, "display.float_format", "{:.4f}".format): print(summary.to_string(index=False))
