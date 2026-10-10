#!/usr/bin/env python3
"""Figure for quality_engines.py: per-engine shape metrics by intensity quintile (medians) and paired differences.
usage: quality_engines_fig.py PREFIX FIG [title]"""
import sys
import numpy as np, pandas as pd, matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
pre, fig_, title = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "")
df = pd.read_csv(pre + "_per_precursor.tsv", sep="\t")
df["Iq"] = pd.qcut(np.log10(df.quantity.clip(lower=1)), 5, labels=[f"Q{i}" for i in range(1, 6)])
ENG = {"DIA-NN": "#8ecae6", "ours A": "#023e8a", "OSW": "#f4a261", "ours B": "#9c4a00", "ours A, MS1 v_tol": "#d62828",
       "ours A, MS1 v_dnn": "#e9c46a"}
MET = [("coelution", "fragment co-elution (median pairwise r) ↑", "median"), ("cos_apex", "library cosine at apex ↑", "median"),
       ("dropout_frac", "dropouts inside peak (mean frac.) ↓", "mean"), ("spike_frac", "isolated spikes (mean frac.) ↓", "mean"),
       ("jagged", "jaggedness (2nd-diff energy) ↓", "median"), ("ms1_coelution", "MS1 M+0 vs fragments r ↑", "median")]
fig, axs = plt.subplots(2, 4, figsize=(15, 6.6))
for ax, (m, lab, how) in zip(axs.flat, MET):
    for e, col in ENG.items():
        if m != "ms1_coelution" and "MS1 v" in e: continue
        g = df[df.engine == e].groupby("Iq", observed=True)[m].agg(how)
        ax.plot(range(1, 6), g.values, "-o", ms=3, c=col, label=e, lw=1.6 if "ours" not in e else 1.0,
                ls="--" if e.startswith("ours") else "-")
    ax.set_xticks(range(1, 6)); ax.set_xticklabels([f"Q{i}" for i in range(1, 6)]); ax.set_title(lab, fontsize=9)
    ax.set_xlabel("DIA-NN precursor quantity quintile", fontsize=8); ax.tick_params(labelsize=7)
axs.flat[0].legend(fontsize=7); axs.flat[5].legend(fontsize=6)
# paired differences
piv = {m: df.pivot(index="precursor_id", columns="engine", values=m) for m, _, _ in MET}
for ax, (x, y, m, xl) in zip(axs.flat[6:], [("DIA-NN", "OSW", "coelution", "co-elution: DIA-NN − OSW (per precursor)"),
                                            ("ours A, MS1 v_tol", "ours A", "ms1_coelution", "MS1 co-elution: narrow MS1 (v_tol) − ours A")]):
    d = (piv[m][x] - piv[m][y]).dropna(); d = d[d != 0]
    ax.hist(d.clip(-1, 1), bins=80, color="#555")
    ax.axvline(0, c="k", lw=0.6)
    ax.set_title(f"{xl}\n(nonzero: {len(d)}; x better {np.mean(d > 0):.0%}, median {d.median():+.3f})", fontsize=8)
    ax.set_yscale("log"); ax.tick_params(labelsize=7)
fig.suptitle(title, fontsize=10); fig.tight_layout()
for e in ("png", "pdf"): fig.savefig(f"{fig_}.{e}", dpi=170)
