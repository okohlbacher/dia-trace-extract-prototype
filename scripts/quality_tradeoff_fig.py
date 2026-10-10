#!/usr/bin/env python3
"""MS2 tolerance trade-off figure from quality_sample.py outputs (<prefix>_sweep.tsv, <prefix>_engine_rules.tsv).
Left: captured true signal vs interference in the peak region (fixed windows, constant / quadratic offset; ±kσ(I)
windows shown hollow, since the truth itself is defined as ±3σ(I) -- their capture is partly by construction).
Right: engine-agnostic shape quality vs half width (co-elution, library cosine, dropouts), the independent judge.
usage: quality_tradeoff_fig.py PREFIX FIG [title]
"""
import sys
import pandas as pd, matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
pre, fig_, title = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "")
sw = pd.read_csv(pre + "_sweep.tsv", sep="\t"); eng = pd.read_csv(pre + "_engine_rules.tsv", sep="\t")
fig, axs = plt.subplots(1, 2, figsize=(11, 4.2))
ax = axs[0]
for model, col in (("const", "#2a9d8f"), ("quad", "#e9c46a"), ("quadI", "#d62828")):
    g = sw[(sw.model == model) & (sw["agg"] == "max")].sort_values("half")
    ax.plot(g.interference_peak * 100, g.capture_peak * 100, "-o", ms=3, c=col, label={"const": "±t, constant offset", "quad": "±t, quadratic(m/z) offset", "quadI": "±t, quadratic(m/z) + intensity shift"}[model])
    if model == "quadI":
        for _, r in g.iterrows():
            if r.half in (2, 3, 4, 5, 6, 8, 10):
                ax.annotate(f"±{r.half:g}", (r.interference_peak * 100, r.capture_peak * 100), fontsize=7,
                            xytext=(3, -9), textcoords="offset points")
g = sw[sw.model == "quad_sigmaI"].sort_values("half")
ax.plot(g.interference_peak * 100, g.capture_peak * 100, "--s", ms=4, mfc="none", c="#6a4c93", label="±kσ(I), same centre (truth-defined; hollow)")
for _, r in g.iterrows():
    ax.annotate(f"{r.half:g}σ", (r.interference_peak * 100, r.capture_peak * 100), fontsize=7, color="#6a4c93",
                xytext=(3, 3), textcoords="offset points")
mk = {"OSW": ("*", "#f4a261"), "DIA-NN": ("P", "#023e8a"), "ours A": ("X", "#2a9d8f")}
for _, r in eng.iterrows():
    m_, c_ = next(v for k, v in mk.items() if r.rule.startswith(k))
    ax.plot(r.interference_peak * 100, r.capture_peak * 100, m_, ms=11, c=c_, mec="k", mew=0.5, label=r.rule)
ax.set_ylim(80, 100.5); ax.set_xlabel("interference in peak region (% of extracted intensity)")
ax.set_ylabel("captured true signal in peak region (%)"); ax.legend(fontsize=7, loc="lower right")
ax.set_title("signal capture vs interference", fontsize=10)
ax = axs[1]
g = sw[(sw.model == "quadI") & (sw["agg"] == "max") & sw.coelution.notna()].sort_values("half")
for k, col, lab in (("coelution", "#d62828", "fragment co-elution (mean pairwise r)"), ("cos_apex", "#023e8a", "library cosine at apex"),
                    ("dropout_frac", "#6a4c93", "dropout fraction in peak"), ("frac_r_vs_diann_gt_0_99", "#888", "frac. traces r>0.99 vs DIA-NN")):
    ax.plot(g.half, g[k], "-o", ms=3, c=col, label=lab)
    for _, r in eng.iterrows():
        if r.rule.startswith("DIA-NN"): ax.axhline(r[k], c=col, lw=0.6, ls=":")
ax.axvline(eng.rule.str.extract(r"±([0-9.]+)\)")[0].astype(float).iloc[0], c="k", lw=0.6)
ax.set_xlabel("half width t (ppm), quadratic + intensity-shift centre, max"); ax.set_ylabel("value")
ax.legend(fontsize=7); ax.set_title("shape quality vs width (dotted: DIA-NN rule; vertical: current tolerance)", fontsize=9)
fig.suptitle(title, fontsize=10); fig.tight_layout()
for e in ("png", "pdf"): fig.savefig(f"{fig_}.{e}", dpi=180)
