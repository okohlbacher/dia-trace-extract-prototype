#!/usr/bin/env python3
"""Runtime vs threads on the cluster node and on the laptop, from docs/figures/thread_scaling.tsv (cluster,
plot_thread_scaling.py) and docs/figures/laptop_m4max_summary.tsv (bench_macos.sh summary: run, wall_s, peak_rss_gib).
usage: plot_machine_scaling.py OUT_PREFIX"""
import sys, re
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
out = sys.argv[1]
cl = pd.read_csv("docs/figures/thread_scaling.tsv", sep="\t")
lap = pd.read_csv("docs/figures/laptop_m4max_summary.tsv", sep="\t")
lap["tool"] = lap.run.str.extract(r"^(odiaA|odiaB|full|osw)")[0]
lap["threads"] = lap.run.str.extract(r"_t(\d+)")[0].astype(float)
lap = lap.groupby(["tool", "threads"], as_index=False).agg(wall_s=("wall_s", "mean"), peak_rss_gib=("peak_rss_gib", "mean"))
series = [("cluster, ours A", cl[cl.tool.str.startswith("ours A")], "#023e8a", "s", "-"),
          ("cluster, DIA-NN 1.7.12", cl[cl.tool.str.startswith("DIA-NN")], "#2a9fd6", "o", "-"),
          ("cluster, OpenSwathWorkflow", cl[cl.tool.str.startswith("OpenSwath")], "#f4a261", "^", "-"),
          ("MacBook Pro M4 Max, ours A", lap[lap.tool == "odiaA"], "#d9480f", "s", "--"),
          ("MacBook Pro M4 Max, OpenSwathWorkflow", lap[lap.tool == "osw"], "#f4a261", "^", "--")]
fig, axs = plt.subplots(1, 2, figsize=(10.5, 4.4))
for name, g, c, mk, ls in series:
    if g.empty: continue
    g = g.sort_values("threads")
    axs[0].plot(g.threads, g.wall_s, marker=mk, ls=ls, color=c, lw=1.6, label=name)
    for t, w in zip(g.threads, g.wall_s):
        axs[0].annotate(f"{w:.0f}" if w >= 10 else f"{w:.1f}", (t, w), textcoords="offset points", xytext=(4, 4), fontsize=7, color=c)
    axs[1].plot(g.threads, g.peak_rss_gib, marker=mk, ls=ls, color=c, lw=1.6, label=name)
for ax in axs:
    ax.set_xscale("log", base=2); ax.set_xticks([1, 2, 4, 8, 16, 32, 64]); ax.set_xticklabels(["1", "2", "4", "8", "16", "32", "64"])
    ax.set_xlabel("threads"); ax.grid(alpha=0.3, lw=0.5)
axs[0].set_yscale("log"); axs[0].set_ylabel("wall time (s, log)"); axs[0].set_title("Runtime per run", loc="left", fontsize=10)
axs[1].set_ylabel("peak RSS (GiB)"); axs[1].set_ylim(0, None); axs[1].set_title("Peak memory", loc="left", fontsize=10)
axs[0].legend(fontsize=7.5, frameon=False, loc="lower left")
fig.suptitle("Run 01, shared library (82,333 precursors, all fragments): 2 x EPYC 9654 node (solid) vs MacBook Pro M4 Max (dashed)\n"
             "V0.2.7 on both machines (laptop on mains power, mean of two repeats; node at a load of 0.12-0.25 per hardware thread)", fontsize=9, x=0.01, ha="left")
fig.tight_layout()
for ext in ("png", "pdf"): fig.savefig(f"{out}.{ext}", dpi=170)
print(lap.to_string(index=False))
