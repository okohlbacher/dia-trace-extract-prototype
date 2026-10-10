#!/usr/bin/env bash
# MS1 divergence experiments (ours A vs DIA-NN 1.7.12): re-run DIATraceExtractor on the shared tuned library
# with MS1 mass offset / tolerance / aggregation variants. MS2 settings are kept as in benchmark A
# (the measured MS2 offset is passed explicitly instead of -mass_calibration auto). MS1 M+0..M+2 extracted.
# Usage: ms1_variants.sh 01 [threads]   -> results/analysis/ms1var_runNN/<variant>.parquet
set -euo pipefail
RUN=$1; THREADS=${2:-16}
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
NAME=proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01   # library tuned on run 01, used for all runs
R=$P/results/refined/$NAME/run$RUN
MZML=$P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_$RUN.mzML
LIB=$P/library/refined/${NAME}_ids_run$RUN.parquet
X=$P/build/DIATraceExtractor
O=$P/results/analysis/ms1var_run$RUN; mkdir -p $O
ACC=$(grep -oE 'Optimised mass accuracy: [0-9.]+' $R/diann/diann.log | tail -1 | grep -oE '[0-9.]+$')
MS2OFF=$(grep -oE 'MS2 offset [-0-9.e]+' $R/odiaA.log | grep -oE '[-0-9.e]+$')
MS1OFF=$(grep -oE 'MS1 offset [-0-9.e]+' $R/odiaA.log | grep -oE '[-0-9.e]+$')
ACC1=${ACC1:?set ACC1 = DIA-NN MS1 extraction accuracy in ppm (Recommended extraction MS1 mass accuracy, --verbose 2)}
echo "run $RUN: MS2 tol $ACC ppm, MS2 offset $MS2OFF, MS1 offset (ours) $MS1OFF, DIA-NN MS1 tol $ACC1"
run() { # name, extra flags
  local n=$1; shift
  [ -d $O/$n.parquet ] && return
  $X -in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol $ACC -mz_offset $MS2OFF \
     -threads $THREADS -out $O/$n.parquet "$@" > $O/$n.log 2>&1
}
run v_dnn   -aggregation max -mz_tol_ms1 $ACC1 -mz_offset_ms1 0          # DIA-NN's MS1 rule: raw m/z, its MS1 tolerance
run v_tol   -aggregation max -mz_tol_ms1 $ACC1 -mz_offset_ms1 $MS1OFF    # only the tolerance changed
run v_off   -aggregation max -mz_tol_ms1 $ACC  -mz_offset_ms1 0          # only the offset removed
run v_A     -aggregation max -mz_tol_ms1 $ACC  -mz_offset_ms1 $MS1OFF    # = benchmark A (sanity)
run v_sumA  -aggregation sum -mz_tol_ms1 $ACC  -mz_offset_ms1 $MS1OFF    # A with sum
run v_dnn_sum -aggregation sum -mz_tol_ms1 $ACC1 -mz_offset_ms1 0
echo done
