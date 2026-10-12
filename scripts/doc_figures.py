#!/usr/bin/env python3
"""Figures for docs/EXTRACTION.md.

usage: doc_figures.py MZML TRACES_DIR AXES_PARQUET ANCHORS_TSV PRECURSOR_ID OUT_DIR
       doc_figures.py --overview OUT_DIR      (the overview figure only; needs no data)

  MZML          the run (indexed mzML; only the spectra around the example are read)
  TRACES_DIR    DIATraceExtractor output (Parquet directory) that contains PRECURSOR_ID
  AXES_PARQUET  its axes file
  ANCHORS_TSV   -out_calibration of a run with -mass_calibration auto
Writes overview, run_grid, extraction_example, calibration and parallel figures (PNG) to OUT_DIR.
"""
import base64, os, re, sys, zlib
import numpy as np, pandas as pd
import pyarrow.dataset as ds, pyarrow.parquet as pq
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle

INK, MUTED, ACCENT, ACCENT2, SOFT = "#1d2433", "#6b7280", "#1f6feb", "#d9480f", "#e8eef9"
plt.rcParams.update({"font.size": 9, "axes.spines.top": False, "axes.spines.right": False, "axes.edgecolor": MUTED})


def box(ax, x, y, w, h, text, fc=SOFT, ec=ACCENT, fs=9, bold=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.03", fc=fc, ec=ec, lw=1.2))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=fs, color=INK, weight="bold" if bold else None, wrap=True)


def arrow(ax, x0, y0, x1, y1, color=MUTED, ls="-"):
    ax.annotate("", xy=(x1, y1), xytext=(x0, y0), arrowprops=dict(arrowstyle="-|>", color=color, lw=1.3, ls=ls))


# ---------------------------------------------------------------- 1. overview
def overview(out):
    """Data flow: library and run in, calibration from the run, extraction, traces out. No line crosses a box."""
    fig, ax = plt.subplots(figsize=(10, 3.6)); ax.set_xlim(0, 10); ax.set_ylim(0, 3.8); ax.axis("off")
    box(ax, 0.1, 2.0, 2.0, 0.8, "Spectral library\n(predicted, RT-tuned)")
    box(ax, 0.1, 0.4, 2.0, 0.8, "DIA run\n(centroided mzML / mzPeak)")
    box(ax, 2.9, 2.0, 2.0, 0.8, "Calibration\nfrom the run itself")
    box(ax, 2.9, 0.4, 2.0, 0.8, "Run grid\ncycles × isolation windows")
    box(ax, 5.8, 1.2, 2.2, 0.8, "Extraction\nfragment + MS1 isotope traces", fc="#dbe7ff", bold=True)
    box(ax, 8.4, 1.2, 1.5, 0.8, "Traces\n(Parquet)", fc="#f3f4f6", ec=MUTED)
    # left column to middle column
    arrow(ax, 2.1, 2.4, 2.9, 2.4); arrow(ax, 2.1, 0.8, 2.9, 0.8)
    # the grid feeds the calibration (its spectra) and the extraction
    arrow(ax, 3.9, 1.2, 3.9, 2.0); ax.text(4.0, 1.6, "spectra", fontsize=7.5, color=MUTED, ha="left", va="center")
    arrow(ax, 4.9, 0.8, 5.8, 1.45)
    # the calibration result feeds the extraction
    arrow(ax, 4.9, 2.4, 5.8, 1.75)
    ax.text(5.45, 2.45, "RT map, mass model,\ntolerances", fontsize=7.5, color=MUTED, ha="center", va="bottom")
    # the library's precursors and fragments go to the extraction, routed above the boxes
    xs, ys = [1.1, 1.1, 6.9], [2.8, 3.45, 3.45]
    ax.plot(xs, ys, color=MUTED, lw=1.0, ls="--")
    arrow(ax, 6.9, 3.45, 6.9, 2.0, ls="--")
    ax.text(4.0, 3.5, "precursors and fragments", fontsize=7.5, color=MUTED, ha="center", va="bottom")
    # traces out
    arrow(ax, 8.0, 1.6, 8.4, 1.6)
    fig.tight_layout(); fig.savefig(os.path.join(out, "overview.png"), dpi=170); plt.close(fig)


if len(sys.argv) == 3 and sys.argv[1] == "--overview":
    os.makedirs(sys.argv[2], exist_ok=True); overview(sys.argv[2]); sys.exit(0)

mzml, traces_dir, axes_file, anchors_file, pid, out = sys.argv[1:7]
os.makedirs(out, exist_ok=True)
overview(out)

# ---------------------------------------------------------------- example precursor: our traces and axes
t = ds.dataset(traces_dir).to_table(filter=ds.field("precursor_id") == pid).to_pandas()
axes = pq.read_table(axes_file).to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
ms1_rt = axes[axes.lane == -2].sort_values("cycle").rt.to_numpy()
win = int(t.window.iloc[0])
lane = axes[axes.lane == win]
w_lo, w_hi = float(lane.window_lower.dropna().iloc[0]), float(lane.window_upper.dropna().iloc[0])
pmz, z, rtc = float(t.precursor_mz.iloc[0]), int(t.precursor_charge.iloc[0]), float(t.rt_center.iloc[0])
ms2 = t[t.ms_level == 2].sort_values("library_intensity", ascending=False).head(6).copy()
ms1 = t[(t.ms_level == 1) & (t.isotope == 0)]
meta = pq.read_schema(sorted(f for f in os.listdir(traces_dir))[0] and os.path.join(traces_dir, sorted(os.listdir(traces_dir))[0])).metadata
md = {k.decode(): v.decode() for k, v in (meta or {}).items()}
ms2_tol = float(md.get("odia.mz_tol_ppm", "6"))
ms2_off = float(md.get("odia.mz_offset_ppm", "0"))


# ---------------------------------------------------------------- mzML: the window's spectra around the example
def mzml_index(path):
    with open(path, "rb") as f:
        f.seek(0, 2); size = f.tell(); f.seek(max(0, size - (64 << 20)))
        tail = f.read().decode("latin-1")
    return [int(x) for x in re.findall(r'<offset idRef="[^"]*">(\d+)</offset>', tail.split('<index name="spectrum">')[1].split("</index>")[0])]


def read_spectrum(f, off):
    f.seek(off); buf = b""
    while b"</spectrum>" not in buf:
        chunk = f.read(1 << 16)
        if not chunk: break
        buf += chunk
    x = buf[:buf.index(b"</spectrum>")].decode("latin-1")
    rt = float(re.search(r'name="scan start time" value="([0-9.eE+-]+)"(?:[^>]*unitName="(\w+)")?', x).group(1))
    if re.search(r'name="scan start time"[^>]*unitName="minute"', x): rt *= 60
    lvl = int(re.search(r'name="ms level" value="(\d)"', x).group(1))
    tgt = re.search(r'name="isolation window target m/z" value="([0-9.]+)"', x)
    arrays = []
    for b in re.findall(r"<binaryDataArray.*?</binaryDataArray>", x, re.S):
        raw = base64.b64decode(re.search(r"<binary>(.*?)</binary>", b, re.S).group(1))
        if "MS:1000574" in b: raw = zlib.decompress(raw)
        arrays.append((("MS:1000514" in b), np.frombuffer(raw, "<f8" if "MS:1000523" in b else "<f4")))
    mz = next(a for k, a in arrays if k); it = next(a for k, a in arrays if not k)
    return rt, lvl, float(tgt.group(1)) if tgt else None, mz, it


offs = mzml_index(mzml)
spectra = []
with open(mzml, "rb") as f:
    lo, hi = 0, len(offs) - 1   # binary search on RT for the window start
    while lo < hi:
        mid = (lo + hi) // 2
        if read_spectrum(f, offs[mid])[0] < rtc - 75: lo = mid + 1
        else: hi = mid
    i = lo
    while i < len(offs):
        rt, lvl, tgt, mz, it = read_spectrum(f, offs[i])
        if rt > rtc + 75: break
        if lvl == 2 and tgt is not None and w_lo - 1e-3 <= tgt and abs(tgt - (w_lo + w_hi) / 2) < 0.6: spectra.append((rt, mz, it))
        i += 1

# ---------------------------------------------------------------- 2. extraction example (real data)
colors = plt.cm.tab10(np.arange(6))
fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 6.6), sharex=True, gridspec_kw={"height_ratios": [1.15, 1]})
for k, (_, row) in enumerate(ms2.iloc[:3].iterrows()):
    tm = row.target_mz * (1 + ms2_off * 1e-6)
    rts, ppm, ints = [], [], []
    for rt, mz, it in spectra:
        sel = np.abs(mz - tm) / tm * 1e6 <= 20
        rts += [rt] * int(sel.sum()); ppm += list((mz[sel] - tm) / tm * 1e6); ints += list(it[sel])
    if not rts: continue
    ints = np.array(ints); off = k * 50
    a1.scatter(rts, np.array(ppm) + off, s=4 + 60 * np.sqrt(ints / ints.max()), color=colors[k], alpha=0.65, lw=0)
    a1.axhspan(off - ms2_tol, off + ms2_tol, color=colors[k], alpha=0.12, lw=0)
    a1.text(rtc + 77, off, f"{row.annotation}\n{row.target_mz:.4f}", va="center", fontsize=7.5, color=colors[k])
a1.set_yticks([0, 50, 100]); a1.set_yticklabels(["", "", ""])
a1.set_ylabel(f"centroids within ±20 ppm of three fragments\n(band = measured offset ±{ms2_tol:.1f} ppm)")
a1.set_title(f"{pid[:-1]} ({z}+), m/z {pmz:.4f}: raw centroids in its isolation window ({w_lo:.1f}–{w_hi:.1f})",
             loc="left", fontsize=9.5)
for k, (_, row) in enumerate(ms2.iterrows()):
    y = np.frombuffer(row.intensity, "<f4"); x = cycle_rt[row.cycle_begin:row.cycle_begin + len(y)]
    a2.plot(x, y, color=colors[k], lw=1.4, label=row.annotation)
if len(ms1):
    r1 = ms1.iloc[0]; y1 = np.frombuffer(r1.intensity, "<f4")
    ax2 = a2.twinx(); ax2.plot(ms1_rt[r1.cycle_begin:r1.cycle_begin + len(y1)], y1, color=INK, lw=1.0, ls="--", label="MS1 M+0")
    ax2.set_ylabel("MS1 M+0 (dashed)", color=INK); ax2.spines["top"].set_visible(False)
for a in (a1, a2):
    a.axvline(rtc, color=ACCENT2, lw=1, ls=":"); a.axvspan(rtc - 60, rtc + 60, color="#f8fafc", zorder=-5)
a2.text(rtc, a2.get_ylim()[1] * 0.97, " predicted RT", color=ACCENT2, fontsize=8, va="top")
a2.set_xlabel("retention time (s)"); a2.set_ylabel("fragment intensity\n(max centroid per spectrum)")
a2.legend(fontsize=7.5, frameon=False, ncol=3, loc="upper left")
a2.set_xlim(rtc - 62, rtc + 62)
fig.tight_layout(); fig.savefig(os.path.join(out, "extraction_example.png"), dpi=170); plt.close(fig)

# ---------------------------------------------------------------- 3. run grid (schematic, real axis sizes)
n_cyc, n_win = len(cycle_rt), len(axes[axes.lane >= 0].lane.unique())
fig = plt.figure(figsize=(10, 3.8))
g = fig.add_gridspec(1, 2, width_ratios=[1.25, 1])
a = fig.add_subplot(g[0]); a.set_xlim(0, 24); a.set_ylim(0, 12)
for i in range(24):
    for j in range(12):
        a.add_patch(Rectangle((i + 0.05, j + 0.05), 0.9, 0.9, fc="#eef2f7", ec="white", lw=0.5))
for i in range(8, 17): a.add_patch(Rectangle((i + 0.05, 6.05), 0.9, 0.9, fc=ACCENT, ec="white", alpha=0.85))
a.add_patch(Rectangle((0, 6), 24, 1, fill=False, ec=ACCENT, lw=1.2))
a.annotate("one isolation window", (24, 6.5), (24.4, 9.2), fontsize=8, color=ACCENT, arrowprops=dict(arrowstyle="-", color=ACCENT))
a.annotate("± RT window around the\nprecursor's predicted RT", (12.5, 6), (13.5, 2.2), fontsize=8, color=ACCENT, ha="left",
           arrowprops=dict(arrowstyle="-", color=ACCENT))
a.set_xticks([]); a.set_yticks([]); [s.set_visible(False) for s in a.spines.values()]
a.set_xlabel(f"DIA cycles = one shared time axis ({n_cyc} cycles in run 01)")
a.set_ylabel(f"isolation windows ({n_win})")
a.set_title("Run grid: every cell holds one MS2 spectrum's centroids", loc="left", fontsize=9.5)
b = fig.add_subplot(g[1])
apex = min(spectra, key=lambda s: abs(s[0] - rtc)) if spectra else None
if apex:
    rt, mz, it = apex
    b.vlines(mz, 0, it, color="#9aa4b2", lw=0.6)
    for k, (_, row) in enumerate(ms2.iterrows()):
        tm = row.target_mz * (1 + ms2_off * 1e-6); sel = np.abs(mz - tm) / tm * 1e6 <= ms2_tol
        b.vlines(mz[sel], 0, it[sel], color=colors[k], lw=1.6)
        b.axvspan(tm * (1 - 3e-4), tm * (1 + 3e-4), color=colors[k], alpha=0.15, lw=0)
    b.set_xlabel("m/z"); b.set_ylabel("intensity"); b.set_xlim(150, min(1300, mz.max() + 20))
    b.set_title(f"One cell (the spectrum at {rt:.0f} s) and the six fragment windows\n(widened for visibility): the strongest centroid in each counts",
                loc="left", fontsize=9)
fig.tight_layout(); fig.savefig(os.path.join(out, "run_grid.png"), dpi=170); plt.close(fig)

# ---------------------------------------------------------------- 4. calibration (anchors)
an = pd.read_csv(anchors_file, sep="\t")
fig, (c1, c2) = plt.subplots(1, 2, figsize=(10, 3.8))
u = an[an.used == 1]; n_ = an[an.used == 0]
c1.scatter(u.library_rt, u.run_rt, s=5, color=ACCENT, alpha=0.6, lw=0, label=f"anchors used ({len(u)})")
c1.scatter(n_.library_rt, n_.run_rt, s=8, color=ACCENT2, marker="x", lw=0.8, label=f"rejected ({len(n_)})")
s_ = an.sort_values("library_rt"); c1.plot(s_.library_rt, s_.run_rt - s_.residual, color=INK, lw=1.4, label="RT map (LOWESS)")
c1.set_xlabel("library RT"); c1.set_ylabel("run RT (s)"); c1.legend(frameon=False, fontsize=8)
c1.set_title("Co-eluting anchors found in the run, and the fitted RT map", loc="left", fontsize=9.5)
c2.hist(u.residual, bins=60, color=ACCENT, alpha=0.8)
q95 = np.quantile(np.abs(u.residual), 0.95)
for v in (-q95, q95): c2.axvline(v, color=ACCENT2, ls="--", lw=1)
c2.set_xlabel("anchor RT minus map (s)"); c2.set_ylabel("anchors")
c2.set_title(f"Residuals: 95% within ±{q95:.0f} s", loc="left", fontsize=9.5)
fig.tight_layout(); fig.savefig(os.path.join(out, "calibration.png"), dpi=170); plt.close(fig)

# ---------------------------------------------------------------- 5. parallel layout
fig, ax = plt.subplots(figsize=(10, 3.3)); ax.set_xlim(0, 10); ax.set_ylim(0, 3.3); ax.axis("off")
box(ax, 0.1, 2.2, 1.9, 0.75, "Read run\n(parallel chunks)")
box(ax, 0.1, 1.15, 1.9, 0.75, "Read library\n(parts, streamed)")
box(ax, 0.1, 0.1, 1.9, 0.75, "Calibration panel\n(alongside reading)")
ax.add_patch(FancyBboxPatch((2.7, 0.25), 5.1, 2.7, boxstyle="round,pad=0.02,rounding_size=0.05", fc="#f8fafc", ec=MUTED, lw=1, ls="--"))
ax.text(5.25, 2.78, "one shared pool of worker threads", ha="center", fontsize=8.5, color=MUTED)
for k, (lab, x) in enumerate([("plan\nunits", 2.9), ("extract\ntraces", 4.15), ("encode\nrows", 5.4), ("write\nfiles", 6.65)]):
    box(ax, x, 1.25, 1.0, 0.8, lab, fc="#dbe7ff" if k == 1 else SOFT)
    if k: arrow(ax, x - 0.25, 1.65, x, 1.65)
ax.text(5.25, 0.55, "a unit = up to 256 precursors of one isolation window;\nunits run independently and are written in a fixed order",
        ha="center", fontsize=8, color=INK)
arrow(ax, 2.0, 1.52, 2.9, 1.6); arrow(ax, 2.0, 2.55, 2.9, 1.9)
box(ax, 8.3, 1.25, 1.6, 0.8, "32 Parquet\nparts", fc="#f3f4f6", ec=MUTED); arrow(ax, 7.65, 1.65, 8.3, 1.65)
fig.tight_layout(); fig.savefig(os.path.join(out, "parallel.png"), dpi=170); plt.close(fig)
print("figures ->", out, "| spectra read:", len(spectra), "| window", win, w_lo, w_hi, "| footer keys:", sorted(md)[:12])
