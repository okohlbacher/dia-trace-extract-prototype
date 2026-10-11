#!/usr/bin/env python3
"""Summarise scripts/benchmark_refined.sh outputs (RECAL=tuned) across libraries and runs: DIA-NN IDs and mass accuracy,
calibration, wall time and peak RSS of ours A / the full-library run / DIA-NN / OpenSWATH, and the trace agreement of
ours A and P with DIA-NN (median Pearson r, share of traces with r > 0.9, log2 ratio).

usage: summarize_refined.py RESULTS_REFINED_DIR LIB_NAME [LIB_NAME ...]
"""
import os, re, sys

root, libs = sys.argv[1], sys.argv[2:]


def read(path):
    return open(path).read() if os.path.exists(path) else ""


def wall_rss(log):
    t = read(log)
    w = re.search(r"Elapsed \(wall clock\) time .*?: ([0-9:.]+)", t)
    m = re.search(r"Maximum resident set size \(kbytes\): (\d+)", t)
    if not w:
        return "—"
    parts = [float(x) for x in w.group(1).split(":")]
    s = sum(p * 60 ** i for i, p in enumerate(reversed(parts)))
    return f"{s:.1f} s / {int(m.group(1)) / 1048576:.2f} GiB" if m else f"{s:.1f} s"


def cmp_rows(path):
    """{(pair, level): (median r, frac r>0.9, log2 ratio)} from the last table of a compare_traces.py output."""
    rows = {}
    for line in read(path).splitlines():
        f = line.split()
        if len(f) == 8 and f[1] in ("1", "2"):
            try:
                rows[(f[0], int(f[1]))] = (float(f[3]), float(f[4]), float(f[6]))
            except ValueError:
                pass
    return rows


print("| library | run | DIA-NN IDs (acc.) | MS2 / MS1 offset, accuracy (ours) | ours A (IDs) | ours, full library | DIA-NN | OpenSWATH "
      "| A vs DIA-NN MS1 M+0: r (log2) | A vs DIA-NN MS2 r>0.9 | P vs DIA-NN MS1 M+0 | P vs DIA-NN MS2 r>0.9 |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for lib in libs:
    for run in ("01", "02", "03"):
        d = os.path.join(root, lib, "run" + run)
        dl = read(os.path.join(d, "diann", "diann.log"))
        ids = re.findall(r"Number of IDs at 0.01 FDR: (\d+)", dl)
        acc = re.findall(r"Optimised mass accuracy: ([0-9.]+)", dl)
        al = read(os.path.join(d, "odiaA.log"))
        cal = re.search(r"MS2 offset ([-0-9.]+) ppm.*?MS1 offset ([-0-9.]+) ppm.*?accuracy MS2 \+-([0-9.]+) / MS1 \+-([0-9.]+)", al)
        calib = f"{float(cal.group(1)):.2f} / {float(cal.group(2)):.2f} ppm, ±{float(cal.group(3)):.1f} / ±{float(cal.group(4)):.2f}" if cal else "—"
        a, p = cmp_rows(os.path.join(d, "cmpA.out")), cmp_rows(os.path.join(d, "cmpP.out"))
        f = lambda r, k, i: f"{r[k][i]:.3f}" if k in r else "—"
        pct = lambda r, k: f"{100 * r[k][1]:.1f}%" if k in r else "—"
        ms1 = lambda r: f"{f(r, ('odia_vs_diann', 1), 0)} ({float(f(r, ('odia_vs_diann', 1), 2)):+.2f})" if ('odia_vs_diann', 1) in r else "—"
        print(f"| {lib.split('_astral')[0]} | {run} | {ids[-1] if ids else '—'} (±{acc[-1] if acc else '?'}) | {calib} | "
              f"{wall_rss(os.path.join(d, 'odiaA.log'))} | {wall_rss(os.path.join(d, 'odia_full.log'))} | "
              f"{wall_rss(os.path.join(d, 'diann', 'diann.log'))} | {wall_rss(os.path.join(d, 'osw.log'))} | "
              f"{ms1(a)} | {pct(a, ('odia_vs_diann', 2))} | {ms1(p)} | {pct(p, ('odia_vs_diann', 2))} |")
