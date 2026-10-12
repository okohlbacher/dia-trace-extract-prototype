#!/usr/bin/env python3
"""Capture / interference / shape of the V0.2 rule A on the run-01 centroid sample, next to DIA-NN and B (results/analysis/quality/v02_run01_*)."""
import sys, os, re, io, contextlib
import numpy as np, pandas as pd
P="/ceph/ibmi/abi/oliver/Claude/ODIA-V3"; sys.path.insert(0, P+"/scripts")
R=P+"/results/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01/run01"; SD=P+"/results/analysis/quality/reex_run01"
OUT=P+"/results/analysis/quality/v02_run01"
# --- MS2: run quality_sample.py up to its engine-rule table, then add the V0.2 rule
src=open(P+"/scripts/quality_sample.py").read(); src=src[:src.index("rows = [rule_metrics(")]
sys.argv=["quality_sample.py",R,SD,OUT,OUT+"_fig","--dnn_quad","1.97571e-09,0.000387701,-5.58913e-06","--dnn_ms1","2.13078","--a_ms2","-3.213","--a_ms1","1.265","--tol","5.9051"]
g={"__name__":"qs","__file__":P+"/scripts/quality_sample.py"}
exec(compile(src,"quality_sample.py","exec"),g)
rm=g["rule_metrics"]
rows=[rm("ours B / OpenSWATH (no correction, sum, ±5.91)",0.0,5.9051,"sum"),
      rm("DIA-NN (quadratic correction, max, ±5.91)",lambda cc: g["dnn_ctr"](cc.target_mz.to_numpy()),5.9051,"max",strict=True),
      rm("ours A V0.1 (-3.21 ppm, max, ±5.91)",-3.213,5.9051,"max"),
      rm("ours A V0.2 (-3.21 ppm -0.56 ppm/kTh, max, ±7.89)",lambda cc: -3.21341-0.000560545*(cc.target_mz.to_numpy()-626.28),7.89197,"max")]
e=pd.DataFrame(rows); e.to_csv(OUT+"_engine_rules.tsv",sep="\t",index=False)
with pd.option_context("display.width",250,"display.max_columns",30): print(e[["rule","interference_peak","capture_peak","coelution","cos_apex","dropout_frac","r_vs_diann_median","frac_r_vs_diann_gt_0_99"]].round(4).to_string())
# --- MS1: quality_ms1_examples.py up to its rule table, then the V0.2 rule
src=open(P+"/scripts/quality_ms1_examples.py").read(); src=src[:src.index("# ---------------------------------------------------------------- MS2 interference examples")]
sys.argv=["quality_ms1_examples.py",SD,OUT,OUT+"_fig","--a_ms2","-3.213","--a_ms1","1.265","--tol","5.9051","--dnn_ms1","2.13078","--mu_quad=-2.8805,-1.068e-03,4.597e-07","--sigma","0.833,70.4","--ishift","1.449,4.75"]
h={"__name__":"qm","__file__":P+"/scripts/quality_ms1_examples.py"}
exec(compile(src,"quality_ms1_examples.py","exec"),h)
c1=h["c1"]; ctr=1.26479+0.0010732*(c1.mz.to_numpy()-566.294)
h["ms1_rule"]("ours A V0.2 (+1.26 ppm +1.07 ppm/kTh, max, ±1.43)",ctr,1.43279,"max")
m1=pd.DataFrame(h["rows"],columns=["rule","centre_ppm","half_ppm","agg","interference_peak","capture_peak","multi_centroid_points"])
m1["centre_ppm"]=[x if np.isscalar(x) else "linear" for x in m1.centre_ppm]
m1.to_csv(OUT+"_ms1_rules.tsv",sep="\t",index=False); print(m1.round(4).to_string())
