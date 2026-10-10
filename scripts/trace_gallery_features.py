#!/usr/bin/env python3
"""Per-precursor divergence features for the trace gallery (scripts/trace_gallery.py).

For every precursor present in all three engines (ours A/B, DIA-NN 1.7.12 --xic, OpenSWATH), takes the
displayed traces -- the top library fragment (MS2) and the MS1 monoisotope (M+0) -- and collects the
per-trace metrics written by compare_traces.py (cmpA/cmpB .per_trace.tsv) plus intensity and coverage:
  r2_do, l1_2_do, lr2_do, dt2_do, pts2_do   DIA-NN vs OpenSWATH, top fragment
  r2_ad, l1_2_ad / r2_bo                    ours A vs DIA-NN / ours B vs OpenSWATH, top fragment
  r1_ad, l1_1_ad, lr1_ad                    ours A vs DIA-NN, MS1 M+0
  r1_do, l1_1_do, lr1_do, dt1_do            DIA-NN vs OpenSWATH, MS1 M+0
  r1_bo                                     ours B vs OpenSWATH, MS1 M+0
  int2, int1                                log10 apex intensity of DIA-NN's top fragment / MS1 trace
  cov2                                      DIA-NN/OpenSWATH common points / ours-A points (top fragment)
usage: trace_gallery_features.py RUN_DIR OUT.parquet
"""
import sys
import numpy as np, pandas as pd, pyarrow.parquet as pq

R, out = sys.argv[1], sys.argv[2]
UNIMOD = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNIMOD.items(): s = s.replace(k, v)
    return s

o = pq.read_table(R + "/odiaA.parquet", columns=["precursor_id", "precursor_mz", "precursor_charge", "rt_center",
                                                 "ms_level", "annotation", "library_intensity", "intensity"]).to_pandas()
o["n_pts"] = o.intensity.map(len) // 4
o = o.drop(columns="intensity")
ms2 = o[o.ms_level == 2].sort_values(["precursor_id", "library_intensity"], ascending=[True, False])
top = ms2.drop_duplicates("precursor_id")[["precursor_id", "precursor_mz", "precursor_charge", "rt_center",
                                            "annotation", "n_pts"]].rename(columns={"annotation": "frag"})

d = pq.read_table(R + "/diann/diann_xic.parquet", columns=["precursor_id", "ms_level", "annotation", "intensity"]).to_pandas()
d["precursor_id"] = d.precursor_id.map(from_unimod)
d["apex"] = d.intensity.map(lambda v: float(np.max(v)) if len(v) else 0.0)
d = d.drop(columns="intensity")

cols = ["pair", "precursor_id", "ms_level", "annotation", "pearson", "l1_norm", "log2_sum_ratio", "apex_dt_s", "points"]
ca = pd.read_csv(R + "/cmpA.per_trace.tsv", sep="\t", usecols=cols)
cb = pd.read_csv(R + "/cmpB.per_trace.tsv", sep="\t", usecols=cols)
cb["pair"] = "B_vs_openswath"
c = pd.concat([ca, cb])
c = c[(c.ms_level == 1) & (c.annotation == "M+0") | (c.ms_level == 2)]

f = top.copy()
m2 = c[c.ms_level == 2].merge(top[["precursor_id", "frag"]], left_on=["precursor_id", "annotation"],
                              right_on=["precursor_id", "frag"])
m1 = c[c.ms_level == 1]
short = {"diann_vs_openswath": "do", "odia_vs_diann": "ad", "odia_vs_openswath": "ao", "B_vs_openswath": "bo"}
for lvl, m in ((2, m2), (1, m1)):
    for pair, s in short.items():
        g = m[m.pair == pair].set_index("precursor_id")
        f = f.merge(g[["pearson", "l1_norm", "log2_sum_ratio", "apex_dt_s", "points"]].rename(columns={
            "pearson": f"r{lvl}_{s}", "l1_norm": f"l1_{lvl}_{s}", "log2_sum_ratio": f"lr{lvl}_{s}",
            "apex_dt_s": f"dt{lvl}_{s}", "points": f"pts{lvl}_{s}"}), left_on="precursor_id", right_index=True, how="left")
dm2 = d[d.ms_level == 2].merge(top[["precursor_id", "frag"]], left_on=["precursor_id", "annotation"],
                               right_on=["precursor_id", "frag"]).set_index("precursor_id").apex
dm1 = d[d.ms_level == 1].set_index("precursor_id").apex
f["int2"] = np.log10(f.precursor_id.map(dm2).clip(lower=1))
f["int1"] = np.log10(f.precursor_id.map(dm1).clip(lower=1))
f["cov2"] = f.pts2_do / f.n_pts
f["in_all"] = f.precursor_id.isin(dm2.index) & f.r2_do.notna() & f.r2_ao.notna() & f.r1_ad.notna()
f.to_parquet(out, index=False)
print(f"{len(f)} precursors, {int(f.in_all.sum())} with top fragment + MS1 in all three engines -> {out}")
print(f[f.in_all].describe(percentiles=[.05, .25, .5, .75, .95]).T.to_string())
