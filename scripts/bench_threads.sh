#!/usr/bin/env bash
# Runtime vs threads for the four extractions on the same run and the same shared (tuned, ID-restricted)
# library, with the benchmark's settings (scripts/benchmark_refined.sh, RECAL=tuned):
#   DIA-NN 1.7.12 (search + --xic export: it cannot export traces without searching), ours A (the defaults:
#   tolerances as instrument-class multiples of the measured widths, m/z-dependent offset when material),
#   ours B (OpenSWATH rules), OpenSwathWorkflow (extraction + its scoring; features discarded).
# Each tool runs once per thread count, interleaved by thread count; /usr/bin/time -v logs per run.
# ODIA_ONLY=1 REF=dir: run only ours; DIA-NN and OpenSWATH logs are linked from REF (an earlier OUT_DIR) per thread count.
# Usage: [SCRATCH=dir] [ODIA_ONLY=1 REF=dir] bench_threads.sh OUT_DIR [threads...]   (default 4 8 16 32 64). Inputs are staged to
# $SCRATCH/input (default /scratch/$USER/odia-bench); OUT_DIR gets logs; trace output goes to $SCRATCH/out.
set -uo pipefail
OUT=$(realpath -m "$1"); shift
THREADS=${*:-4 8 16 32 64}
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
R=$P/results/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01/run01
# Inputs and trace output on local /scratch (not Ceph): the inputs are copied once (size-checked).
SCRATCH=${SCRATCH:-/scratch/$USER/odia-bench}
mkdir -p $SCRATCH/input $SCRATCH/out
stage() { local src=$1 dst=$SCRATCH/input/$(basename $1)
  if [ ! -s $dst ] || [ $(stat -c %s $src) != $(stat -c %s $dst) ]; then cp $src $dst.part && mv $dst.part $dst; fi
  echo $dst; }
MZML=$(stage $P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_01.mzML)
LIB=$(stage $P/library/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01_ids_run01.parquet)
LIB_DIANN=$(stage $R/lib_diann17.tsv)
LIB_OSW=$(stage $R/lib_openswath.tsv)
X=${X:-$P/build-stream/DIATraceExtractor}
DIANN=$P/tools/DiaNN-1.7.12/diann-1.7.12-xicfix
OSW=$P/OpenMS/OpenMS-build/bin/OpenSwathWorkflow
ACC=5.9051; FULL=$(python3 -c "print(2*$ACC)")
mkdir -p $OUT
for t in $THREADS; do
  W=$OUT/t$t; mkdir -p $W/diann; T=$SCRATCH/out/t$t; mkdir -p $T/diann/temp
  echo "== $t threads $(date -Is) load $(cut -d' ' -f1 /proc/loadavg)"
  /usr/bin/time -v $X -in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 \
      -threads $t -out $T/odiaA.parquet > $W/odiaA.log 2>&1
  /usr/bin/time -v $X -in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol_ms2 $ACC -mz_tol_ms1 $ACC \
      -scheme B -threads $t -out $T/odiaB.parquet > $W/odiaB.log 2>&1
  if [ "${ODIA_ONLY:-0}" = 1 ]; then
    ln -sfn $(realpath $REF/t$t/diann/diann.log) $W/diann/diann.log; ln -sfn $(realpath $REF/t$t/osw.log) $W/osw.log
  else
  ( cd $T/diann && /usr/bin/time -v $DIANN --f $MZML --lib $LIB_DIANN --out $T/diann/report.tsv --temp $T/diann/temp \
      --threads $t --verbose 1 --qvalue 0.01 --xic 60 > $W/diann/diann.log 2>&1 )
  /usr/bin/time -v $OSW -in $MZML -tr $LIB_OSW -out_chrom $T/osw.xic -out_features $T/osw.featureXML \
      -rt_extraction_window 120 -mz_extraction_window $FULL -mz_extraction_window_unit ppm \
      -mz_extraction_window_ms1 $FULL -mz_extraction_window_ms1_unit ppm -ms1_isotopes 2 \
      -Calibration:auto_irt:enabled false -threads $t > $W/osw.log 2>&1
  fi
  rm -rf $T
  for f in odiaA odiaB diann/diann osw; do
    printf '  %-12s %s  %s KiB\n' $f "$(grep -E 'Elapsed' $W/$f.log | grep -oE '[0-9:.]+$')" "$(grep -E 'Maximum resident' $W/$f.log | grep -oE '[0-9]+$')"
  done
done
