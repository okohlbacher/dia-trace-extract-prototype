#!/usr/bin/env bash
# Three-way trace benchmark for one ProteoBench Astral run:
#   DIA-NN 1.7.12 (patched --xic export) search -> RT anchors + IDs + reference traces
#   DIATraceExtractor: mass-offset scan, DIA-NN-matched extraction (A), OpenSWATH-matched extraction (B),
#   self-calibrated extraction (C: no DIA-NN anchors or offsets)
#   OpenSwathWorkflow on identical coordinates -> .xic -> trace layout
#   compare_traces.py for A (vs DIA-NN and OpenSWATH) and B (vs OpenSWATH)
# Usage: benchmark_run.sh 01|02|03 [threads]
set -euo pipefail
RUN=$1; THREADS=${2:-64}
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
MZML=$P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_$RUN.mzML
LIB=$P/library/human_UP000005640_astral_nce25.parquet
LIB_DIANN=$P/library/human_UP000005640_astral_nce25.diann17.tsv
PY=$P/tools/pyenv/bin/python
X=$P/build/DIATraceExtractor
B=$P/OpenMS/OpenMS-build/bin
D17=$P/results/diann17/run$RUN
C=$P/results/compare/run$RUN
mkdir -p $D17/temp $C/massscan

# 1. DIA-NN 1.7.12 search with XIC export
if [ ! -s $D17/diann_xic.parquet ]; then
  ( cd $D17 && /usr/bin/time -v $P/tools/DiaNN-1.7.12/diann-1.7.12-xicfix --f $MZML --lib $LIB_DIANN --out report.tsv \
      --temp $D17/temp --threads $THREADS --verbose 1 --qvalue 0.01 --xic 60 > diann.log 2>&1 )
  $PY $P/scripts/diann17_xic_to_parquet.py $D17/temp/*.xic $D17/diann_xic.parquet
fi

# 2. RT anchors and precursor ids from the 1% FDR identifications
$PY - "$D17" <<'EOF'
import sys, pandas as pd
d = sys.argv[1]
r = pd.read_csv(f"{d}/report.tsv", sep="\t", usecols=["Precursor.Id", "Q.Value", "RT", "iRT"])
r = r[r["Q.Value"] <= 0.01]
r["pid"] = r["Precursor.Id"].str.replace("UniMod:4", "Carbamidomethyl").str.replace("UniMod:35", "Oxidation")
pd.DataFrame({"lib_rt": r.iRT / 100.0, "run_s": r.RT * 60}).to_csv(f"{d}/anchors.tsv", sep="\t", index=False)
r.pid.to_csv(f"{d}/ids.txt", index=False, header=False)
print(len(r), "identifications at 1% FDR")
EOF

COMMON="-in $MZML -lib $LIB -ids $D17/ids.txt -rt_mode anchors -rt_anchors $D17/anchors.tsv"

# 3. systematic mass offsets: maximise captured intensity at +-2 ppm on 5,000 identified precursors
head -5000 $D17/ids.txt > $C/massscan/ids5k.txt
for off in -5 -4 -3 -2.5 -2 -1.5 -1 -0.5 0 0.5 1 1.5 2 3; do
  ( $X -in $MZML -lib $LIB -ids $C/massscan/ids5k.txt -rt_mode anchors -rt_anchors $D17/anchors.tsv -rt_window 20 \
       -mz_tol 2 -mz_tol_ms1 2 -mz_offset $off -mz_offset_ms1 $off -aggregation sum -threads 4 \
       -out $C/massscan/off$off.parquet > $C/massscan/off$off.log 2>&1 ) &
done
wait
read OFF2 OFF1 < <($PY - "$C/massscan" <<'EOF'
import sys, glob, re, numpy as np, pyarrow.parquet as pq
best = {1: (-1, 0.0), 2: (-1, 0.0)}
for f in glob.glob(sys.argv[1] + "/off*.parquet"):
    if f.endswith(".axes.parquet"): continue
    off = float(re.search(r"off(-?[0-9.]+)\.parquet$", f).group(1))
    t = pq.read_table(f, columns=["ms_level", "intensity"]).to_pandas()
    for lvl, g in t.groupby("ms_level"):
        s = sum(np.frombuffer(b, "<f4").sum() for b in g.intensity)
        if s > best[lvl][0]: best[lvl] = (s, off)
print(best[2][1], best[1][1])
EOF
)
echo "mass offsets: MS2 $OFF2 ppm, MS1 $OFF1 ppm" | tee $C/mass_offsets.txt
ACC=$(grep -oE 'Optimised mass accuracy: [0-9.]+' $D17/diann.log | tail -1 | grep -oE '[0-9.]+$')
echo "DIA-NN optimised mass accuracy: $ACC ppm" | tee -a $C/mass_offsets.txt

# 4. DIATraceExtractor, (A) DIA-NN-matched and (B) OpenSWATH-matched
/usr/bin/time -v $X $COMMON -top_fragments 0 -rt_window 60 -mz_tol $ACC -mz_tol_ms1 $ACC -aggregation max \
    -mz_offset $OFF2 -mz_offset_ms1 $OFF1 -threads $THREADS -out $C/odiaA.parquet -out_stats $C/odiaA.json > $C/odiaA.log 2>&1
/usr/bin/time -v $X $COMMON -top_fragments 0 -rt_window 60 -mz_tol $ACC -mz_tol_ms1 $ACC -aggregation sum \
    -ms1_isotope_spacing unit -threads $THREADS -out $C/odiaB.parquet -out_stats $C/odiaB.json > $C/odiaB.log 2>&1

# 4c. (C) self-calibrated: RT map and mass offsets from the run itself (panel from the whole library),
#     DIA-NN-matched rules; the DIA-NN IDs only select which precursors to export.
/usr/bin/time -v $X -in $MZML -lib $LIB -ids $D17/ids.txt -top_fragments 0 -rt_mode auto -mass_calibration auto \
    -rt_window 60 -mz_tol $ACC -mz_tol_ms1 $ACC -aggregation max -threads $THREADS \
    -out $C/odiaC.parquet -out_stats $C/odiaC.json -out_calibration $C/odiaC_anchors.tsv > $C/odiaC.log 2>&1

# 5. OpenSWATH on identical coordinates (library built from the extractor's traces)
FULL=$($PY -c "print(2*float('$ACC'))")
$PY $P/scripts/traces_to_openswath_tsv.py $C/odiaB.parquet $C/osw_lib.tsv
/usr/bin/time -v $B/OpenSwathWorkflow -in $MZML -tr $C/osw_lib.tsv -out_chrom $C/osw.xic -out_features $C/osw.featureXML \
    -rt_extraction_window 120 -mz_extraction_window $FULL -mz_extraction_window_unit ppm \
    -mz_extraction_window_ms1 $FULL -mz_extraction_window_ms1_unit ppm -ms1_isotopes 2 \
    -Calibration:auto_irt:enabled false -threads $THREADS > $C/osw.log 2>&1
rm -f $C/osw.featureXML   # scores are not part of this benchmark
$P/build/OpenSwathXicToTraces $C/osw.xic $C/osw_traces.parquet

# 6. comparisons
$PY $P/scripts/compare_traces.py --odia $C/odiaA.parquet --axes $C/odiaA.axes.parquet --diann $D17/diann_xic.parquet \
    --openswath $C/osw_traces.parquet --out $C/cmpA.tsv > $C/cmpA.out 2>&1
$PY $P/scripts/compare_traces.py --odia $C/odiaB.parquet --axes $C/odiaB.axes.parquet \
    --openswath $C/osw_traces.parquet --out $C/cmpB.tsv > $C/cmpB.out 2>&1
$PY $P/scripts/compare_traces.py --odia $C/odiaC.parquet --axes $C/odiaC.axes.parquet --diann $D17/diann_xic.parquet \
    --out $C/cmpC.tsv > $C/cmpC.out 2>&1
$PY $P/scripts/validate_calibration.py $D17/report.tsv $C/odiaC.parquet $C/odiaC_anchors.tsv > $C/calC.out 2>&1
echo "== run $RUN"; cat $C/mass_offsets.txt; grep Calibration $C/odiaC.log | cut -c1-200
echo "-- A (DIA-NN anchors + offsets) vs DIA-NN and OpenSWATH"; tail -7 $C/cmpA.out
echo "-- B (OpenSWATH rules) vs OpenSWATH"; tail -3 $C/cmpB.out
echo "-- C (self-calibrated) vs DIA-NN"; tail -3 $C/cmpC.out; cat $C/calC.out
