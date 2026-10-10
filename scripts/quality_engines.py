#!/usr/bin/env python3
"""Engine-agnostic shape quality on the ACTUAL exported traces (all IDs of a run): DIA-NN 1.7.12 --xic,
OpenSWATH, ours A (DIA-NN rules), ours B (OpenSWATH rules), and ours A with narrow MS1 variants
(scripts/ms1_variants.sh: v_tol = ±DIA-NN MS1 accuracy around our offset; v_dnn = DIA-NN's export rule).

Common grid per precursor: the acquisition times of its isolation-window lane over ours' cycle range, restricted
to the cycles where DIA-NN exported data (DIA-NN's XIC is only 60 scans around its own apex), so every engine
is judged on the same spectra. Fragments: those present in all engines. MS1 M+0 of every engine is linearly
interpolated at the same MS2 acquisition times. Metrics: quality_lib.shape_metrics (peak region = ±4 points
around each engine's own apex of the summed top-6 library fragments).
Paired comparison per precursor: median difference and Wilcoxon signed-rank p; stratified by intensity
(quintiles of DIA-NN Precursor.Quantity) and precursor m/z.

usage: quality_engines.py RUN_DIR VARIANT_DIR OUT_PREFIX [--threads 16] [--limit N]
"""
import argparse, os, sys
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS"): os.environ.setdefault(_v, "1")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from multiprocessing import Pool
import numpy as np, pandas as pd, pyarrow.parquet as pq
from scipy.stats import wilcoxon
from quality_lib import shape_metrics

ap = argparse.ArgumentParser()
ap.add_argument("run_dir"); ap.add_argument("variant_dir"); ap.add_argument("out")
ap.add_argument("--threads", type=int, default=16); ap.add_argument("--limit", type=int, default=0)
a = ap.parse_args()
R, V = a.run_dir, a.variant_dir
UNI = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNI.items(): s = s.replace(k, v)
    return s

axes = pq.read_table(R + "/odiaA.axes.parquet").to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
acq = {}
for w, g in axes[axes.lane >= 0].groupby("lane"):
    t = np.full(len(cycle_rt), np.nan); t[g.cycle.to_numpy()] = g.rt.to_numpy(); acq[w] = t

def ours(path, ms1_only=False):
    cols = ["precursor_id", "ms_level", "annotation", "window", "cycle_begin", "intensity"] + ([] if ms1_only else ["library_intensity"])
    t = pq.read_table(path, columns=cols).to_pandas()
    if ms1_only: t = t[t.annotation == "M+0"]
    else: t = t[(t.ms_level == 2) | (t.annotation == "M+0")]
    return t
A = ours(R + "/odiaA.parquet"); B = ours(R + "/odiaB.parquet")
VT = ours(V + "/v_tol.parquet", True); VD = ours(V + "/v_dnn.parquet", True)
d = pq.read_table(R + "/diann/diann_xic.parquet", columns=["precursor_id", "ms_level", "annotation", "rt", "intensity"]).to_pandas()
d["precursor_id"] = d.precursor_id.map(from_unimod)
o = pq.read_table(R + "/osw_traces.parquet").to_pandas()
rep = pd.read_csv(R + "/diann/report.tsv", sep="\t", usecols=["Precursor.Id", "Precursor.Quantity", "Q.Value"])
rep["precursor_id"] = rep["Precursor.Id"].map(from_unimod)
quant = rep.set_index("precursor_id")["Precursor.Quantity"]

def group(df): return {k: g for k, g in df.groupby("precursor_id", sort=False)}
GA, GB, GVT, GVD, GD, GO = map(group, (A, B, VT, VD, d, o))
pids = [p for p in GA if p in GD and p in GO and p in GB]
if a.limit: pids = pids[:a.limit]
print(f"{len(pids)} precursors in all engines")

def per_precursor(p):
    ga, gb, gd, go = GA[p], GB[p], GD[p], GO[p]
    f2 = ga[ga.ms_level == 2]
    win = int(f2.window.iloc[0]); cb = int(f2.cycle_begin.iloc[0]); n = len(f2.intensity.iloc[0]) // 4
    t = acq[win][cb:cb + n]
    dd = {an: (np.asarray(rt, float), np.asarray(y, float)) for lvl, an, rt, y in gd[["ms_level", "annotation", "rt", "intensity"]].itertuples(index=False)}
    oo = {an: (np.frombuffer(rb, "<f4").astype(float), np.frombuffer(yb, "<f4").astype(float)) for an, rb, yb in go[["annotation", "rt", "intensity"]].itertuples(index=False)}
    frags = [an for an in f2.annotation if an in dd and an in oo]
    if len(frags) < 3 or "M+0" not in dd: return None
    # DIA-NN span on this grid
    rt0 = dd[frags[0]][0]; rt0 = rt0[rt0 > 0]
    m = (t >= rt0.min() - 0.05) & (t <= rt0.max() + 0.05) & ~np.isnan(t)
    if m.sum() < 9: return None
    tg = t[m]
    lib = f2.set_index("annotation").library_intensity.loc[frags].to_numpy()
    def snap(rt, y):   # exported (rt, y) at the window spectra -> grid (exact spectra, tolerance 0.05 s)
        k = rt > 0; rt, y = rt[k], y[k]; out = np.zeros(len(tg))
        idx = np.searchsorted(tg, rt); idx = np.clip(idx, 1, len(tg) - 1)
        idx = np.where(np.abs(tg[idx - 1] - rt) < np.abs(tg[idx] - rt), idx - 1, idx)
        ok = np.abs(tg[idx] - rt) < 0.05; out[idx[ok]] = y[ok]; return out
    def interp(rt, y):
        k = rt > 0; rt, y = rt[k], y[k]
        o_ = np.argsort(rt); return np.interp(tg, rt[o_], y[o_], left=0, right=0)
    def ours_ms2(g):
        g2 = g[g.ms_level == 2].set_index("annotation")
        return np.array([np.frombuffer(g2.intensity.loc[an], "<f4").astype(float)[m] for an in frags])
    def ours_ms1(g):
        r = g[g.annotation == "M+0"].iloc[0]; y = np.frombuffer(r.intensity, "<f4").astype(float)
        return np.interp(tg, cycle_rt[int(r.cycle_begin):int(r.cycle_begin) + len(y)], y)
    FA, FB = ours_ms2(ga), ours_ms2(gb)
    FD = np.array([snap(*dd[an]) for an in frags]); FO = np.array([snap(*oo[an]) for an in frags])
    eng = {"DIA-NN": (FD, interp(*dd["M+0"])), "OSW": (FO, interp(*oo["M+0"]) if "M+0" in oo else None),
           "ours A": (FA, ours_ms1(ga)), "ours B": (FB, ours_ms1(gb))}
    if p in GVT: eng["ours A, MS1 v_tol"] = (FA, ours_ms1(GVT[p]))
    if p in GVD: eng["ours A, MS1 v_dnn"] = (FA, ours_ms1(GVD[p]))
    out = []
    for e, (F, M1) in eng.items():
        r = shape_metrics(F, lib, M1)
        if r: r.update(precursor_id=p, engine=e, n_frag=len(frags)); out.append(r)
    return out

with Pool(a.threads) as pool:
    res = pool.map(per_precursor, pids, chunksize=200)
rows = [r for x in res if x for r in x]
df = pd.DataFrame(rows)
pm = A[A.ms_level == 2].drop_duplicates("precursor_id").set_index("precursor_id")
meta = pq.read_table(R + "/odiaA.parquet", columns=["precursor_id", "precursor_mz"]).to_pandas().drop_duplicates("precursor_id").set_index("precursor_id")
df["precursor_mz"] = df.precursor_id.map(meta.precursor_mz); df["quantity"] = df.precursor_id.map(quant)
df.to_csv(a.out + "_per_precursor.tsv", sep="\t", index=False)

METRICS = ["coelution", "cos_apex", "cos_area", "apex_spread", "jagged", "spike_frac", "dropout_frac", "ms1_coelution", "ms1_dropout"]
BETTER = {"coelution": 1, "cos_apex": 1, "cos_area": 1, "apex_spread": -1, "jagged": -1, "spike_frac": -1,
          "dropout_frac": -1, "ms1_coelution": 1, "ms1_dropout": -1}
summ = df.groupby("engine")[METRICS].agg(["median", "mean"])
summ.columns = [f"{m}_{s}" for m, s in summ.columns]
summ.to_csv(a.out + "_engine_summary.tsv", sep="\t")
with pd.option_context("display.width", 250, "display.max_columns", 30): print(df.groupby("engine")[METRICS].median().round(4))

piv = {m: df.pivot(index="precursor_id", columns="engine", values=m) for m in METRICS}
pairs = [("ours A", "DIA-NN"), ("ours B", "OSW"), ("DIA-NN", "OSW"), ("ours A", "ours B"), ("ours A, MS1 v_tol", "ours A"),
         ("ours A, MS1 v_dnn", "ours A"), ("ours A, MS1 v_tol", "OSW")]
pr = []
for x, y in pairs:
    for m in METRICS:
        if x not in piv[m] or y not in piv[m]: continue
        dd = (piv[m][x] - piv[m][y]).dropna()
        if x.startswith("ours A, MS1") and not m.startswith("ms1"): continue
        nz = dd[dd != 0]
        p = wilcoxon(nz).pvalue if len(nz) > 10 else np.nan
        pr.append((x, y, m, len(dd), dd.median(), dd.mean(), float((np.sign(dd) == BETTER[m]).mean()),
                   float((np.sign(dd) == -BETTER[m]).mean()), p))
pr = pd.DataFrame(pr, columns=["x", "y", "metric", "n", "median_diff", "mean_diff", "frac_x_better", "frac_y_better", "wilcoxon_p"])
pr.to_csv(a.out + "_paired.tsv", sep="\t", index=False)
with pd.option_context("display.width", 250): print(pr.round(5).to_string())

# stratified: DIA-NN vs OSW and A vs B by intensity quintile and m/z
df["Iq"] = pd.qcut(np.log10(df.quantity.clip(lower=1)), 5, labels=[f"Q{i}" for i in range(1, 6)])
df["mzb"] = pd.cut(df.precursor_mz, [0, 500, 650, 800, 2000]).astype(str)
st = []
for col in ("Iq", "mzb"):
    for lev, g in df.groupby(col, observed=True):
        md = g.groupby("engine")[["coelution", "cos_apex", "dropout_frac", "spike_frac", "ms1_coelution"]].median()
        md.insert(0, "stratum", f"{col}={lev}"); st.append(md)
st = pd.concat(st); st.to_csv(a.out + "_strata.tsv", sep="\t")
with pd.option_context("display.width", 250): print(st.round(4).to_string())
