#!/usr/bin/env python3
"""Why do ~8% of MS2 traces of ours P (DIA-NN parity) disagree with DIA-NN 1.7.12's --xic export?

Part A (all traces): stratify the tail of cmpP.per_trace.tsv (odia_vs_diann, MS2) by fragment m/z, charge,
loss, intensity, window, RT; check DIA-NN's extraction m/z vs ours, the window/spectra DIA-NN used
(its RTs vs our lane's acquisition times), and the predicted centre difference
dc = DIA-NN quadratic correction - our constant offset.
Part B (3000-precursor centroid sample, scripts/quality_reextract.py): emulate both rules on the same centroids
and test whether DIA-NN's transform removes the tail; analyse what remains (window-edge float32 effects etc.).
Part C: example panels.

usage: ms2_tail.py RUN_DIR SAMPLE_DIR OUT_DIR FIG --dnn_quad t0,t1,t2 --p_ms2 ppm --tol ppm
"""
import argparse, os, sys
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS"): os.environ.setdefault(_v, "16")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, pandas as pd, pyarrow.parquet as pq
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
from quality_lib import Sample

ap = argparse.ArgumentParser()
ap.add_argument("run_dir"); ap.add_argument("sample_dir"); ap.add_argument("out"); ap.add_argument("fig")
ap.add_argument("--dnn_quad", required=True); ap.add_argument("--p_ms2", type=float, required=True)
ap.add_argument("--tol", type=float, required=True)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
R, TOL, POFF = a.run_dir, a.tol, a.p_ms2
t0, t1, t2 = map(float, a.dnn_quad.split(","))
dnn_ctr = lambda mz: (t0 * mz * mz + t1 + t2 * mz) / mz * 1e6
log = open(a.out + "/ms2_tail.txt", "w")
def say(*x):
    s = " ".join(map(str, x)); print(s); log.write(s + "\n"); log.flush()
UNI = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNI.items(): s = s.replace(k, v)
    return s
PD = lambda: pd.option_context("display.width", 250, "display.max_columns", 30, "display.float_format", "{:.4f}".format)

# ------------------------------------------------------------------ A. all traces
c = pd.read_csv(R + "/cmpP.per_trace.tsv", sep="\t")
c = c[(c.pair == "odia_vs_diann") & (c.ms_level == 2)]
o = pq.read_table(R + "/odiaP.parquet", columns=["precursor_id", "precursor_mz", "precursor_charge", "ms_level", "annotation",
                  "fragment_type", "fragment_charge", "fragment_loss", "target_mz", "library_intensity", "window", "rt_center",
                  "cycle_begin", "intensity"]).to_pandas()
o = o[o.ms_level == 2]
o["n"] = o.intensity.map(len) // 4; o = o.drop(columns="intensity")
o["lib_rank"] = o.groupby("precursor_id").library_intensity.rank(ascending=False, method="first")
d = pq.read_table(R + "/diann/diann_xic.parquet", columns=["precursor_id", "ms_level", "annotation", "extraction_mz", "rt", "intensity"]).to_pandas()
d["precursor_id"] = d.precursor_id.map(from_unimod)
d2 = d[d.ms_level == 2].copy()
d2["dnn_apex"] = d2.intensity.map(lambda v: float(np.max(v)) if len(v) else 0.0)
c = c.merge(o, on=["precursor_id", "annotation"]).merge(d2[["precursor_id", "annotation", "extraction_mz", "dnn_apex"]],
                                                        on=["precursor_id", "annotation"], how="left")
c["is_tail"] = c.pearson < 0.9; c["is_dev"] = c.l1_norm > 0.01
c["mz_diff_ppm"] = (c.extraction_mz - c.target_mz) / c.target_mz * 1e6
c["dc"] = dnn_ctr(c.target_mz) - POFF          # DIA-NN window centre minus ours (ppm)
say(f"A. {len(c)} MS2 traces: r<0.9 {c.is_tail.mean():.4f}, l1>0.01 {c.is_dev.mean():.4f}, r<0.99 {(c.pearson < 0.99).mean():.4f}")
say(f"   DIA-NN extraction m/z vs ours: |diff| median {c.mz_diff_ppm.abs().median():.4f} ppm, max {c.mz_diff_ppm.abs().max():.4f} ppm "
    "(float32 rounding of the library m/z only)")

# DIA-NN's spectra: do its RTs coincide with our lane's acquisition times?
axes = pq.read_table(R + "/odiaP.axes.parquet").to_pandas()
lanes = axes[axes.lane >= 0][["lane", "cycle", "rt"]]
alltimes = lanes.sort_values("rt"); at = alltimes.rt.to_numpy(); al = alltimes.lane.to_numpy()
cyc_of = {}
first = d2.drop_duplicates("precursor_id")[["precursor_id", "rt"]]
win_of = o.drop_duplicates("precursor_id").set_index("precursor_id").window
mism, other = 0, 0; rows = []
for pid, rt in first.itertuples(index=False):
    rt = np.asarray(rt, float); rt = rt[rt > 0]
    if pid not in win_of.index or not len(rt): continue
    k = np.clip(np.searchsorted(at, rt), 1, len(at) - 1); k = np.where(np.abs(at[k - 1] - rt) < np.abs(at[k] - rt), k - 1, k)
    lane_hit = al[k]; ok = np.abs(at[k] - rt) < 0.05
    w = win_of[pid]
    rows.append((pid, float(ok.mean()), float((lane_hit[ok] == w).mean()) if ok.any() else np.nan, len(rt)))
sp = pd.DataFrame(rows, columns=["precursor_id", "frac_rt_matched", "frac_same_window", "n_dnn_points"])
sp.to_csv(a.out + "/diann_spectra_check.tsv", sep="\t", index=False)
say(f"   DIA-NN XIC times matching an acquired MS2 spectrum (±0.05 s): {sp.frac_rt_matched.mean():.5f}; "
    f"same isolation window as ours: {sp.frac_same_window.mean():.5f}; precursors with any other window: {(sp.frac_same_window < 1).sum()}")
# window-edge precursors
wb = axes[axes.lane >= 0].groupby("lane")[["window_lower", "window_upper"]].first()
pm = o.drop_duplicates("precursor_id").set_index("precursor_id")
edge = np.minimum(pm.precursor_mz - wb.window_lower.reindex(pm.window).to_numpy(), wb.window_upper.reindex(pm.window).to_numpy() - pm.precursor_mz)
say(f"   precursors within 0.01 Th of a window edge: {(edge < 0.01).sum()} of {len(pm)}")

# stratification
c["mzb"] = pd.cut(c.target_mz, [0, 200, 300, 400, 600, 800, 1000, 1200, 1500, 2500])
c["dcb"] = pd.cut(c.dc.abs(), [0, 0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 3])
c["Ib"] = pd.qcut(np.log10(c.dnn_apex.clip(lower=1)), 10)
c["rtb"] = pd.cut(c.rt_center, 8); c["winb"] = pd.cut(c.window, [-1, 50, 100, 150, 200, 250, 300])
c["rankb"] = pd.cut(c.lib_rank, [0, 3, 6, 10, 30])
c["ptsb"] = pd.cut(c.points, [0, 20, 40, 50, 55, 60])
st = []
for col in ("dcb", "mzb", "fragment_charge", "fragment_loss", "fragment_type", "Ib", "rankb", "rtb", "winb", "ptsb"):
    g = c.groupby(col, observed=True).agg(n=("is_tail", "size"), frac_r_lt_0_9=("is_tail", "mean"), frac_l1_gt_0_01=("is_dev", "mean"),
                                          median_dc=("dc", "median"))
    g.insert(0, "by", col); g.index = g.index.astype(str); st.append(g)
st = pd.concat(st); st.to_csv(a.out + "/tail_strata.tsv", sep="\t")
with PD(): say(st.to_string())

# ------------------------------------------------------------------ B. sample emulation
S = Sample(a.sample_dir); q = S.q[S.q.ms_level == 2]
ext = {"P_emul": S.extract(2, POFF, TOL, "max"),
       "DNN_emul": S.extract(2, lambda mz: dnn_ctr(mz), TOL, "max", strict=True),
       "DNN_emul_inclusive": S.extract(2, lambda mz: dnn_ctr(mz), TOL, "max", strict=False)}
# float32 emulation of DIA-NN's window: query_mz, margin and peak m/z in float32, strict bounds
cc = S.c[S.c.ms_level == 2]
tm = cc.target_mz.to_numpy()
qmz = np.float32(np.float32(tm) + np.float32(t0 * tm * tm + t1 + t2 * tm))
qmz = (np.float32(tm) + np.float32(np.float32(t0) * np.float32(tm) * np.float32(tm) + np.float32(t1) + np.float32(t2) * np.float32(tm))).astype(np.float32)
marg = (qmz * np.float32(TOL * 1e-6)).astype(np.float32)
pk = cc.mz.to_numpy().astype(np.float32)
ok32 = (pk > (qmz - marg)) & (pk < (qmz + marg))
g32 = cc[ok32].groupby(["qid", "pos"]).intensity.max()
ext["DNN_emul_f32"] = S.dense(g32, 2)
# exported traces on the sample
want = list(q.precursor_id.unique())
P = pq.read_table(R + "/odiaP.parquet", columns=["precursor_id", "ms_level", "annotation", "cycle_begin", "intensity"],
                  filters=[("precursor_id", "in", want)]).to_pandas()
P = P[P.ms_level == 2]; Pe = {(p, an): (cb, np.frombuffer(b, "<f4").astype(float)) for p, _, an, cb, b in P.itertuples(index=False)}
De = {(p, an): (np.asarray(rt, float), np.asarray(y, float)) for p, l, an, mz_, rt, y in
      d[(d.ms_level == 2) & d.precursor_id.isin(want)][["precursor_id", "ms_level", "annotation", "extraction_mz", "rt", "intensity"]].itertuples(index=False)}
acq = {}
ncyc = axes[axes.lane == -1].shape[0]
for w, g in lanes.groupby("lane"):
    t = np.full(ncyc, np.nan); t[g.cycle.to_numpy()] = g.rt.to_numpy(); acq[w] = t

def metrics(x, y):
    ok = ~np.isnan(x) & ~np.isnan(y)
    if ok.sum() < 5: return (np.nan, np.nan)
    x, y = x[ok], y[ok]; sx, sy = x.sum(), y.sum()
    r = np.corrcoef(x, y)[0, 1] if x.std() > 0 and y.std() > 0 else (1.0 if np.allclose(x, y) else np.nan)
    l1 = np.abs(x / sx - y / sy).sum() if sx > 0 and sy > 0 else (0.0 if sx == sy else np.nan)
    return r, l1

rows = []; keep = {}
for qid, p, an, lane, c0, c1, tmz in q[["qid", "precursor_id", "annotation", "lane", "c0", "c1", "target_mz"]].itertuples(index=False):
    if (p, an) not in De or (p, an) not in Pe: continue
    n = c1 - c0; ref_t = acq[lane][c0:c1]
    rt, y = De[(p, an)]; k = rt > 0; rt, y = rt[k], y[k]
    dn = np.full(n, np.nan)
    for t_, v in zip(rt, y):
        j = np.nanargmin(np.abs(ref_t - t_))
        if abs(ref_t[j] - t_) < 0.05: dn[j] = v
    cb, py = Pe[(p, an)]; pe = np.full(n, np.nan)
    lo = max(cb, c0); hi = min(cb + len(py), c1)
    if hi > lo: pe[lo - c0:hi - c0] = py[lo - cb:hi - cb]
    row = {"qid": qid, "precursor_id": p, "annotation": an, "target_mz": tmz, "dc": dnn_ctr(tmz) - POFF}
    row["r_P_D"], row["l1_P_D"] = metrics(pe, dn)
    row["r_Pemul_P"], row["l1_Pemul_P"] = metrics(np.where(np.isnan(pe), np.nan, ext["P_emul"][qid]), pe)
    for kx in ("DNN_emul", "DNN_emul_inclusive", "DNN_emul_f32"):
        row[f"r_{kx}_D"], row[f"l1_{kx}_D"] = metrics(np.where(np.isnan(dn), np.nan, ext[kx][qid]), dn)
    row["r_Pemul_DNNemul"], row["l1_Pemul_DNNemul"] = metrics(np.where(np.isnan(dn), np.nan, ext["P_emul"][qid]),
                                                              np.where(np.isnan(dn), np.nan, ext["DNN_emul_f32"][qid]))
    rows.append(row); keep[qid] = (pe, dn)
B = pd.DataFrame(rows); B.to_csv(a.out + "/sample_traces.tsv", sep="\t", index=False)
def frac(col_r, col_l): return f"r<0.9 {(B[col_r] < 0.9).mean():.4f}, r<0.99 {(B[col_r] < 0.99).mean():.4f}, l1>0.01 {(B[col_l] > 0.01).mean():.4f}"
say(f"B. sample: {len(B)} fragment traces with ours P and DIA-NN")
say("   ours P export vs DIA-NN export:      ", frac("r_P_D", "l1_P_D"))
say("   P emulation vs P export (sanity):    ", frac("r_Pemul_P", "l1_Pemul_P"))
say("   P emulation vs DNN emulation (f32):  ", frac("r_Pemul_DNNemul", "l1_Pemul_DNNemul"))
for kx in ("DNN_emul", "DNN_emul_inclusive", "DNN_emul_f32"):
    say(f"   {kx:22s} vs DIA-NN export:", frac(f"r_{kx}_D", f"l1_{kx}_D"))
tail = B[(B.r_P_D < 0.9) | (B.l1_P_D > 0.01)]
fixed = (tail.l1_DNN_emul_f32_D <= 0.01) & (tail.r_DNN_emul_f32_D >= 0.9)
say(f"   P-vs-DIA-NN deviating traces (r<0.9 or l1>0.01): {len(tail)}; reproduced by DIA-NN's transform: {fixed.mean():.4f}")
res_ = tail[~fixed]

# residual: where DNN emulation still differs, classify the differing points
cls = []
cc2 = cc.assign(dev_dnn=cc.ppm - dnn_ctr(cc.target_mz))
for qid in res_.qid:
    pe, dn = keep[qid]; em = ext["DNN_emul_f32"][qid]
    bad = np.nonzero(~np.isnan(dn) & (np.abs(em - dn) > 1e-3 * np.maximum(1, np.maximum(em, dn))))[0]
    g = cc2[cc2.qid == qid]
    c0 = int(q.set_index("qid").c0[qid])
    for j in bad:
        cen = g[g.pos == c0 + j]
        hit = cen[np.isclose(cen.intensity, dn[j], rtol=1e-4)]
        if dn[j] == 0: kind = "DIA-NN 0, we have a centroid"
        elif len(hit):
            e = float(np.abs(hit.dev_dnn).min())
            kind = "DIA-NN value is a centroid at the window edge (|dev|-tol < 0.2 ppm)" if abs(e - TOL) < 0.2 else \
                   ("DIA-NN value is a centroid inside our emulated window" if e < TOL else "DIA-NN value is a centroid outside its window")
        else: kind = "DIA-NN value matches no centroid at this spectrum"
        edge = np.abs(np.abs(cen.dev_dnn) - TOL).min() if len(cen) else np.nan
        cls.append((qid, j, kind, edge, dn[j], em[j]))
cls = pd.DataFrame(cls, columns=["qid", "pos", "kind", "closest_edge_ppm", "dnn", "emul"])
cls.to_csv(a.out + "/residual_points.tsv", sep="\t", index=False)
say(f"   residual traces: {len(res_)}; differing points: {len(cls)}")
if len(cls): say(cls.kind.value_counts().to_string()); say(f"   closest centroid distance to the window edge (ppm), median {cls.closest_edge_ppm.median():.3f}")

# ------------------------------------------------------------------ C. examples
# representative (not the most extreme, near-empty) cases: strongest DIA-NN signal among (i) r in [0.3, 0.9)
# and (ii) r >= 0.9 with l1 in (0.02, 0.3]; residual (unexplained) cases appended if any
tl = tail[fixed].copy(); tl["dmax"] = [np.nanmax(keep[x][1]) for x in tl.qid]
ex = pd.concat([tl[(tl.r_P_D >= 0.3) & (tl.r_P_D < 0.9)].sort_values("dmax", ascending=False).drop_duplicates("precursor_id").head(4),
                tl[(tl.r_P_D >= 0.9) & (tl.l1_P_D > 0.02) & (tl.l1_P_D <= 0.3)].sort_values("dmax", ascending=False).drop_duplicates("precursor_id").head(4),
                res_.sort_values("l1_P_D", ascending=False).drop_duplicates("precursor_id").head(2)])
fig, axs = plt.subplots(2, len(ex), figsize=(2.3 * len(ex), 4.8), gridspec_kw={"height_ratios": [1.2, 1]})
qq = q.set_index("qid")
for j, r in enumerate(ex.itertuples()):
    pe, dn = keep[r.qid]; m = ~np.isnan(dn); idx = np.nonzero(m)[0]
    lo, hi = idx.min(), idx.max()
    x = np.arange(lo, hi + 1)
    ax = axs[0, j]
    ax.plot(x, pe[lo:hi + 1], c="#023e8a", lw=1, label="ours P"); ax.plot(x, -dn[lo:hi + 1], c="#8ecae6", lw=1.4, label="DIA-NN (mirrored)")
    ax.axhline(0, c="k", lw=0.4); ax.set_yticks([]); ax.tick_params(labelsize=6)
    tag = "explained" if r.qid in tail[fixed].qid.values else "residual"
    ax.set_title(f"{r.precursor_id[:18]}\n{r.annotation} {r.target_mz:.1f}  r {r.r_P_D:.2f} [{tag}]", fontsize=6.5)
    if j == 0: ax.legend(fontsize=5.5, loc="upper left")
    ax = axs[1, j]
    g = cc2[(cc2.qid == r.qid)]; c0 = int(qq.c0[r.qid]); g = g[(g.pos - c0 >= lo) & (g.pos - c0 <= hi)]
    ax.scatter(g.pos - c0, g.ppm, s=3 + 40 * np.sqrt(g.intensity / max(1, g.intensity.max())), c="#555")
    pc, dc_ = POFF, dnn_ctr(r.target_mz)
    ax.axhspan(pc - TOL, pc + TOL, color="#023e8a", alpha=0.10); ax.axhspan(dc_ - TOL, dc_ + TOL, color="#8ecae6", alpha=0.25)
    for v in (pc - TOL, pc + TOL): ax.axhline(v, c="#023e8a", lw=0.6)
    for v in (dc_ - TOL, dc_ + TOL): ax.axhline(v, c="#219ebc", lw=0.6, ls="--")
    ax.set_ylim(min(pc, dc_) - TOL - 4, max(pc, dc_) + TOL + 4); ax.tick_params(labelsize=6)
    ax.set_xlabel("cycle", fontsize=6)
    if j == 0: ax.set_ylabel("centroid error vs library (ppm)\nbands: ours P (dark), DIA-NN (light, dashed)", fontsize=6)
fig.suptitle("MS2 tail examples, ours P vs DIA-NN 1.7.12 (run sample): 'explained' = reproduced by emulating DIA-NN's quadratic transform", fontsize=8)
fig.tight_layout()
for e in ("png", "pdf"): fig.savefig(f"{a.fig}.{e}", dpi=180)
say("done")
