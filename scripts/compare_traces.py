#!/usr/bin/env python3
"""Three-way trace comparison: DIATraceExtractor vs DIA-NN 1.7.12 (--xic) vs OpenSWATH (.xic exploded).

Traces are joined on (precursor_id, ms_level, annotation); DIA-NN UniMod names are mapped to the
library's names. Each pair is compared on the common RT span after interpolating onto the extractor's
sampling times of the first tool of each pair (for the extractor: the true acquisition times of the
window spectra for MS2, the global cycle axis for MS1). Metrics: Pearson r, apex RT difference (s), log2 intensity ratio of sums, and L1
distance of sum-normalised traces (baseline-sensitive; correlation alone hides baseline damage).

usage: compare_traces.py --odia traces.parquet --axes traces.axes.parquet
                         [--diann diann_xic.parquet] [--openswath osw_explode.tsv] --out summary.tsv
"""
import argparse
import numpy as np, pandas as pd, pyarrow.parquet as pq

ap = argparse.ArgumentParser()
ap.add_argument("--odia", required=True); ap.add_argument("--axes", required=True)
ap.add_argument("--diann"); ap.add_argument("--openswath"); ap.add_argument("--out", required=True)
ap.add_argument("--min_points", type=int, default=5)
a = ap.parse_args()

UNIMOD = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNIMOD.items(): s = s.replace(k, v)
    return s

axes = pq.read_table(a.axes).to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
# Exact acquisition time of window w in cycle c (NaN where the window was not acquired). MS2 points are
# compared at these times (their true sampling times); MS1 points at the MS1 scan times (or on the global axis
# for traces written with -ms1_axis cycles).
n_cycles = len(cycle_rt)
acq = {}
for w, g in axes[axes.lane >= 0].groupby("lane"):
    t = np.full(n_cycles, np.nan); t[g.cycle.to_numpy()] = g.rt.to_numpy(); acq[w] = t

def ms1_axis_rt(odia_path, axes, cycle_rt):
    """Times of MS1 rows: the MS1 scans (axes lane -2) when the traces say odia.ms1_axis = ms1_scans, else the cycle axis."""
    import glob as _glob, os as _os, pyarrow.parquet as _pq
    first = sorted(_glob.glob(_os.path.join(odia_path, "*.parquet")))[0] if _os.path.isdir(odia_path) else odia_path
    md = _pq.read_schema(first).metadata or {}   # (pyarrow.dataset drops the footer metadata)
    if md.get(b"odia.ms1_axis") == b"ms1_scans":
        return axes[axes.lane == -2].sort_values("cycle").rt.to_numpy()
    return cycle_rt
ms1_rt = ms1_axis_rt(a.odia, axes, cycle_rt)

o = pq.read_table(a.odia, columns=["precursor_id", "ms_level", "window", "annotation", "cycle_begin", "intensity"]).to_pandas()
traces = {}
for pid, lvl, win, ann, cb, blob in o.itertuples(index=False):
    y = np.frombuffer(blob, "<f4").astype(np.float64)
    t = acq[win][cb:cb + len(y)] if lvl == 2 else ms1_rt[cb:cb + len(y)]
    ok = ~np.isnan(t)
    traces[(pid, lvl, ann)] = {"odia": (t[ok], y[ok])}

if a.diann:
    d = pq.read_table(a.diann).to_pandas()
    for r in d.itertuples(index=False):
        k = (from_unimod(r.precursor_id), r.ms_level, r.annotation)
        rt, y = np.asarray(r.rt, float), np.asarray(r.intensity, float)
        keep = rt > 0                                    # DIA-NN pads unfilled XIC slots with RT 0
        if k in traces: traces[k]["diann"] = (rt[keep], y[keep])

if a.openswath:  # output of OpenSwathXicToTraces (Parquet, float32 blobs)
    s = pq.read_table(a.openswath).to_pandas()
    for pid, lvl, an, rtb, yb in s.itertuples(index=False):
        k = (pid, lvl, an)
        if k in traces:
            rt, y = np.frombuffer(rtb, "<f4").astype(float), np.frombuffer(yb, "<f4").astype(float)
            o_ = np.argsort(rt, kind="stable"); traces[k]["openswath"] = (rt[o_], y[o_])

def compare(x, y):
    (tx, vx), (ty, vy) = x, y
    if len(tx) < 2 or len(ty) < 2: return None
    lo, hi = max(tx.min(), ty.min()), min(tx.max(), ty.max())
    grid = tx[(tx >= lo) & (tx <= hi)]   # the first tool's own sampling times
    if len(grid) < a.min_points: return None
    ix, iy = np.interp(grid, tx, vx), np.interp(grid, ty, vy)
    sx, sy = ix.sum(), iy.sum()
    r = np.corrcoef(ix, iy)[0, 1] if ix.std() > 0 and iy.std() > 0 else np.nan
    l1 = np.abs(ix / sx - iy / sy).sum() if sx > 0 and sy > 0 else np.nan
    ratio = np.log2(sx / sy) if sx > 0 and sy > 0 else np.nan
    apex = grid[ix.argmax()] - grid[iy.argmax()] if sx > 0 and sy > 0 else np.nan
    return r, l1, ratio, apex, len(grid)

rows = []
pairs = [("odia", "diann"), ("odia", "openswath"), ("diann", "openswath")]
for k, t in traces.items():
    for p, q in pairs:
        if p in t and q in t:
            c = compare(t[p], t[q])
            if c: rows.append((p + "_vs_" + q, k[0], k[1], k[2]) + c)
res = pd.DataFrame(rows, columns=["pair", "precursor_id", "ms_level", "annotation", "pearson", "l1_norm",
                                  "log2_sum_ratio", "apex_dt_s", "points"])
res.to_csv(a.out.replace(".tsv", ".per_trace.tsv"), sep="\t", index=False)
summ = res.groupby(["pair", "ms_level"]).agg(
    traces=("pearson", "size"), pearson_median=("pearson", "median"),
    frac_r_gt_0_9=("pearson", lambda v: float((v > 0.9).mean())),
    l1_median=("l1_norm", "median"), log2_ratio_median=("log2_sum_ratio", "median"),
    apex_abs_dt_median=("apex_dt_s", lambda v: float(np.nanmedian(np.abs(v))))).reset_index()
summ.to_csv(a.out, sep="\t", index=False)
print(summ.to_string(index=False))
