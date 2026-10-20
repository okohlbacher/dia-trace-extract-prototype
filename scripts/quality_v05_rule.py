#!/usr/bin/env python3
"""Capture / interference / shape of the V0.5 rule A (intensity-dependent fragment tolerance) on the run-01 centroid
sample of report §4, next to V0.2's rule A and DIA-NN (results/analysis/quality/v05_run01_*). The bands, base
tolerance and centre are those of V0.5.0's calibration of the benchmark's run-01 extraction (-out_stats of report §3).
usage: quality_v05_rule.py STATS_JSON"""
import sys, json
import numpy as np, pandas as pd
P = "/ceph/ibmi/abi/oliver/Claude/ODIA-V3"; sys.path.insert(0, P + "/scripts")
R = P + "/results/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01/run01"; SD = P + "/results/analysis/quality/reex_run01"
OUT = P + "/results/analysis/quality/v05_run01"
st = json.load(open(sys.argv[1]))
base = float(st["values"]["mz_tol_ms2"]["value"]); m = [e for e in st["run_model"]["mz"] if e["level"] == "ms2" and e["search"] == "broad"][0]
fac = np.clip(np.array(m["scale_by_intensity"]) / m["scale"], 0.5, float(st["values"]["mz_tol_intensity_max"]["value"]))   # as the tool
upper = np.sqrt(np.array(m["intensity_hi"][:-1]) * np.array(m["intensity_lo"][1:]))   # as the tool: band b holds I below upper[b]
off, slope, ref = st["ms2_offset_ppm"], st["ms2_offset_slope_ppm_per_th"], st["ms2_offset_mz_ref"]
src = open(P + "/scripts/quality_sample.py").read(); src = src[:src.index("rows = [rule_metrics(")]
sys.argv = ["quality_sample.py", R, SD, OUT, OUT + "_fig", "--dnn_quad", "1.97571e-09,0.000387701,-5.58913e-06", "--dnn_ms1", "2.13078",
            "--a_ms2", "-3.213", "--a_ms1", "1.265", "--tol", "5.9051"]
g = {"__name__": "qs", "__file__": P + "/scripts/quality_sample.py"}
exec(compile(src, "quality_sample.py", "exec"), g)
rm = g["rule_metrics"]
ctr05 = lambda cc: off + slope * (cc.target_mz.to_numpy() - ref)
half05 = lambda cc: base * fac[np.searchsorted(upper, cc.intensity.to_numpy(), side="right")]
rows = [rm("DIA-NN (quadratic correction, max, ±5.91)", lambda cc: g["dnn_ctr"](cc.target_mz.to_numpy()), 5.9051, "max", strict=True),
        rm("ours A V0.2 (-3.21 ppm -0.56 ppm/kTh, max, ±7.89)", lambda cc: -3.21341 - 0.000560545 * (cc.target_mz.to_numpy() - 626.28), 7.89197, "max"),
        rm(f"ours A V0.5 constant (max, ±{base:.2f} = 9σ)", ctr05, base, "max"),
        rm(f"ours A V0.5, factors uncapped ({np.clip(np.array(m['scale_by_intensity']) / m['scale'], 0.5, 3).max():.2f} at most)", ctr05,
           lambda cc: base * np.clip(np.array(m["scale_by_intensity"]) / m["scale"], 0.5, 3.0)[np.searchsorted(upper, cc.intensity.to_numpy(), side="right")], "max"),
        rm(f"ours A V0.5 (max, ±{base:.2f} x intensity factor {fac.min():.2f}-{fac.max():.2f})", ctr05, half05, "max")]
e = pd.DataFrame(rows); e.to_csv(OUT + "_engine_rules.tsv", sep="\t", index=False)
with pd.option_context("display.width", 250, "display.max_columns", 30):
    print(e[["rule", "interference_peak", "capture_peak", "coelution", "cos_apex", "dropout_frac", "r_vs_diann_median", "frac_r_vs_diann_gt_0_99"]].round(4).to_string())
