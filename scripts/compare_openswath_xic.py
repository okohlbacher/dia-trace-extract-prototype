#!/usr/bin/env python3
"""Compare DIATraceExtractor traces (long TSV) with OpenSWATH XICs (TextExporter -xic:mode explode).

MS2: each OpenSWATH transition trace is paired with the DIATraceExtractor trace of the same precursor
that matches it best (OpenSWATH's exploded output carries transition ids, not annotations).
MS1: OpenSWATH traces sit on MS1 scan times; they are interpolated at the global cycle times.
Exit status 1 if any trace differs by more than --tol (relative to the trace maximum).
"""
import argparse, sys
import numpy as np, pandas as pd

ap = argparse.ArgumentParser()
ap.add_argument("odia_tsv"); ap.add_argument("openswath_explode_tsv")
ap.add_argument("--tol", type=float, default=1e-3)
a = ap.parse_args()
m = pd.read_csv(a.odia_tsv, sep="\t"); o = pd.read_csv(a.openswath_explode_tsv, sep="\t")
o["trace"] = np.where(o.ms_level == 2, "T" + o.transition_id.astype(str),
                      "M+" + o.annotation.astype(str).str.replace("Precursor_i", "", regex=False))
worst = {1: 0.0, 2: 0.0}; n = {1: 0, 2: 0}
for (seq, lvl, tr), og in o.groupby(["modified_sequence", "ms_level", "trace"]):
    og = og.sort_values("rt"); best = np.inf
    cand = m[(m.modified_sequence == seq) & (m.ms_level == lvl)]
    if lvl == 1: cand = cand[cand.annotation == tr]
    for _, mg in cand.groupby("annotation"):
        mg = mg.sort_values("rt")
        ref = og.intensity.values if lvl == 2 else np.interp(mg.rt, og.rt, og.intensity)
        if len(ref) != len(mg): continue
        best = min(best, np.abs(ref - mg.intensity.values).max() / max(1.0, np.abs(ref).max()))
    worst[lvl] = max(worst[lvl], best); n[lvl] += 1
print(f"MS2 traces {n[2]}, worst rel diff {worst[2]:.2e}; MS1 traces {n[1]}, worst rel diff {worst[1]:.2e}")
sys.exit(0 if max(worst.values()) <= a.tol else 1)
