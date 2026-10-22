#!/usr/bin/env python3
"""Plot runtime vs threads for DIA-NN, ours A, ours B and OpenSWATH from scripts/bench_threads.sh logs.

usage: plot_thread_scaling.py BENCH_DIR OUT_PREFIX
Panels: wall time (log y), parallel efficiency relative to the smallest thread count, peak RSS.
"""
import re, sys, glob, os
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

bench, out = sys.argv[1], sys.argv[2]
TOOLS = [("diann/diann", "DIA-NN 1.7.12 (search + XIC export)", "#2a9fd6", "o"),
         ("odiaA", "ours A (defaults)", "#023e8a", "s"),
         ("odiaB", "ours B (OpenSWATH rules)", "#9c4a00", "D"),
         ("osw", "OpenSwathWorkflow (extraction + scoring)", "#f4a261", "^")]

def parse(path):
    txt = open(path).read()
    m = re.search(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([0-9:.]+)", txt)
    if not m: return None
    parts = [float(x) for x in m.group(1).split(":")]
    wall = sum(p * 60 ** (len(parts) - 1 - i) for i, p in enumerate(parts))
    cpu = float(re.search(r"User time \(seconds\): ([0-9.]+)", txt).group(1)) + \
          float(re.search(r"System time \(seconds\): ([0-9.]+)", txt).group(1))
    rss = int(re.search(r"Maximum resident set size \(kbytes\): ([0-9]+)", txt).group(1)) / 1048576
    return wall, cpu, rss

rows = []
for d in sorted(glob.glob(os.path.join(bench, "t*"))):
    t = int(os.path.basename(d)[1:])
    for key, name, *_ in TOOLS:
        p = os.path.join(d, key + ".log")
        if os.path.exists(p) and (r := parse(p)):
            rows.append({"tool": name, "threads": t, "wall_s": r[0], "cpu_s": r[1], "peak_rss_gib": r[2]})
df = pd.DataFrame(rows).sort_values(["tool", "threads"])
df.to_csv(out + ".tsv", sep="\t", index=False, float_format="%.3f")

fig, axs = plt.subplots(1, 3, figsize=(15, 4.6))
for key, name, col, mk in TOOLS:
    g = df[df.tool == name]
    if g.empty: continue
    t0, w0 = g.threads.iloc[0], g.wall_s.iloc[0]
    axs[0].plot(g.threads, g.wall_s, marker=mk, color=col, lw=1.8, label=name)
    for t, w in zip(g.threads, g.wall_s):
        axs[0].annotate(f"{w:.0f}" if w >= 10 else f"{w:.1f}", (t, w), textcoords="offset points", xytext=(4, 4), fontsize=7, color=col)
    axs[1].plot(g.threads, (w0 * t0) / (g.wall_s * g.threads), marker=mk, color=col, lw=1.8, label=name)
    axs[2].plot(g.threads, g.peak_rss_gib, marker=mk, color=col, lw=1.8, label=name)
ticks = sorted(df.threads.unique())
for ax in axs:
    ax.set_xscale("log", base=2); ax.set_xticks(ticks); ax.set_xticklabels([str(t) for t in ticks])
    ax.set_xlabel("threads"); ax.grid(alpha=0.3, lw=0.5)
axs[0].set_yscale("log"); axs[0].set_ylabel("wall time (s, log)"); axs[0].set_title("Runtime per run", loc="left", fontsize=10)
axs[1].set_ylabel(f"parallel efficiency vs {ticks[0]} threads"); axs[1].set_ylim(0, 1.15)
axs[1].set_title("Scaling: (T(t0)·t0) / (T(t)·t)", loc="left", fontsize=10)
axs[2].set_ylabel("peak RSS (GiB)"); axs[2].set_ylim(0, None); axs[2].set_title("Peak memory", loc="left", fontsize=10)
axs[1].legend(fontsize=7.5, frameon=False, loc="lower left")
fig.suptitle("Trace extraction runtime vs threads: run 01 (5.8 GB mzML), shared tuned library (82,333 precursors, all fragments), "
             "one run per point, shared node" + (f"; {os.environ['ODIA_VERSION_NOTE']}" if os.environ.get("ODIA_VERSION_NOTE") else ""),
             fontsize=10, x=0.01, ha="left")
fig.tight_layout()
for ext in ("png", "pdf"): fig.savefig(f"{out}.{ext}", dpi=170)
with pd.option_context("display.width", 200): print(df.to_string(index=False))
