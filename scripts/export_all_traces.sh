#!/usr/bin/env bash
# Export every arm's mass traces for every benchmark file, in both formats (Parquet + long TSV), onto Ceph.
#   Astral runs 01-03 (entrapment tuned library restricted to DIA-NN's IDs of the run, all fragments, +-60 s):
#     odiaA  ours, V0.2 defaults            odiaP  ours, both tolerances = DIA-NN's accuracy
#     odiaB  ours, -scheme B (OpenSWATH)    diann  DIA-NN 1.7.12 --xic export      osw  OpenSwathWorkflow
#   diaPASEF A01/B01 (HYE library restricted to DIA-NN's IDs): odiaA, odiaP, odiaB, diann (no OpenSWATH run).
# Reference traces are copied from the benchmark results and written as TSV with the same long layout
# (precursor_id, ms_level, annotation, rt, intensity).
# Usage: [X=extractor] [THREADS=n] export_all_traces.sh OUT_ROOT
set -uo pipefail
OUT=${1:?OUT_ROOT}; P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3; S=/scratch/$USER/odia-bench
X=${X:-$P/build-stream/DIATraceExtractor}; T=${THREADS:-32}; PY=$P/tools/pyenv/bin/python
mkdir -p $OUT; echo "started $(date -Is) binary $X ($(git -C $P rev-parse --short HEAD))" | tee -a $OUT/export.log
run() {  # run <dir> <args...>: extractor with Parquet + TSV + stats; skip if done
  local d=$1; shift; [ -s $d.tsv ] && [ -s $d.json ] && { echo "  have $(basename $d)"; return; }
  /usr/bin/time -v $X "$@" -out $d.parquet -out_tsv $d.tsv -out_stats $d.json > $d.log 2>&1 \
    && printf '  %-7s %s  %s KiB\n' $(basename $d) "$(grep -E Elapsed $d.log | grep -oE '[0-9:.]+$')" "$(grep 'Maximum resident' $d.log | grep -oE '[0-9]+$')" \
    || echo "  FAILED $(basename $d): see $d.log"
}
ref_tsv() {  # ref_tsv <parquet> <tsv>: reference traces (rt/intensity as lists, or as float32 blobs) as long TSV
  [ -s $2 ] && return; $PY $P/scripts/traces_to_long_tsv.py "$1" "$2"
}
for RUN in 01 02 03; do
  D=$OUT/astral_run$RUN; mkdir -p $D; echo "== Astral run $RUN $(date -Is)" | tee -a $OUT/export.log
  M=$S/input/LFQ_Astral_DIA_15min_50ng_Human_$RUN.mzML
  LIB=$P/library/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01_ids_run$RUN.parquet
  R=$P/results/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01/run$RUN
  ACC=$(grep -oE 'Optimised mass accuracy: [0-9.]+' $R/diann/diann.log | tail -1 | grep -oE '[0-9.]+$')
  run $D/odiaA -in $M -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -threads $T
  run $D/odiaP -in $M -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol_ms2 $ACC -mz_tol_ms1 $ACC -threads $T
  run $D/odiaB -in $M -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol_ms2 $ACC -mz_tol_ms1 $ACC -scheme B -threads $T
  cp --update=none $R/diann/diann_xic.parquet $D/diann.parquet; ref_tsv $D/diann.parquet $D/diann.tsv
  [ -s $R/osw_traces.parquet ] && { cp --update=none $R/osw_traces.parquet $D/osw.parquet; ref_tsv $D/osw.parquet $D/osw.tsv; }
  echo "DIA-NN accuracy $ACC ppm; files:" >> $D/README.txt; ls -la $D >> $D/README.txt
done 2>&1 | tee -a $OUT/export.log
for RUN in A01 B01; do
  D=$OUT/diapasef_$RUN; mkdir -p $D; echo "== diaPASEF $RUN $(date -Is)" | tee -a $OUT/export.log
  M=$S/input/diapasef/$RUN.mzpeak; LIB=$P/library/hye_timstof.parquet; IDS=$P/results/diapasef/diann_${RUN}_ids.txt
  ACC=$(grep -oE 'Optimised mass accuracy: [0-9.]+' $P/results/diapasef/diann_$RUN.log | tail -1 | grep -oE '[0-9.]+$')
  run $D/odiaA -in $M -lib $LIB -rt_mode auto -rt_window 60 -top_fragments 0 -ids $IDS -threads $T
  run $D/odiaP -in $M -lib $LIB -rt_mode auto -rt_window 60 -top_fragments 0 -ids $IDS -mz_tol_ms2 $ACC -mz_tol_ms1 $ACC -threads $T
  run $D/odiaB -in $M -lib $LIB -rt_mode auto -rt_window 60 -top_fragments 0 -ids $IDS -mz_tol_ms2 $ACC -mz_tol_ms1 $ACC -scheme B -threads $T
  cp --update=none $P/results/diapasef/diann_${RUN}_xic.parquet $D/diann.parquet; ref_tsv $D/diann.parquet $D/diann.tsv
  echo "DIA-NN accuracy $ACC ppm; files:" >> $D/README.txt; ls -la $D >> $D/README.txt
done 2>&1 | tee -a $OUT/export.log
echo "finished $(date -Is)" | tee -a $OUT/export.log; du -sh $OUT | tee -a $OUT/export.log
