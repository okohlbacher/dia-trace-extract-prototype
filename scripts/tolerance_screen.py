#!/usr/bin/env python3
"""Trace quality as a function of the mass tolerance, in multiples of the measured error width (sigma).

For each k the identified precursors are extracted at +-k sigma (MS2 and MS1, each with its own sigma, offsets
measured by the tool) and scored on the traces: fragment co-elution (mean pairwise Pearson over +-W cycles around
the apex of the summed fragment trace, a pair with a fragment absent at the apex counts 0), dropouts (fragments
with no signal at the apex), library cosine (sqrt apex intensities vs sqrt library intensities), off-peak share
(signal outside +-3 W around the apex over the total: interference and noise admitted by the window), and MS1
co-elution (Pearson of the M+0 trace, interpolated onto the cycle axis, with the summed fragment trace over the
same window).

usage: tolerance_screen.py --run RUN --lib LIB --ids IDS --sigma-ms2 S2 --sigma-ms1 S1 --rt_mode MODE --out PREFIX
                           [--ks 1,1.5,2,...] [--threads N] [--extractor BIN] [--window CYCLES] [--extra ARGS]
"""
import argparse, os, subprocess, sys
import numpy as np, pandas as pd, pyarrow.dataset as ds, pyarrow.parquet as pq

ap = argparse.ArgumentParser()
ap.add_argument("--run", required=True); ap.add_argument("--lib", required=True); ap.add_argument("--ids", required=True)
ap.add_argument("--sigma-ms2", type=float, required=True); ap.add_argument("--sigma-ms1", type=float, required=True)
ap.add_argument("--rt_mode", default="minutes"); ap.add_argument("--out", required=True)
ap.add_argument("--ks", default="1,1.5,2,2.5,3,4,5,6,8,10,12"); ap.add_argument("--threads", type=int, default=32)
ap.add_argument("--extractor", default="build-stream/DIATraceExtractor"); ap.add_argument("--window", type=int, default=4)
ap.add_argument("--extra", default=""); ap.add_argument("--scratch", default=f"/scratch/{os.environ.get('USER','x')}/odia-bench/out/tolscreen")
a = ap.parse_args()
os.makedirs(a.scratch, exist_ok=True)
ks = [float(k) for k in a.ks.split(",")]


def extract(k):
    out = os.path.join(a.scratch, f"{os.path.basename(a.out)}_k{k:g}.parquet")
    if not os.path.exists(out):
        cmd = [a.extractor, "-in", a.run, "-lib", a.lib, "-rt_mode", a.rt_mode, "-top_fragments", "0", "-ids", a.ids,
               "-mz_tol_ms2", f"{k * a.sigma_ms2:.4f}", "-mz_tol_ms1", f"{k * a.sigma_ms1:.4f}", "-threads", str(a.threads), "-out", out] + a.extra.split()
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0: sys.exit(f"k={k}: {r.stderr[-2000:]}")
    return out


def score(path, W):
    t = ds.dataset(path).to_table(columns=["precursor_id", "ms_level", "annotation", "cycle_begin", "intensity", "library_intensity"]).to_pandas()
    axes = pq.read_table(path.replace(".parquet", ".axes.parquet")).to_pandas()
    cyc = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy(); ms1t = axes[axes.lane == -2].sort_values("cycle").rt.to_numpy()
    co, drop, cos, off, ms1co = [], [], [], [], []
    ms1 = {pid: g for pid, g in t[(t.ms_level == 1) & (t.annotation == "M+0")].groupby("precursor_id")}
    for pid, g in t[t.ms_level == 2].groupby("precursor_id"):
        Y = np.stack([np.frombuffer(b, "<f4") for b in g.intensity]); F, T = Y.shape
        if F < 3 or T < 2 * W + 1: continue
        s = Y.sum(0); c = int(s.argmax())
        if s[c] <= 0: continue
        lo, hi = max(0, c - W), min(T, c + W + 1); Wn = Y[:, lo:hi]
        pres = Wn[:, c - lo] > 0; drop.append(1 - pres.mean())
        rs = 0.0; n = 0
        for i in range(F):
            for j in range(i + 1, F):
                n += 1
                if pres[i] and pres[j]:
                    x, y = Wn[i], Wn[j]
                    if x.std() > 0 and y.std() > 0: rs += np.corrcoef(x, y)[0, 1]
        co.append(rs / n)
        li = g.library_intensity.to_numpy(); u = np.sqrt(Y[:, c]); v = np.sqrt(li)
        cos.append((u * v).sum() / max(np.linalg.norm(u) * np.linalg.norm(v), 1e-9))
        plo, phi = max(0, c - 3 * W), min(T, c + 3 * W + 1)
        off.append(1 - s[plo:phi].sum() / s.sum())
        m = ms1.get(pid)
        if m is not None:
            r0 = m.iloc[0]; y1 = np.frombuffer(r0.intensity, "<f4"); t1 = ms1t[r0.cycle_begin:r0.cycle_begin + len(y1)]
            tc = cyc[g.cycle_begin.iloc[0] + lo:g.cycle_begin.iloc[0] + hi]
            if len(t1) > 1 and y1.max() > 0:
                yi = np.interp(tc, t1, y1); sw = s[lo:hi]
                if yi.std() > 0 and sw.std() > 0: ms1co.append(np.corrcoef(yi, sw)[0, 1])
    return dict(n=len(co), coelution=np.median(co), coelution_mean=np.mean(co), dropouts=np.mean(drop), cosine=np.median(cos),
                offpeak=np.median(off), ms1_coelution=np.median(ms1co) if ms1co else np.nan)


rows = []
for k in ks:
    p = extract(k); m = score(p, a.window)
    m.update(k=k, ms2_ppm=k * a.sigma_ms2, ms1_ppm=k * a.sigma_ms1); rows.append(m)
    print(f"k={k:g}: +-{m['ms2_ppm']:.2f} / {m['ms1_ppm']:.2f} ppm  n={m['n']}  co-elution {m['coelution']:.4f}  dropouts {100*m['dropouts']:.2f}%  "
          f"cosine {m['cosine']:.4f}  off-peak {100*m['offpeak']:.2f}%  MS1 co-elution {m['ms1_coelution']:.4f}", flush=True)
pd.DataFrame(rows).to_csv(a.out + ".tsv", sep="\t", index=False, float_format="%.5f")
print("->", a.out + ".tsv")
