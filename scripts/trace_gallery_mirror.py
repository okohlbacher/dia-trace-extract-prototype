#!/usr/bin/env python3
"""Mirrored pairwise trace gallery: for each selected precursor three pair panels,
  ours A (DIA-NN rules) vs DIA-NN 1.7.12 | ours B (OpenSWATH rules) vs OpenSWATH | OpenSWATH vs DIA-NN,
first engine upward, second mirrored downward; top library fragment above, MS1 M+0 below.

Uses the selection and modes written by scripts/trace_gallery.py (same random 50 / stratified sample, same
divergence modes); traces are normalised to their maximum inside DIA-NN's exported span (grey band), as in
trace_gallery.py; times are true acquisition times.

usage: trace_gallery_mirror.py RUN_DIR SELECTION.tsv FIG_DIR [--sample random|stratified] [--per_row 4]
"""
import argparse, os
import numpy as np, pandas as pd, pyarrow.parquet as pq
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("run_dir"); ap.add_argument("selection"); ap.add_argument("fig_dir")
ap.add_argument("--sample", default="random"); ap.add_argument("--per_row", type=int, default=4)
a = ap.parse_args()
R = a.run_dir
os.makedirs(a.fig_dir, exist_ok=True)

MODES = [("identical", "All engines agree"), ("low_signal", "Low signal / noisy"),
         ("shape_osw", "MS2 shape differs, OSW higher"), ("shape_dnn", "MS2 shape differs, DIA-NN higher"),
         ("scale", "MS2 scale-only (sum vs max)"), ("truncated", "Truncated common span (DIA-NN XIC off-centre)"),
         ("ms1_only", "MS1-only divergence (ours A vs DIA-NN)"), ("minor", "Minor differences")]
NAME = dict(MODES)
COLOR = dict(zip(NAME, ["#2a9d8f", "#8d8d8d", "#e76f51", "#264653", "#e9c46a", "#9b5de5", "#f15bb5", "#6c757d"]))
ENG = {"A": ("ours A", "#023e8a"), "B": ("ours B", "#9c4a00"), "DIA-NN": ("DIA-NN", "#2a9fd6"), "OSW": ("OSW", "#f4a261")}
PAIRS = [("A", "DIA-NN"), ("B", "OSW"), ("OSW", "DIA-NN")]

sel = pd.read_csv(a.selection, sep="\t")
sel = sel[sel["sample"] == a.sample].copy()
order = {m: i for i, m in enumerate(NAME)}
sel = sel.assign(o=sel["mode"].map(order)).sort_values(["o", "precursor_id"])

# ---------------------------------------------------------------- traces (as in trace_gallery.py)
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
def keep(pid, lvl, ann): return pid in want and ((lvl == 1 and ann == "M+0") or (lvl == 2 and ann == frag_of[pid]))
def ms1_axis_rt(odia_path, axes, cycle_rt):
    """Times of MS1 rows: the MS1 scans (axes lane -2) when the traces say odia.ms1_axis = ms1_scans, else the cycle axis."""
    import glob as _glob, os as _os, pyarrow.parquet as _pq
    first = sorted(_glob.glob(_os.path.join(odia_path, "*.parquet")))[0] if _os.path.isdir(odia_path) else odia_path
    md = _pq.read_schema(first).metadata or {}   # (pyarrow.dataset drops the footer metadata)
    if md.get(b"odia.ms1_axis") == b"ms1_scans":
        return axes[axes.lane == -2].sort_values("cycle").rt.to_numpy()
    return cycle_rt
tr = {}
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

def corr_on_common(up, dn):
    """Pearson r of two traces on the up-trace's times (down trace interpolated), within both spans."""
    (x1, y1), (x2, y2) = up, dn
    if len(x1) < 3 or len(x2) < 3: return np.nan
    m = (x1 >= x2.min()) & (x1 <= x2.max())
    if m.sum() < 3: return np.nan
    a1, a2 = y1[m], np.interp(x1[m], x2, y2)
    if a1.std() == 0 or a2.std() == 0: return np.nan
    return float(np.corrcoef(a1, a2)[0, 1])

def mirror(ax, x, lvl, up, dn):
    t = tr.get((x.precursor_id, lvl), {})
    span = (t["DIA-NN"][0].min(), t["DIA-NN"][0].max()) if "DIA-NN" in t and len(t["DIA-NN"][0]) else None
    if span: ax.axvspan(span[0] / 60, span[1] / 60, color="#f1f1f1", zorder=0, lw=0)
    for eng, sign in ((up, 1.0), (dn, -1.0)):
        if eng not in t or not len(t[eng][1]): continue
        xx, yy = t[eng]
        inside = (xx >= span[0]) & (xx <= span[1]) if span else np.ones(len(xx), bool)
        m = yy[inside].max() if inside.any() else yy.max()
        yn = np.minimum(yy / m, 1.1) if m > 0 else yy
        ax.fill_between(xx / 60, 0, sign * yn, color=ENG[eng][1], alpha=0.25, lw=0, zorder=1)
        ax.plot(xx / 60, sign * yn, color=ENG[eng][1], lw=0.7, zorder=2)
    ax.axhline(0, color="#444", lw=0.4, zorder=3)
    if up in t and dn in t:
        r = corr_on_common(t[up], t[dn])
        if np.isfinite(r): ax.text(0.98, 0.95, f"r {r:.2f}", transform=ax.transAxes, fontsize=4.5, ha="right", va="top")
    ax.set_ylim(-1.15, 1.15); ax.set_yticks([]); ax.tick_params(labelsize=4.5, length=1.5, pad=1)
    ax.set_xlim((x.rt_center - 62) / 60, (x.rt_center + 62) / 60)
    for sp in ("top", "right"): ax.spines[sp].set_visible(False)

def figure(rows, title, path):
    per = a.per_row
    layout = []
    for m in NAME:
        g = rows[rows["mode"] == m]
        if not len(g): continue
        layout.append(f"{NAME[m]}  ({len(g)})|{m}")
        for i in range(0, len(g), per): layout.append(list(g.iloc[i:i + per].itertuples()))
    hr = [0.16 if isinstance(r, str) else 1.0 for r in layout]
    fig = plt.figure(figsize=(per * 3 * 1.25 + 0.3, sum(h * 1.6 for h in hr) + 0.9))
    outer = fig.add_gridspec(len(layout), 1, height_ratios=hr, hspace=0.45, top=1 - 0.75 / fig.get_figheight(),
                             bottom=0.2 / fig.get_figheight(), left=0.01, right=0.995)
    for i, r in enumerate(layout):
        if isinstance(r, str):
            txt, m = r.split("|")
            ax = fig.add_subplot(outer[i]); ax.axis("off")
            ax.text(0, 0.1, txt, fontsize=8, fontweight="bold", color=COLOR[m], va="bottom")
            continue
        cells = outer[i].subgridspec(1, per, wspace=0.12)
        for j, x in enumerate(r):
            inner = cells[j].subgridspec(2, 3, height_ratios=[1.6, 1], hspace=0.1, wspace=0.06)
            for k, (up, dn) in enumerate(PAIRS):
                a2, a1 = fig.add_subplot(inner[0, k]), fig.add_subplot(inner[1, k])
                mirror(a2, x, 2, up, dn); mirror(a1, x, 1, up, dn)
                a2.set_xticklabels([])
                if k == 0:
                    a2.text(0.02, 0.97, x.frag, transform=a2.transAxes, fontsize=4.5, va="top")
                    a1.text(0.02, 0.97, "MS1", transform=a1.transAxes, fontsize=4.5, va="top")
                if i == 1 or True:
                    a2.set_title(f"{ENG[up][0]} / {ENG[dn][0]}", fontsize=4.5, pad=1)
            pid = x.precursor_id.replace("(Carbamidomethyl)", "[CAM]").replace("(Oxidation)", "[Ox]")
            fig.add_subplot(cells[j]).axis("off")
            fig.axes[-1].set_title(pid[:30], fontsize=5.5, pad=9, color=COLOR[x.mode], loc="left")
    fig.text(0.01, 1 - 0.12 / fig.get_figheight(), title, fontsize=9, va="top", fontweight="bold")
    fig.text(0.01, 1 - 0.40 / fig.get_figheight(),
             "Each precursor: three pair panels, first engine up, second mirrored down; top: top library fragment, "
             "bottom: MS1 M+0; RT in min; each trace scaled to its max inside DIA-NN's span (grey); r = Pearson on common span.  "
             "ours A = DIA-NN rules (max, mass-corrected), ours B = OpenSWATH rules (sum, unit isotope spacing).",
             fontsize=6.5, va="top")
    for name, (lab, col) in ENG.items():
        pass
    for ext in ("png", "pdf"): fig.savefig(f"{path}.{ext}", dpi=200)
    plt.close(fig)

tag = f"random {len(sel)}" if a.sample == "random" else "stratified sample"
figure(sel, f"Run 01, {tag}: mirrored pairwise XICs by divergence mode",
       os.path.join(a.fig_dir, f"trace_mirror_{a.sample}"))
print("->", os.path.join(a.fig_dir, f"trace_mirror_{a.sample}.png"))
