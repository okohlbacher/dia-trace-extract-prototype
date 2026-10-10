#!/usr/bin/env python3
"""(1) MS1 mass-error calibration and MS1 rule interference/capture on the centroid sample;
(2) example panels of MS2 interference cases (centroid m/z error vs RT with the rule windows).

usage: quality_ms1_examples.py SAMPLE_DIR OUT_PREFIX FIG --a_ms2 ppm --a_ms1 ppm --tol ppm --dnn_ms1 ppm
                               --mu_quad c0,c1,c2 --sigma s0,b --ishift slope,L0   (from quality_sample.py's .txt)
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
from quality_lib import Sample

ap = argparse.ArgumentParser()
ap.add_argument("sample_dir"); ap.add_argument("out"); ap.add_argument("fig")
for k in ("a_ms2", "a_ms1", "tol", "dnn_ms1"): ap.add_argument("--" + k, type=float, required=True)
ap.add_argument("--mu_quad", required=True); ap.add_argument("--sigma", required=True)
ap.add_argument("--ishift", default="0,5", help="intensity shift: ppm per decade above 10^L0 (slope,L0)")
a = ap.parse_args()
k0, k1, k2 = map(float, a.mu_quad.split(",")); s0, bb = map(float, a.sigma.split(","))
quad0 = lambda mz: k0 + k1 * mz + k2 * mz * mz
isl, iL0 = map(float, a.ishift.split(","))
muI = lambda mz, I: quad0(mz) + isl * np.maximum(0.0, np.log10(np.asarray(I, float)) - iL0)
sigma = lambda I: np.sqrt(s0 ** 2 + bb ** 2 / np.asarray(I, float))
mad = lambda v: 1.4826 * np.median(np.abs(v - np.median(v)))
S = Sample(a.sample_dir); q, c = S.q, S.c
out = open(a.out + "_ms1.txt", "w")
def say(*x):
    s = " ".join(map(str, x)); print(s); out.write(s + "\n")

# ---------------------------------------------------------------- MS1: apex = MS1 scan with max M+0 within ±15 ppm
q1 = q[(q.ms_level == 1) & (q.annotation == "M+0")]
c1 = c[c.qid.isin(q1.qid)]
# apex scan: strongest centroid within ±4 ppm of ours-A's MS1 offset (robust against strong neighbours within
# ±15 ppm elsewhere in the ±60 s range); the error is then that of the strongest centroid within ±15 ppm there
ap1 = c1[(c1.ppm - a.a_ms1).abs() <= 4].sort_values("intensity").drop_duplicates("qid", keep="last")[["qid", "pos"]]
best = (c1[c1.ppm.abs() <= 15].merge(ap1, on=["qid", "pos"]).sort_values("intensity").drop_duplicates("qid", keep="last"))
best = best.merge(q1[["qid", "qvalue"]], on="qid")
hi = best[best.qvalue < 0.001]
I = hi.intensity.to_numpy(); r = hi.ppm.to_numpy()
ib = pd.qcut(np.log10(I), 6)
tab = pd.DataFrame({"ppm": r, "ib": ib}).groupby("ib", observed=True).ppm.agg(["size", "median", mad])
mu1 = float(np.median(r)); sg1 = mad(r)
say(f"MS1 apex M+0 (apex scan from ±4 ppm around ours-A offset; q<0.001, n={len(hi)}): median {mu1:.3f} ppm, robust sigma {sg1:.3f} ppm; |err - median| q99 {np.quantile(np.abs(r - mu1), .99):.2f}, q995 {np.quantile(np.abs(r - mu1), .995):.2f}")
say(tab.round(3).to_string())
# truth: within ±3 sigma_bin of mu1 (sigma by intensity, linear interpolation over the bin table)
tab["I"] = pd.DataFrame({"I": I, "ib": ib}).groupby("ib", observed=True).I.median().to_numpy()
sig1 = lambda x: np.interp(np.log10(x), np.log10(tab.I.to_numpy()), tab["<lambda_0>"].to_numpy() if "<lambda_0>" in tab else tab.iloc[:, 2].to_numpy())
c1 = c1.merge(best[["qid", "pos"]].rename(columns={"pos": "apex"}), on="qid")
c1["peak"] = (c1.pos - c1.apex).abs() <= 6        # ±6 MS1 scans ~ ±2 cycles
c1["indist"] = np.abs(c1.ppm - mu1) <= 3 * sig1(c1.intensity)
truth = c1[c1.indist].sort_values("intensity").drop_duplicates(["qid", "pos"], keep="last")
rows = []
def ms1_rule(name, ctr, hw, agg):
    W = c1[np.abs(c1.ppm - ctr) <= hw]
    if agg == "max": W = W.sort_values("intensity").drop_duplicates(["qid", "pos"], keep="last")
    tot = W.intensity[W.peak].sum(); inter = W.intensity[W.peak & ~W.indist].sum()
    cap = W.merge(truth[["qid", "pos", "mz"]], on=["qid", "pos", "mz"])
    rows.append((name, ctr, hw, agg, inter / tot, cap.intensity[cap.peak].sum() / truth.intensity[truth.peak].sum(),
                 float((c1[np.abs(c1.ppm - ctr) <= hw].groupby(["qid", "pos"]).size() > 1).mean())))
ms1_rule("OSW / ours B (0, sum)", 0, a.tol, "sum")
ms1_rule("ours A (offset, max)", a.a_ms1, a.tol, "max")
ms1_rule("DIA-NN export (0, max)", 0, a.dnn_ms1, "max")
ms1_rule("v_tol (offset, DIA-NN width, max)", a.a_ms1, a.dnn_ms1, "max")
for t in (1, 1.5, 2, 2.5, 3, 4, 5, 6, 8):
    ms1_rule(f"median offset ±{t}", mu1, t, "max")
m1 = pd.DataFrame(rows, columns=["rule", "centre_ppm", "half_ppm", "agg", "interference_peak", "capture_peak", "multi_centroid_points"])
m1.to_csv(a.out + "_ms1_rules.tsv", sep="\t", index=False); say(m1.round(4).to_string())

# ---------------------------------------------------------------- MS2 interference examples
q2 = q[q.ms_level == 2]
c2 = c[c.ms_level == 2].merge(q2[["qid", "precursor_id", "annotation", "library_intensity"]], on="qid")
c2["res"] = c2.ppm - muI(c2.target_mz, c2.intensity); c2["indist"] = np.abs(c2.res) <= 3 * sigma(c2.intensity)
# apex per precursor from in-distribution signal of its top-6 fragments
top = q2.assign(rk=q2.groupby("precursor_id").library_intensity.rank(ascending=False, method="first"))
top = top[top.rk <= 6]
tr = c2[c2.qid.isin(top.qid) & c2.indist].groupby(["precursor_id", "pos"]).intensity.sum().reset_index()
apx = tr.sort_values("intensity").drop_duplicates("precursor_id", keep="last").set_index("precursor_id").pos
c2["apex"] = c2.precursor_id.map(apx)
pk = c2[(c2.pos - c2.apex).abs() <= 4]
# A's rule window, max: chosen centroid out of distribution while a true one exists at the same point
inA = pk[np.abs(pk.ppm - a.a_ms2) <= a.tol]
sel = inA.sort_values("intensity").drop_duplicates(["qid", "pos"], keep="last")
bad = sel[~sel.indist].merge(pk[pk.indist].groupby(["qid", "pos"]).intensity.max().rename("true_I").reset_index(), on=["qid", "pos"], how="left")
bad["ratio"] = bad.intensity / bad.true_I.fillna(0).clip(lower=1)
score = bad.groupby("qid").intensity.sum().sort_values(ascending=False)
qtop = q2.set_index("qid")
picks, seen = [], set()
for qid in score.index:
    p = qtop.precursor_id[qid]
    if p in seen: continue
    seen.add(p); picks.append(qid)
    if len(picks) == 3: break
fig, axs = plt.subplots(2, 3, figsize=(13, 5.6), sharex="col", gridspec_kw={"height_ratios": [1, 1.2]})
for j, qid in enumerate(picks):
    g = c2[c2.qid == qid]; rq = qtop.loc[qid]; ap_ = apx[rq.precursor_id]
    lo, hi_ = ap_ - 12, ap_ + 12
    g = g[(g.pos >= lo) & (g.pos <= hi_)]
    rules = [("OSW (0, ±%.1f, sum)" % a.tol, 0.0, a.tol, "sum", "#f4a261"), ("ours A (%.2f, ±%.1f, max)" % (a.a_ms2, a.tol), a.a_ms2, a.tol, "max", "#023e8a"),
             ("quadratic offset, ±4, max", None, 4.0, "max", "#d62828")]
    ax = axs[0, j]
    for name, ctr, hw, agg, col in rules:
        cc = quad0(rq.target_mz) if ctr is None else ctr
        W = g[np.abs(g.ppm - cc) <= hw].groupby("pos").intensity.agg(agg).reindex(range(lo, hi_ + 1), fill_value=0)
        ax.plot(W.index - ap_, W.values, "-o", ms=2.5, lw=1.2, c=col, label=name)
    T = g[g.indist].groupby("pos").intensity.max().reindex(range(lo, hi_ + 1), fill_value=0)
    ax.plot(T.index - ap_, T.values, ":", c="k", lw=1, label="in-distribution signal (±3σ(I))")
    ax.set_title(f"{rq.precursor_id} {rq.annotation} ({rq.target_mz:.3f})", fontsize=8); ax.tick_params(labelsize=7)
    if j == 0: ax.set_ylabel("intensity", fontsize=8)
    ax.legend(fontsize=6)
    ax = axs[1, j]
    sc = ax.scatter(g.pos - ap_, g.ppm, s=4 + 60 * np.sqrt(g.intensity / g.intensity.max()), c=np.where(g.indist, "#2a9d8f", "#e63946"), alpha=0.8)
    for name, ctr, hw, agg, col in rules:
        cc = quad0(rq.target_mz) if ctr is None else ctr
        ax.axhspan(cc - hw, cc + hw, color=col, alpha=0.08); ax.axhline(cc - hw, c=col, lw=0.6); ax.axhline(cc + hw, c=col, lw=0.6)
    ax.set_xlabel("cycles from apex", fontsize=8); ax.tick_params(labelsize=7)
    if j == 0: ax.set_ylabel("centroid error vs library m/z (ppm)\n(green: in distribution, red: not)", fontsize=8)
fig.suptitle("MS2 interference cases (run 01 sample): window bands of each rule; marker area ∝ sqrt(intensity)", fontsize=9)
fig.tight_layout()
for e in ("png", "pdf"): fig.savefig(f"{a.fig}.{e}", dpi=180)
say("interference examples:", [qtop.precursor_id[x] + " " + qtop.annotation[x] for x in picks])
