#!/usr/bin/env bash
# Runtime vs threads for the four extractions on the same run and the same shared (tuned, ID-restricted)
# library, with the benchmark's settings (scripts/benchmark_refined.sh, RECAL=tuned):
#   DIA-NN 1.7.12 (search + --xic export: it cannot export traces without searching), ours A (DIA-NN rules),
#   ours B (OpenSWATH rules), OpenSwathWorkflow (extraction + its scoring; features discarded).
# Each tool runs once per thread count, interleaved by thread count; /usr/bin/time -v logs per run.
# Usage: bench_threads.sh OUT_DIR [threads...]   (default 4 8 16 32 64)
set -uo pipefail
OUT=$(realpath -m "$1"); shift
THREADS=${*:-4 8 16 32 64}
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
R=$P/results/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01/run01
MZML=$P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_01.mzML
LIB=$P/library/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01_ids_run01.parquet
X=${X:-$P/build-stream/DIATraceExtractor}
DIANN=$P/tools/DiaNN-1.7.12/diann-1.7.12-xicfix
OSW=$P/OpenMS/OpenMS-build/bin/OpenSwathWorkflow
ACC=5.9051; FULL=$(python3 -c "print(2*$ACC)")
mkdir -p $OUT
for t in $THREADS; do
  W=$OUT/t$t; mkdir -p $W/diann/temp
  echo "== $t threads $(date -Is) load $(cut -d' ' -f1 /proc/loadavg)"
  /usr/bin/time -v $X -in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol $ACC -mz_tol_ms1 $ACC \
      -aggregation max -mass_calibration auto -threads $t -out $W/odiaA.parquet > $W/odiaA.log 2>&1
  /usr/bin/time -v $X -in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol $ACC -mz_tol_ms1 $ACC \
      -aggregation sum -ms1_isotope_spacing unit -threads $t -out $W/odiaB.parquet > $W/odiaB.log 2>&1
  ( cd $W/diann && /usr/bin/time -v $DIANN --f $MZML --lib $R/lib_diann17.tsv --out report.tsv --temp $W/diann/temp \
      --threads $t --verbose 1 --qvalue 0.01 --xic 60 > diann.log 2>&1 )
  /usr/bin/time -v $OSW -in $MZML -tr $R/lib_openswath.tsv -out_chrom $W/osw.xic -out_features $W/osw.featureXML \
      -rt_extraction_window 120 -mz_extraction_window $FULL -mz_extraction_window_unit ppm \
      -mz_extraction_window_ms1 $FULL -mz_extraction_window_ms1_unit ppm -ms1_isotopes 2 \
      -Calibration:auto_irt:enabled false -threads $t > $W/osw.log 2>&1
  rm -rf $W/odiaA.parquet $W/odiaB.parquet $W/osw.xic $W/osw.featureXML $W/diann/temp $W/odiaA.axes.parquet $W/odiaB.axes.parquet
  for f in odiaA odiaB diann/diann osw; do
    printf '  %-12s %s  %s KiB\n' $f "$(grep -E 'Elapsed' $W/$f.log | grep -oE '[0-9:.]+$')" "$(grep -E 'Maximum resident' $W/$f.log | grep -oE '[0-9]+$')"
  done
done
