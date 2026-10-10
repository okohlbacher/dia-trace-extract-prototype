#!/usr/bin/env python3
"""Trace gallery: OpenSWATH, DIA-NN 1.7.12 and DIATraceExtractor (ours A = DIA-NN rules, ours B = OpenSWATH
rules) on a shared RT axis, grouped by mode of divergence between the engines.

Input: per-precursor features from scripts/trace_gallery_features.py (top library fragment + MS1 M+0).
Modes (first matching rule wins; thresholds anchored on the data: the B-vs-OpenSWATH re-implementation
noise floor is l1 <= 0.05 and |log2 ratio| <= 0.03 at the 99th percentile, so "agree" allows 2x that):
  identical    all agree: MS2 DIA-NN~OSW (r >= 0.99, l1 <= 0.1, |log2| <= 0.1), MS1 A~DIA-NN (l1 <= 0.1),
               DIA-NN XIC fully inside the +-60 s window (>= 50 common points)
  low_signal   DIA-NN apex of the top fragment in the run's bottom decile and MS2 r(DIA-NN, OSW) < 0.95
  shape_osw    MS2 peak shape differs (r < 0.9 or l1 > 0.5), OpenSWATH/B has more signal (log2 DNN/OSW < 0)
  shape_dnn    MS2 peak shape differs, DIA-NN/A has more signal (log2 >= 0)
  scale        MS2 same shape (r >= 0.98) but |log2 DNN/OSW| > 0.2 (sum vs max of centroids in the window)
  truncated    DIA-NN's XIC (60 scans around its own apex) overlaps the +-60 s window by < 50 points
  ms1_only     MS2 fine, MS1 A vs DIA-NN differs (l1 > 0.3 or r < 0.9; MS1 tolerance, see ms1_divergence.md)
  minor        everything else (small differences in MS2 and/or MS1)
A K-means clustering of the same features (k = 8) is cross-tabulated against the modes as a check.

Panels: top = top library fragment, bottom = MS1 M+0; each trace normalised to its own maximum inside DIA-NN's
XIC span (grey band; DIA-NN exports only 60 scans around its apex), values above 1.1 clipped.
OSW thick light orange, ours B thin dark orange dashed; DIA-NN thick light blue, ours A thin dark blue dashed
(pairs that agree overlap). Times are true acquisition times (MS2: the window's spectra; MS1: DIA-NN and
OSW at MS1 scans, ours on the global cycle axis).

usage: trace_gallery.py RUN_DIR FEATURES.parquet FIG_DIR OUT_PREFIX [--n 50] [--seed 20261010] [--per_mode 8]
"""
import argparse, os
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"): os.environ.setdefault(_v, "16")  # shared node
import numpy as np, pandas as pd, pyarrow.parquet as pq
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from sklearn.preprocessing import StandardScaler
from sklearn.cluster import KMeans

ap = argparse.ArgumentParser()
ap.add_argument("run_dir"); ap.add_argument("features"); ap.add_argument("fig_dir"); ap.add_argument("out_prefix")
ap.add_argument("--n", type=int, default=50); ap.add_argument("--seed", type=int, default=20261010)
ap.add_argument("--per_mode", type=int, default=8)
a = ap.parse_args()
R = a.run_dir
os.makedirs(a.fig_dir, exist_ok=True)

MODES = [("identical", "All engines agree"), ("low_signal", "Low signal / noisy"),
         ("shape_osw", "MS2 shape differs, OSW higher"), ("shape_dnn", "MS2 shape differs, DIA-NN higher"),
         ("scale", "MS2 scale-only (sum vs max)"), ("truncated", "Truncated common span (DIA-NN XIC off-centre)"),
         ("ms1_only", "MS1-only divergence (ours A vs DIA-NN)"), ("minor", "Minor differences")]
NAME = dict(MODES)
COLOR = dict(zip(NAME, ["#2a9d8f", "#8d8d8d", "#e76f51", "#264653", "#e9c46a", "#9b5de5", "#f15bb5", "#6c757d"]))

f = pd.read_parquet(a.features)
f = f[f.in_all].reset_index(drop=True)
low_int = np.percentile(f.int2, 10)

def mode(x):
    if (x.r2_do >= 0.99 and x.l1_2_do <= 0.1 and abs(x.lr2_do) <= 0.1 and x.l1_1_ad <= 0.1 and x.pts2_do >= 50):
        return "identical"
    if x.int2 < low_int and x.r2_do < 0.95: return "low_signal"
    if x.r2_do < 0.9 or x.l1_2_do > 0.5: return "shape_osw" if x.lr2_do < 0 else "shape_dnn"
    if x.r2_do >= 0.98 and abs(x.lr2_do) > 0.2: return "scale"
    if x.pts2_do < 50: return "truncated"
    if x.l1_1_ad > 0.3 or x.r1_ad < 0.9: return "ms1_only"
    return "minor"
f["mode"] = [mode(x) for x in f.itertuples()]

# data-driven check: K-means on the same features
X = pd.DataFrame({"l1_2_do": f.l1_2_do, "lr2_do": f.lr2_do.clip(-3, 3), "dt2": np.log1p(f.dt2_do.abs()),
                  "l1_1_ad": f.l1_1_ad, "lr1_ad": f.lr1_ad.clip(-3, 3), "l1_1_do": f.l1_1_do, "int2": f.int2,
                  "pts2_do": f.pts2_do})
f["kmeans"] = KMeans(8, n_init=10, random_state=0).fit_predict(StandardScaler().fit_transform(X))
freq = f["mode"].value_counts().reindex(NAME).rename("precursors").to_frame()
freq["fraction"] = freq.precursors / len(f); freq.insert(0, "name", [NAME[m] for m in freq.index])

# samples: random (fixed seed) and stratified (up to per_mode per mode, not already in the random sample)
rng = np.random.default_rng(a.seed)
rand = f.iloc[np.sort(rng.choice(len(f), a.n, replace=False))].copy(); rand["sample"] = "random"
strat = []
for m in NAME:
    pool = f[(f["mode"] == m) & ~f.precursor_id.isin(rand.precursor_id)]
    k = min(a.per_mode, len(pool))
    if k: strat.append(pool.iloc[np.sort(rng.choice(len(pool), k, replace=False))])
strat = pd.concat(strat).copy(); strat["sample"] = "stratified"
freq["in_random_50"] = rand["mode"].value_counts().reindex(NAME).fillna(0).astype(int)
sel = pd.concat([rand, strat])
f.drop(columns=[c for c in f.columns if c.startswith("pts1")]).to_csv(a.out_prefix + "_modes.tsv", sep="\t", index=False)
sel[["sample", "precursor_id", "frag", "mode", "kmeans", "precursor_mz", "precursor_charge", "rt_center", "r2_do",
     "l1_2_do", "lr2_do", "dt2_do", "pts2_do", "r2_ad", "r1_ad", "l1_1_ad", "lr1_ad", "r1_do", "int2", "int1"]].to_csv(
    a.out_prefix + "_selection.tsv", sep="\t", index=False)
freq.to_csv(a.out_prefix + "_mode_frequencies.tsv", sep="\t")
ct = pd.crosstab(f["mode"], f["kmeans"]).reindex(NAME)
ct.to_csv(a.out_prefix + "_mode_vs_kmeans.tsv", sep="\t")
with pd.option_context("display.width", 200): print(freq); print(ct)

# ---------------------------------------------------------------- traces of the selected precursors
want = set(sel.precursor_id)
frag_of = dict(zip(sel.precursor_id, sel.frag))
axes = pq.read_table(R + "/odiaA.axes.parquet").to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
acq = {}
for w, g in axes[axes.lane >= 0].groupby("lane"):
    t = np.full(len(cycle_rt), np.nan); t[g.cycle.to_numpy()] = g.rt.to_numpy(); acq[w] = t
UNI = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNI.items(): s = s.replace(k, v)
    return s
def ms1_axis_rt(odia_path, axes, cycle_rt):
    """Times of MS1 rows: the MS1 scans (axes lane -2) when the traces say odia.ms1_axis = ms1_scans, else the cycle axis."""
    import glob as _glob, os as _os, pyarrow.parquet as _pq
    first = sorted(_glob.glob(_os.path.join(odia_path, "*.parquet")))[0] if _os.path.isdir(odia_path) else odia_path
    md = _pq.read_schema(first).metadata or {}   # (pyarrow.dataset drops the footer metadata)
    if md.get(b"odia.ms1_axis") == b"ms1_scans":
        return axes[axes.lane == -2].sort_values("cycle").rt.to_numpy()
    return cycle_rt
tr = {}   # (pid, level) -> {engine: (t, y)}
def keep(pid, lvl, ann): return pid in want and ((lvl == 1 and ann == "M+0") or (lvl == 2 and ann == frag_of[pid]))
for eng, path in (("A", R + "/odiaA.parquet"), ("B", R + "/odiaB.parquet")):
    ms1_rt = ms1_axis_rt(path, axes, cycle_rt)
    t = pq.read_table(path, columns=["precursor_id", "ms_level", "window", "annotation", "cycle_begin", "intensity"],
                      filters=[("precursor_id", "in", list(want))]).to_pandas()
    for pid, lvl, win, ann, cb, blob in t.itertuples(index=False):
        if not keep(pid, lvl, ann): continue
        y = np.frombuffer(blob, "<f4").astype(float)
        x = acq[win][cb:cb + len(y)] if lvl == 2 else ms1_rt[cb:cb + len(y)]
        ok = ~np.isnan(x); tr.setdefault((pid, lvl), {})[eng] = (x[ok], y[ok])
d = pq.read_table(R + "/diann/diann_xic.parquet", columns=["precursor_id", "ms_level", "annotation", "rt", "intensity"]).to_pandas()
d["precursor_id"] = d.precursor_id.map(from_unimod)
for pid, lvl, ann, rt, y in d[d.precursor_id.isin(want)].itertuples(index=False):
    if not keep(pid, lvl, ann): continue
    rt, y = np.asarray(rt, float), np.asarray(y, float); k = rt > 0
    tr.setdefault((pid, lvl), {})["DIA-NN"] = (rt[k], y[k])
s = pq.read_table(R + "/osw_traces.parquet", filters=[("precursor_id", "in", list(want))]).to_pandas()
for pid, lvl, ann, rtb, yb in s.itertuples(index=False):
    if not keep(pid, lvl, ann): continue
    rt, y = np.frombuffer(rtb, "<f4").astype(float), np.frombuffer(yb, "<f4").astype(float)
    o = np.argsort(rt, kind="stable"); tr.setdefault((pid, lvl), {})["OSW"] = (rt[o], y[o])

STYLE = {"OSW": dict(color="#f4a261", lw=2.6, alpha=0.75, zorder=1),
         "B": dict(color="#9c4a00", lw=0.8, ls=(0, (3, 1.5)), zorder=3),
         "DIA-NN": dict(color="#8ecae6", lw=2.6, alpha=0.9, zorder=2),
         "A": dict(color="#023e8a", lw=0.8, ls=(0, (1.5, 1)), zorder=4)}
LABEL = {"OSW": "OpenSWATH", "B": "ours B (OSW rules)", "DIA-NN": "DIA-NN 1.7.12", "A": "ours A (DIA-NN rules)"}

def draw(cell, x):
    """cell: (ax_ms2, ax_ms1); x: selection row."""
    for ax, lvl in zip(cell, (2, 1)):
        t = tr.get((x.precursor_id, lvl), {})
        span = (t["DIA-NN"][0].min(), t["DIA-NN"][0].max()) if "DIA-NN" in t and len(t["DIA-NN"][0]) else None
        if span: ax.axvspan(span[0] / 60, span[1] / 60, color="#f1f1f1", zorder=0, lw=0)
        for eng in ("OSW", "DIA-NN", "B", "A"):
            if eng in t and len(t[eng][1]):
                xx, yy = t[eng]
                inside = (xx >= span[0]) & (xx <= span[1]) if span else np.ones(len(xx), bool)
                m = yy[inside].max() if inside.any() else yy.max()
                ax.plot(xx / 60, np.minimum(yy / m, 1.1) if m > 0 else yy, **STYLE[eng])
        ax.set_ylim(-0.05, 1.12); ax.set_yticks([]); ax.tick_params(labelsize=5, length=2, pad=1)
        ax.set_xlim((x.rt_center - 62) / 60, (x.rt_center + 62) / 60)
        for sp in ("top", "right"): ax.spines[sp].set_visible(False)
    cell[0].set_xticklabels([])
    pid = x.precursor_id.replace("(Carbamidomethyl)", "[CAM]").replace("(Oxidation)", "[Ox]")
    tag = "R" if x.sample == "random" else "S"
    cell[0].set_title(f"{tag} {pid[:24]}", fontsize=5.5, pad=1.5, color=COLOR[x.mode], loc="left")
    cell[0].text(0.99, 0.97, f"{x.frag}  r {x.r2_do:.2f}  log2 {x.lr2_do:+.2f}", transform=cell[0].transAxes,
                 fontsize=4.6, ha="right", va="top")
    cell[1].text(0.99, 0.95, f"MS1  rA/D {x.r1_ad:.2f}  l1 {x.l1_1_ad:.2f}", transform=cell[1].transAxes,
                 fontsize=4.6, ha="right", va="top")

def grid(rows_df, title, path, ncol=10, headers=True):
    """rows_df ordered; a new figure row starts at each mode when headers."""
    layout = []  # list of rows; each row a list of selection rows or a header string
    if headers:
        for m in NAME:
            g = rows_df[rows_df["mode"] == m]
            if not len(g): continue
            layout.append(f"{NAME[m]}  ({len(g)})|{m}")
            for i in range(0, len(g), ncol): layout.append(list(g.iloc[i:i + ncol].itertuples()))
    else:
        for i in range(0, len(rows_df), ncol): layout.append(list(rows_df.iloc[i:i + ncol].itertuples()))
    hr = [0.18 if isinstance(r, str) else 1.0 for r in layout]
    fig = plt.figure(figsize=(ncol * 1.55, sum(h * 1.25 for h in hr) + 0.75))
    outer = fig.add_gridspec(len(layout), 1, height_ratios=hr, hspace=0.35, top=1 - 0.62 / fig.get_figheight(),
                             bottom=0.25 / fig.get_figheight(), left=0.01, right=0.995)
    for i, r in enumerate(layout):
        if isinstance(r, str):
            txt, m = r.split("|")
            ax = fig.add_subplot(outer[i]); ax.axis("off")
            ax.text(0, 0.1, txt, fontsize=8, fontweight="bold", color=COLOR[m], va="bottom")
            continue
        inner = outer[i].subgridspec(2, ncol, height_ratios=[1.7, 1], hspace=0.08, wspace=0.08)
        for j, x in enumerate(r):
            draw((fig.add_subplot(inner[0, j]), fig.add_subplot(inner[1, j])), x)
    handles = [Line2D([], [], **{k: v for k, v in STYLE[e].items() if k != "zorder"}, label=LABEL[e])
               for e in ("OSW", "B", "DIA-NN", "A")]
    fig.legend(handles=handles, loc="upper left", ncol=5, fontsize=7, frameon=False,
               bbox_to_anchor=(0.005, 1 - 0.26 / fig.get_figheight()))
    fig.text(0.01, 1 - 0.12 / fig.get_figheight(), title, fontsize=9, va="top", fontweight="bold")
    for ext in ("png", "pdf"): fig.savefig(f"{path}.{ext}", dpi=200)
    plt.close(fig)

order = {m: i for i, m in enumerate(NAME)}
r_sorted = rand.assign(o=rand["mode"].map(order)).sort_values(["o", "precursor_id"])
grid(r_sorted, f"Run 01: {a.n} random precursors (seed {a.seed}) by divergence mode. Top: top library fragment, bottom: MS1 M+0 (RT min; per-trace max in grey DIA-NN span = 1)", os.path.join(a.fig_dir, "trace_gallery_by_mode"))
s_sorted = sel[sel["sample"] == "stratified"].assign(o=lambda z: z["mode"].map(order)).sort_values(["o", "precursor_id"])
grid(s_sorted, f"Run 01: STRATIFIED sample (up to {a.per_mode} per mode, seed {a.seed}; not in the random 50)",
     os.path.join(a.fig_dir, "trace_gallery_stratified"))
for m in NAME:
    g = sel[sel["mode"] == m].sort_values(["sample", "precursor_id"])
    if len(g):
        grid(g, f"{NAME[m]}: {len(f[f['mode'] == m])} of {len(f)} precursors ({(f['mode'] == m).mean():.1%}). "
             "R = random sample, S = stratified sample", os.path.join(a.fig_dir, f"trace_gallery_mode_{m}"),
             ncol=8, headers=False)
print("figures ->", a.fig_dir)
