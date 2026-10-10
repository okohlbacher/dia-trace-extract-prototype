#!/usr/bin/env python3
"""RT accuracy of a library against DIA-NN 1.7.12 observed RT (1% FDR IDs of a report).
  --lib L    : error of the library's own RT column (minutes; tuned libraries)
  (default)  : error of DIA-NN's Predicted.RT (DIA-NN's calibration of the library RT; untuned libraries)
Usage: rt_accuracy.py report.parquet [--lib lib.parquet] [--label X]"""
import argparse, re, numpy as np, pyarrow.parquet as pq, pyarrow.compute as pc, pyarrow as pa
UNIMOD = {"Carbamidomethyl": "UniMod:4", "Oxidation": "UniMod:35"}
pat = re.compile(r"\(([^)]*)\)")
ap = argparse.ArgumentParser(); ap.add_argument("report"); ap.add_argument("--lib"); ap.add_argument("--label", default="")
a = ap.parse_args()
r = pq.read_table(a.report, columns=["Modified.Sequence", "Precursor.Charge", "Q.Value", "RT", "Predicted.RT"])
r = r.filter(pc.less_equal(r["Q.Value"], 0.01))
obs = r["RT"].to_numpy()
if a.lib:
    keys = {f"{s}/{z}": i for i, (s, z) in enumerate(zip(r["Modified.Sequence"].to_pylist(), r["Precursor.Charge"].to_pylist()))}
    pred = np.full(len(obs), np.nan)
    f = pq.ParquetFile(a.lib)
    for rg in range(f.num_row_groups):
        t = f.read_row_group(rg, columns=["Modified.Sequence", "Precursor.Charge", "RT"])
        for s, z, rt in zip(pc.cast(t["Modified.Sequence"], pa.string()).to_pylist(), t["Precursor.Charge"].to_pylist(), t["RT"].to_pylist()):
            i = keys.get(f"{pat.sub(lambda m: '(' + UNIMOD.get(m.group(1), m.group(1)) + ')', s)}/{z}")
            if i is not None: pred[i] = rt
else:
    pred = r["Predicted.RT"].to_numpy()
d = (pred - obs) * 60.0; d = d[np.isfinite(d)]
mad = 1.4826 * np.median(np.abs(d - np.median(d)))
print(f"{a.label:28s} n={len(d):6d}  median {np.median(d):+6.1f} s  robust SD {mad:5.1f} s  |err|<=60s {np.mean(np.abs(d) <= 60):6.1%}  95% |err| {np.percentile(np.abs(d), 95):5.1f} s")
