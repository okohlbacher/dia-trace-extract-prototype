#!/usr/bin/env python3
"""Trace quality on the centroid sample (quality_reextract.py): mass-error calibration, validation of the rule
emulation against the engines' exported traces, interference / capture / shape for engine and candidate rules,
and the MS2 tolerance trade-off.

usage: quality_sample.py RUN_DIR SAMPLE_DIR OUT_PREFIX FIG_PREFIX --dnn_quad t0,t1,t2 --dnn_ms1 ppm
                         --a_ms2 ppm --a_ms1 ppm --tol ppm
  --dnn_quad: DIA-NN's MS2 mass-correction transform (diann.log --verbose 3, last "Mass correction transform"):
              delta(Th) = t0*mz^2 + t1 + t2*mz;  --dnn_ms1: its MS1 extraction accuracy (ppm)
  --a_ms2/--a_ms1: ours-A offsets (odiaA.log); --tol: optimised MS2 accuracy (ppm), used by all engines
Truth model: a centroid is "in distribution" when |ppm - mu(mz, I)| <= 3 sigma(I), mu = quadratic in m/z plus a
linear shift per intensity decade above 10^L0 (strong Astral centroids drift up), sigma(I) = sqrt(s0^2 + b^2 / I) fitted to apex points of top-6 fragments of q < 0.001 IDs.
"""
import argparse, os, sys
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS"): os.environ.setdefault(_v, "16")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, pandas as pd, pyarrow.parquet as pq
from scipy.optimize import curve_fit
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
from quality_lib import Sample, shape_metrics

ap = argparse.ArgumentParser()
ap.add_argument("run_dir"); ap.add_argument("sample_dir"); ap.add_argument("out"); ap.add_argument("fig")
ap.add_argument("--dnn_quad", required=True); ap.add_argument("--dnn_ms1", type=float, required=True)
ap.add_argument("--a_ms2", type=float, required=True); ap.add_argument("--a_ms1", type=float, required=True)
ap.add_argument("--tol", type=float, required=True)
a = ap.parse_args()
R = a.run_dir; TOL = a.tol
t0, t1, t2 = map(float, a.dnn_quad.split(","))
dnn_ctr = lambda mz: (t0 * mz * mz + t1 + t2 * mz) / mz * 1e6
S = Sample(a.sample_dir); q, c = S.q, S.c
mad = lambda v: 1.4826 * np.median(np.abs(v - np.median(v)))
log = open(a.out + ".txt", "w")
def say(*x):
    s = " ".join(str(v) for v in x); print(s); log.write(s + "\n"); log.flush()

# ------------------------------------------------------------------ 1. calibration from apex points
q2 = q[q.ms_level == 2].copy()
q2["rank"] = q2.groupby("precursor_id").library_intensity.rank(ascending=False, method="first")
top = q2[q2["rank"] <= 6]
c2 = c[c.ms_level == 2]
cc = c2[c2.qid.isin(top.qid) & ((c2.ppm + 3).abs() <= 12)]
m = cc.groupby(["qid", "pos"]).intensity.max().reset_index().merge(top[["qid", "precursor_id"]], on="qid")
apex = (m.groupby(["precursor_id", "pos"]).intensity.sum().reset_index().sort_values("intensity")
        .drop_duplicates("precursor_id", keep="last")[["precursor_id", "pos"]].rename(columns={"pos": "apex"}))
x = (c2[c2.qid.isin(top.qid) & (c2.ppm.abs() <= 15)].merge(top[["qid", "precursor_id", "fragment_charge", "rt_center", "qvalue"]], on="qid")
     .merge(apex, on="precursor_id"))
x = x[x.pos == x.apex].sort_values("intensity").drop_duplicates("qid", keep="last")
cal = x[x.qvalue < 0.001].copy()
say(f"calibration: {len(cal)} apex centroids of top-6 fragments, {cal.precursor_id.nunique()} precursors with q < 0.001")

def sig_model(I, s0, b): return np.sqrt(s0 ** 2 + b ** 2 / I)
def design(mz, I, L0):  # quadratic in m/z + linear shift per decade of intensity above 10^L0 (Astral: strong
    mz, lI = np.asarray(mz, float), np.log10(np.asarray(I, float))   # centroids drift towards higher m/z)
    return np.c_[np.ones(len(mz)), mz, mz ** 2, np.maximum(0.0, lI - L0)]
def fit(L0, keep, s0, b):
    X = design(cal.target_mz[keep], cal.intensity[keep], L0); w = 1 / sig_model(cal.intensity[keep].to_numpy(), s0, b)
    cf = np.linalg.lstsq(X * w[:, None], cal.ppm[keep].to_numpy() * w, rcond=None)[0]
    rr = cal.ppm.to_numpy() - design(cal.target_mz, cal.intensity, L0) @ cf
    return cf, rr, float(((rr[keep] * w) ** 2).sum())
keep = np.ones(len(cal), bool); s0, b = 1.0, 50.0; L0 = 5.0
for it in range(6):   # robust: quadratic offset + intensity shift + intensity-dependent sigma, 3-sigma trimming
    L0 = min(np.arange(4.0, 6.01, 0.25), key=lambda L: fit(L, keep, s0, b)[2])
    coef, res, _ = fit(L0, keep, s0, b)
    ib = pd.qcut(np.log10(cal.intensity), 12, labels=False)
    tab = pd.DataFrame({"res": res, "I": cal.intensity, "ib": ib})[keep].groupby("ib").agg(I=("I", "median"), s=("res", mad))
    (s0, b), _ = curve_fit(sig_model, tab.I, tab.s, p0=[s0, b], bounds=(0, np.inf))
    keep = np.abs(res) <= 3 * sig_model(cal.intensity.to_numpy(), s0, b)
mu_const = float(np.median(cal.ppm[keep]))
quad = lambda mz: coef[0] + coef[1] * mz + coef[2] * mz * mz                      # at low/moderate intensity
muI = lambda mz, I: quad(mz) + coef[3] * np.maximum(0.0, np.log10(np.asarray(I, float)) - L0)
# m/z-only quadratic (no intensity term), for the "quad" rule family
_q = np.linalg.lstsq(np.c_[np.ones(keep.sum()), cal.target_mz[keep], cal.target_mz[keep] ** 2], cal.ppm[keep].to_numpy(), rcond=None)[0]
quad_only = lambda mz: _q[0] + _q[1] * mz + _q[2] * mz * mz
sigma = lambda I: sig_model(np.asarray(I, float), s0, b)
cal["res"] = res; cal["inlier"] = keep
say(f"intensity shift: +{coef[3]:.3f} ppm per decade above 10^{L0:.2f} (e.g. +{coef[3]*max(0,6-L0):.2f} ppm at 1e6)")
say(f"offset: constant {mu_const:.3f} ppm; quadratic mu(mz) = {coef[0]:.4f} + {coef[1]:.3e} mz + {coef[2]:.3e} mz^2 "
    f"({quad(300):.2f} / {quad(800):.2f} / {quad(1500):.2f} ppm at 300/800/1500); DIA-NN's {dnn_ctr(300):.2f} / {dnn_ctr(800):.2f} / {dnn_ctr(1500):.2f}")
say(f"sigma(I) = sqrt({s0:.3f}^2 + {b:.1f}^2/I): {sigma(1e3):.2f} / {sigma(1e4):.2f} / {sigma(1e5):.2f} ppm at I = 1e3/1e4/1e5; "
    f"global robust sigma (inliers) {mad(res[keep]):.3f}; inliers {keep.mean():.3f}")
# dependence checks: residual median / sigma by m/z, RT, fragment charge, intensity
cal["mzb"] = pd.cut(cal.target_mz, [0, 300, 400, 600, 800, 1000, 1200, 2000]).astype(str)
cal["rtb"] = pd.cut(cal.rt_center, 6).astype(str); cal["Ib"] = pd.qcut(np.log10(cal.intensity), 6).astype(str)
cal["zb"] = cal.fragment_charge.astype(int).astype(str)
dep = []
for col in ("mzb", "rtb", "zb", "Ib"):
    g = cal[cal.inlier].groupby(col).agg(n=("res", "size"), median_res=("res", "median"), sigma=("res", mad),
                                         raw_median=("ppm", "median"))
    g.insert(0, "by", col); dep.append(g)
dep = pd.concat(dep); dep.to_csv(a.out + "_calibration_dependence.tsv", sep="\t"); say(dep.round(3).to_string())

# coverage of the residual distribution: narrowest half width keeping 99 / 99.5% of apex signal
# (strong apex points = top half intensity, where interference is rare; count- and intensity-weighted)
strong = cal[cal.intensity >= cal.intensity.median()]
cov = []
for name, r in (("const", strong.ppm - mu_const), ("quad_only", strong.ppm - quad_only(strong.target_mz)), ("quad+I", strong.res), ("none", strong.ppm), ("A_const", strong.ppm - a.a_ms2),
                ("dnn_quad", strong.ppm - dnn_ctr(strong.target_mz))):
    r = np.abs(r.to_numpy()); w = strong.intensity.to_numpy(); o = np.argsort(r); cw = np.cumsum(w[o]) / w.sum()
    cov.append((name, np.quantile(r, .99), np.quantile(r, .995), r[o][np.searchsorted(cw, .99)], r[o][np.searchsorted(cw, .995)],
                float((r <= TOL).mean())))
cov = pd.DataFrame(cov, columns=["centre", "q99_count", "q995_count", "q99_intensity", "q995_intensity", "frac_within_tol"])
cov.to_csv(a.out + "_coverage.tsv", sep="\t", index=False); say(cov.round(3).to_string())

# ------------------------------------------------------------------ 2. validation against the exported traces
UNI = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNI.items(): s = s.replace(k, v)
    return s
want = list(q.precursor_id.unique())
axes = pq.read_table(R + "/odiaA.axes.parquet").to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
ms1_rt = axes[axes.lane == -2].sort_values("cycle").rt.to_numpy()
acq = {}
for w, g in axes[axes.lane >= 0].groupby("lane"):
    t = np.full(len(cycle_rt), np.nan); t[g.cycle.to_numpy()] = g.rt.to_numpy(); acq[w] = t
key = {(p, an): (qid, lane, c0) for p, an, qid, lane, c0 in q[["precursor_id", "annotation", "qid", "lane", "c0"]].itertuples(index=False)}

def ours(path):
    t = pq.read_table(path, columns=["precursor_id", "ms_level", "annotation", "cycle_begin", "intensity"],
                      filters=[("precursor_id", "in", want)]).to_pandas()
    return {(p, an): (lvl, cb, np.frombuffer(bl, "<f4").astype(float)) for p, lvl, an, cb, bl in t.itertuples(index=False)}
expA, expB = ours(R + "/odiaA.parquet"), ours(R + "/odiaB.parquet")
d = pq.read_table(R + "/diann/diann_xic.parquet", columns=["precursor_id", "ms_level", "annotation", "rt", "intensity"]).to_pandas()
d["precursor_id"] = d.precursor_id.map(from_unimod); d = d[d.precursor_id.isin(want)]
expD = {(p, an): (lvl, np.asarray(rt, float), np.asarray(y, float)) for p, lvl, an, rt, y in d.itertuples(index=False)}
s_ = pq.read_table(R + "/osw_traces.parquet", filters=[("precursor_id", "in", want)]).to_pandas()
expO = {(p, an): (lvl, np.frombuffer(rb, "<f4").astype(float), np.frombuffer(yb, "<f4").astype(float)) for p, lvl, an, rb, yb in s_.itertuples(index=False)}

def ms1_on_cycles(vals, j0, cb, n):
    """per-MS1-scan values (rows j0..) -> linear interpolation at cycle_rt[cb:cb+n] (as the extractor does)."""
    t = cycle_rt[cb:cb + n]; rr = ms1_rt[j0:j0 + len(vals)]
    return np.interp(t, rr, vals)

def to_positions(lane, rt, y, c0, n):
    """exported (rt, y) of DIA-NN / OSW -> positions of our sample (MS2: cycles of the lane; MS1: MS1 rows)."""
    ok = rt > 0; rt, y = rt[ok], y[ok]
    ref = acq[lane][c0:c0 + n] if lane >= 0 else ms1_rt[c0:c0 + n]
    out = np.full(n, np.nan)
    for t_, v in zip(rt, y):
        k = np.nanargmin(np.abs(ref - t_))
        if abs(ref[k] - t_) < 0.05: out[k] = v
    return out

def agree(mine, ref):
    ok = ~np.isnan(ref) & ~np.isnan(mine)
    if ok.sum() < 5: return None
    x_, y_ = mine[ok], ref[ok]
    r = np.corrcoef(x_, y_)[0, 1] if x_.std() > 0 and y_.std() > 0 else (1.0 if np.allclose(x_, y_) else np.nan)
    rel = np.max(np.abs(x_ - y_)) / max(np.max(np.abs(y_)), 1e-9)
    return r, rel

RULES_ENGINE = {   # name: (MS2 centre, MS2 half, agg, strict, MS1 centre, MS1 half, MS1 agg)
    "OSW": (0.0, TOL, "sum", False, 0.0, TOL, "sum"),
    "DIA-NN": (dnn_ctr, TOL, "max", True, 0.0, a.dnn_ms1, "max"),
    "ours A": (a.a_ms2, TOL, "max", False, a.a_ms1, TOL, "max"),
    "ours B": (0.0, TOL, "sum", False, 0.0, TOL, "sum"),
}
ext = {}
for name, (c2_, h2, ag, st, c1_, h1, ag1) in RULES_ENGINE.items():
    ext[name] = (S.extract(2, c2_, h2, ag, st), S.extract(1, c1_, h1, ag1, st))
val = []
for name, exp in (("ours A", expA), ("ours B", expB), ("DIA-NN", expD), ("OSW", expO)):
    e2, e1 = ext[name]
    for (p, an), (qid, lane, c0) in key.items():
        if (p, an) not in exp: continue
        lvl = 2 if lane >= 0 else 1
        if lvl == 1 and an != "M+0": continue
        mine = (e2 if lvl == 2 else e1)[qid]
        if name.startswith("ours"):
            _, cb, y = exp[(p, an)]
            ref = y if lvl == 2 else y
            if lvl == 1: mine = ms1_on_cycles(mine, c0, cb, len(y))
            if len(ref) != len(mine): continue
        else:
            _, rt, y = exp[(p, an)]
            ref = to_positions(lane, rt, y, c0, len(mine))
        g = agree(np.asarray(mine, float), np.asarray(ref, float))
        if g: val.append((name, lvl) + g)
val = pd.DataFrame(val, columns=["engine", "ms_level", "pearson", "max_rel_diff"])
vs = val.groupby(["engine", "ms_level"]).agg(traces=("pearson", "size"), pearson_median=("pearson", "median"),
                                             frac_r_gt_0_999=("pearson", lambda v: (v > 0.999).mean()),
                                             frac_maxrel_lt_1pct=("max_rel_diff", lambda v: (v < 0.01).mean()))
vs.to_csv(a.out + "_validation.tsv", sep="\t"); say("validation of the rule emulation vs exported traces:\n" + vs.round(4).to_string())

# ------------------------------------------------------------------ 3. rule metrics: interference, capture, shapes
c2 = c[c.ms_level == 2].copy()
c2 = c2.merge(q2[["qid", "precursor_id"]], on="qid").merge(apex, on="precursor_id", how="left")
c2["res_q"] = c2.ppm - muI(c2.target_mz, c2.intensity)
c2["indist"] = np.abs(c2.res_q) <= 3 * sigma(c2.intensity)
c2["peak"] = (c2.pos - c2.apex).abs() <= 4
truth = c2[c2.indist].sort_values("intensity").drop_duplicates(["qid", "pos"], keep="last")   # s* per point
T_all, T_peak = truth.intensity.sum(), truth.intensity[truth.peak].sum()
lib_of = q2.set_index("qid").library_intensity

def rule_metrics(name, centre, half, agg, strict=False, shapes=True):
    ctr = centre(c2) if callable(centre) else centre      # centre(rows) -> ppm per centroid
    hw = half(c2) if callable(half) else half
    dev = (c2.ppm.to_numpy() - ctr)
    inw = (np.abs(dev) < hw) if strict else (np.abs(dev) <= hw)
    W = c2[inw]
    g = W.groupby(["qid", "pos"])
    npts = g.size()
    if agg == "max":
        sel = W.sort_values("intensity").drop_duplicates(["qid", "pos"], keep="last")
        tot = sel.intensity.sum(); interf = sel.intensity[~sel.indist].sum()
        tot_pk = sel.intensity[sel.peak].sum(); interf_pk = sel.intensity[sel.peak & ~sel.indist].sum()
        cap = sel[sel.indist]; cap = cap.merge(truth[["qid", "pos", "mz"]], on=["qid", "pos", "mz"])
    else:
        tot = W.intensity.sum(); interf = W.intensity[~W.indist].sum()
        tot_pk = W.intensity[W.peak].sum(); interf_pk = W.intensity[W.peak & ~W.indist].sum()
        cap = W.merge(truth[["qid", "pos", "mz"]], on=["qid", "pos", "mz"])
    row = {"rule": name, "agg": agg, "interference_all": interf / tot, "interference_peak": interf_pk / tot_pk,
           "multi_centroid_points": float((npts > 1).mean()), "capture_all": cap.intensity.sum() / T_all,
           "capture_peak": cap.intensity[cap.peak].sum() / T_peak}
    if shapes:
        dense = S.dense(sel.set_index(["qid", "pos"]).intensity if agg == "max" else g.intensity.sum(), 2)
        sm = []
        for p, gq in q2.groupby("precursor_id"):
            F = np.array([dense[k] for k in gq.qid]); r_ = shape_metrics(F, gq.library_intensity.to_numpy())
            if r_: sm.append(r_)
        sm = pd.DataFrame(sm)
        for k in ("coelution", "cos_apex", "cos_area", "apex_spread", "jagged", "spike_frac", "dropout_frac"):
            row[k] = float(sm[k].mean()) if k in ("spike_frac", "dropout_frac", "apex_spread") else float(sm[k].median())
        # agreement with DIA-NN's exported MS2 traces
        rs = []
        for (p, an), (qid, lane, c0) in key.items():
            if lane < 0 or (p, an) not in expD: continue
            _, rt, y = expD[(p, an)]
            g_ = agree(dense[qid], to_positions(lane, rt, y, c0, len(dense[qid])))
            if g_: rs.append(g_[0])
        row["r_vs_diann_median"] = float(np.nanmedian(rs)); row["frac_r_vs_diann_gt_0_99"] = float(np.mean(np.array(rs) > 0.99))
    return row

rows = [rule_metrics("OSW / ours B (no corr., sum, ±%.2f)" % TOL, 0.0, TOL, "sum"),
        rule_metrics("DIA-NN (quad. corr., max, ±%.2f)" % TOL, lambda cc: dnn_ctr(cc.target_mz.to_numpy()), TOL, "max", strict=True),
        rule_metrics("ours A (%.2f ppm, max, ±%.2f)" % (a.a_ms2, TOL), a.a_ms2, TOL, "max")]
sweep = []
for model, centre in (("const", mu_const), ("quad", lambda cc: quad_only(cc.target_mz.to_numpy())),
                      ("quadI", lambda cc: muI(cc.target_mz.to_numpy(), cc.intensity.to_numpy()))):
    for t in (1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 6, 7, 8, 10):
        for agg in ("max", "sum"):
            if agg == "sum" and model != "const": continue
            r_ = rule_metrics(f"{model} ±{t}", centre, t, agg, shapes=(agg == "max" and t in (2, 3, 4, 5, 6, 8)))
            r_.update(model=model, half=t); sweep.append(r_)
for k in (2, 2.5, 3, 3.5, 4, 5):
    for cap_ in (None,):
        r_ = rule_metrics(f"quadI ±{k}σ(I)", lambda cc: muI(cc.target_mz.to_numpy(), cc.intensity.to_numpy()), lambda cc, k=k: k * sigma(cc.intensity.to_numpy()), "max", shapes=k in (2.5, 3, 4))
        r_.update(model="quad_sigmaI", half=k); sweep.append(r_)
sw = pd.DataFrame(sweep); eng = pd.DataFrame(rows)
eng.to_csv(a.out + "_engine_rules.tsv", sep="\t", index=False); sw.to_csv(a.out + "_sweep.tsv", sep="\t", index=False)
with pd.option_context("display.width", 250, "display.max_columns", 30):
    say(eng.round(4).to_string()); say(sw.round(4).to_string())

# ------------------------------------------------------------------ figures
fig, axs = plt.subplots(1, 2, figsize=(10, 3.8))
ax = axs[0]
ax.scatter(cal.target_mz, cal.ppm, s=1, alpha=0.15, c="#888")
mzs = np.linspace(150, 1700, 200)
ax.plot(mzs, quad_only(mzs), c="#d62828", label="fitted quadratic"); ax.axhline(mu_const, c="#2a9d8f", ls="--", label="fitted constant")
ax.plot(mzs, dnn_ctr(mzs), c="#023e8a", label="DIA-NN 1.7.12 correction"); ax.axhline(a.a_ms2, c="#f4a261", ls=":", label="ours A constant")
ax.set_ylim(-12, 6); ax.set_xlabel("fragment m/z"); ax.set_ylabel("apex centroid error (ppm)"); ax.legend(fontsize=7)
ax.set_title("MS2 mass error, q<0.001 apex points", fontsize=9)
ax = axs[1]
ax.scatter(cal.intensity, cal.ppm - quad(cal.target_mz), s=1, alpha=0.15, c="#888"); I = np.logspace(2, 6.5, 100)
sh = coef[3] * np.maximum(0, np.log10(I) - L0)
for k_, ls in ((1, "-"), (3, "--")):
    ax.plot(I, sh + k_ * sigma(I), c="#d62828", ls=ls); ax.plot(I, sh - k_ * sigma(I), c="#d62828", ls=ls)
ax.plot(I, sh, c="#d62828", lw=0.8)
ax.axhline(TOL, c="k", lw=0.8); ax.axhline(-TOL, c="k", lw=0.8)
ax.set_xscale("log"); ax.set_ylim(-15, 15); ax.set_xlabel("centroid intensity"); ax.set_ylabel("error minus m/z model (ppm)")
ax.set_title(f"σ(I) = sqrt({s0:.2f}² + {b:.0f}²/I); ±1σ, ±3σ; black ±{TOL:.2f}", fontsize=9)
fig.tight_layout()
for ext_ in ("png", "pdf"): fig.savefig(f"{a.fig}_calibration.{ext_}", dpi=180)
say("done")
