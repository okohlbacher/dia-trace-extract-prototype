#!/usr/bin/env python3
"""External check of DIATraceExtractor's self-calibration against DIA-NN identifications.

For every DIA-NN precursor at q <= 0.01 present in the traces file, residual = DIA-NN apex RT - rt_center
(the extractor's mapped library RT). Reports |residual| quantiles and the share of DIA-NN apexes inside
+-W windows, plus how many calibration anchors are DIA-NN identifications.
usage: validate_calibration.py report.tsv traces.parquet [anchors.tsv]
"""
import sys, pandas as pd, pyarrow.parquet as pq
rep, traces = sys.argv[1], sys.argv[2]
r = pd.read_csv(rep, sep="\t", usecols=["Precursor.Id", "Q.Value", "RT"])
r = r[r["Q.Value"] <= 0.01]
r["pid"] = r["Precursor.Id"].str.replace("UniMod:4", "Carbamidomethyl").str.replace("UniMod:35", "Oxidation")
t = pq.read_table(traces, columns=["precursor_id", "rt_center"]).to_pandas().drop_duplicates("precursor_id")
j = r.merge(t, left_on="pid", right_on="precursor_id")
res = j.RT * 60 - j.rt_center; a = res.abs()
print(f"DIA-NN IDs {len(r)}, joined {len(j)}")
print("external |residual| p50 %.1f p90 %.1f p95 %.1f p99 %.1f s; median signed %.2f s"
      % (a.quantile(.5), a.quantile(.9), a.quantile(.95), a.quantile(.99), res.median()))
print("apex inside window: " + ", ".join("+-%ds %.4f" % (w, (a <= w).mean()) for w in (10, 20, 30, 45, 60)))
if len(sys.argv) > 3:
    an = pd.read_csv(sys.argv[3], sep="\t"); ids = set(r.pid)
    u = an[an.used == 1]
    print(f"anchors {len(an)}, used {len(u)}; DIA-NN 1% IDs among all {an.precursor_id.isin(ids).mean():.3f}, "
          f"among used {u.precursor_id.isin(ids).mean():.3f}")
