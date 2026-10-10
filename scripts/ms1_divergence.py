#!/usr/bin/env python3
"""MS1 (M+0) divergence between DIATraceExtractor and DIA-NN 1.7.12 --xic traces.

Compares one or more extractor outputs (benchmark A and the variants of scripts/ms1_variants.sh) with a
DIA-NN XIC Parquet, exactly like compare_traces.py (DIA-NN interpolated at the extractor's global cycle
times on the common span), plus point-level agreement:
  exact   = fraction of grid points with |ours - dnn| <= 1e-4 * max(|ours|, |dnn|, 1)
  higher  = fraction of points where ours > dnn (beyond that tolerance), lower likewise
  dnn0    = fraction of points where DIA-NN is 0 and ours > 0 (a centroid outside DIA-NN's MS1 window)
  material= fraction of points differing by > 1% (above float32-RT interpolation noise)
Writes <out>.per_trace.tsv (all variants), <out>.summary.tsv and, for the first variant (benchmark A),
stratified medians (<out>.strata.tsv) by DIA-NN intensity, precursor m/z, charge, RT and DIA-NN q-value.

usage: ms1_divergence.py --diann diann_xic.parquet --axes odiaA.axes.parquet --out results/analysis/x
                         --variant A=odiaA.parquet [--variant dnn=v_dnn.parquet ...]
"""
import argparse
import numpy as np, pandas as pd, pyarrow.parquet as pq, pyarrow.compute as pc

ap = argparse.ArgumentParser()
ap.add_argument("--diann", required=True); ap.add_argument("--axes", required=True)
ap.add_argument("--variant", action="append", required=True); ap.add_argument("--out", required=True)
ap.add_argument("--min_points", type=int, default=5)
a = ap.parse_args()

UNIMOD = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNIMOD.items(): s = s.replace(k, v)
    return s

axes = pq.read_table(a.axes).to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()

d = pq.read_table(a.diann, filters=[("ms_level", "=", 1)]).to_pandas()
dnn = {}
for pid, q, mz, rt, y in zip(d.precursor_id, d.qvalue, d.extraction_mz, d.rt, d.intensity):
    rt, y = np.asarray(rt, float), np.asarray(y, float); keep = rt > 0
    dnn[from_unimod(pid)] = (rt[keep], y[keep], q, mz)

def load(path):
    t = pq.read_table(path, columns=["precursor_id", "precursor_mz", "precursor_charge", "rt_center", "ms_level",
                                     "annotation", "cycle_begin", "intensity"])
    t = t.filter(pc.and_(pc.equal(t["ms_level"], 1), pc.equal(t["annotation"], "M+0")))
    return t.to_pandas()

rows = []
for spec in a.variant:
    name, path = spec.split("=", 1)
    o = load(path)
    for pid, pmz, z, rtc, cb, blob in zip(o.precursor_id, o.precursor_mz, o.precursor_charge, o.rt_center,
                                          o.cycle_begin, o.intensity):
        if pid not in dnn: continue
        y = np.frombuffer(blob, "<f4").astype(float); t = cycle_rt[cb:cb + len(y)]
        ty, vy, q, dmz = dnn[pid]
        if len(ty) < 2 or len(t) < 2: continue
        lo, hi = max(t.min(), ty.min()), min(t.max(), ty.max())
        m = (t >= lo) & (t <= hi)
        if m.sum() < a.min_points: continue
        g, ix = t[m], y[m]; iy = np.interp(g, ty, vy)
        sx, sy = ix.sum(), iy.sum()
        r = np.corrcoef(ix, iy)[0, 1] if ix.std() > 0 and iy.std() > 0 else np.nan
        ok = sx > 0 and sy > 0
        tol = 1e-4 * np.maximum(np.maximum(np.abs(ix), np.abs(iy)), 1.0)
        diff = ix - iy
        rows.append((name, pid, pmz, int(z), rtc, q, dmz, r,
                     np.abs(ix / sx - iy / sy).sum() if ok else np.nan,
                     np.log2(sx / sy) if ok else np.nan,
                     g[ix.argmax()] - g[iy.argmax()] if ok else np.nan, int(m.sum()), sy / m.sum(),
                     float((np.abs(diff) <= tol).mean()), float((diff > tol).mean()), float((diff < -tol).mean()),
                     float(((iy == 0) & (ix > 0)).mean()),
                     float((np.abs(diff) > 0.01 * np.maximum(np.abs(ix), np.abs(iy))).mean())))

res = pd.DataFrame(rows, columns=["variant", "precursor_id", "precursor_mz", "charge", "rt_center", "dnn_qvalue",
                                  "dnn_extraction_mz", "pearson", "l1_norm", "log2_sum_ratio", "apex_dt_s", "points",
                                  "dnn_mean_intensity", "frac_exact", "frac_higher", "frac_lower", "frac_dnn0",
                                  "frac_material"])
res.to_csv(a.out + ".per_trace.tsv", sep="\t", index=False)

def summarise(g):
    return pd.Series({"traces": len(g), "pearson_median": g.pearson.median(), "frac_r_gt_0_99": (g.pearson > 0.99).mean(),
                      "frac_identical_traces": (g.frac_exact == 1).mean(), "l1_median": g.l1_norm.median(),
                      "log2_ratio_median": g.log2_sum_ratio.median(), "apex_abs_dt_median": g.apex_dt_s.abs().median(),
                      "apex_dt_nonzero": (g.apex_dt_s.abs() > 1e-6).mean(),
                      "points_exact": g.frac_exact.mean(), "points_ours_higher": g.frac_higher.mean(),
                      "points_ours_lower": g.frac_lower.mean(), "points_dnn0_ours_pos": g.frac_dnn0.mean(),
                      "points_diff_gt_1pct": g.frac_material.mean(), "traces_all_points_within_1pct": (g.frac_material == 0).mean()})
order = [s.split("=", 1)[0] for s in a.variant]
summ = res.groupby("variant").apply(summarise).reindex(order)
summ.to_csv(a.out + ".summary.tsv", sep="\t")
with pd.option_context("display.width", 250, "display.max_columns", 30, "display.float_format", "{:.4f}".format):
    print(summ)

# stratification of the first variant
A = res[res.variant == order[0]].copy()
A["dnn_intensity_decile"] = pd.qcut(np.log10(A.dnn_mean_intensity.clip(lower=1)), 10, labels=False, duplicates="drop")
A["mz_bin"] = pd.cut(A.precursor_mz, [0, 400, 500, 600, 700, 800, 1000, 2000])
A["rt_bin"] = pd.cut(A.rt_center, 8)
A["q_bin"] = pd.cut(A.dnn_qvalue, [-1, 1e-4, 1e-3, 5e-3, 1])
out = []
for col in ["dnn_intensity_decile", "mz_bin", "charge", "rt_bin", "q_bin"]:
    s = A.groupby(col, observed=True).apply(summarise)[["traces", "pearson_median", "l1_median", "log2_ratio_median",
                                                       "frac_identical_traces", "points_dnn0_ours_pos"]]
    s.insert(0, "stratum", col); s.index = s.index.astype(str); out.append(s)
strata = pd.concat(out)
strata.to_csv(a.out + ".strata.tsv", sep="\t")
with pd.option_context("display.width", 250, "display.max_columns", 30, "display.float_format", "{:.4f}".format):
    print(strata)

# residual of the DIA-NN emulation by m/z (float32 m/z ulp doubles at 512 Th: boundary jitter of DIA-NN's float window)
if "v_dnn" in order:
    V = res[res.variant == "v_dnn"].copy()
    V["mz_bin"] = pd.cut(V.precursor_mz, [0, 400, 450, 500, 512, 550, 600, 700, 800, 1024, 2000])
    s = V.groupby("mz_bin", observed=True).apply(summarise)[["traces", "pearson_median", "traces_all_points_within_1pct",
                                                          "points_diff_gt_1pct", "points_dnn0_ours_pos"]]
    s.to_csv(a.out + ".v_dnn_by_mz.tsv", sep="\t")
    with pd.option_context("display.width", 250, "display.float_format", "{:.4f}".format): print(s)
