#!/usr/bin/env bash
# Follow-up diagnosis per the parallel-efficiency skill: hygiene (flock, others' CPU), 1-thread baseline
# (Karp-Flatt), noise (3 interleaved repeats at 32 and 64), external /proc sampling, gdb stack samples at 64/128.
# Usage: diagnose_contention2.sh OUT_DIR [S] [K]
set -uo pipefail
OUT=$(realpath -m "$1"); shift
EXP=${*:-S K}
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
X=${X:-$P/build-stream/DIATraceExtractor}
SCRATCH=${SCRATCH:-/scratch/$USER/odia-bench}
MZML=$SCRATCH/input/LFQ_Astral_DIA_15min_50ng_Human_01.mzML
LIB=$SCRATCH/input/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet
ARGS="-in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -mz_tol 6 -mz_tol_ms1 0 -mass_calibration auto"
mkdir -p $OUT
run() {   # name threads [stacks]
  local name=$1 t=$2 stacks=${3:-}
  ODIA_ALLOW_PTRACE=1 ODIA_PROFILE=1 ODIA_PROFILE_TIMELINE=$OUT/$name.timeline.tsv python3 $P/scripts/bench_run.py $OUT/$name ${stacks:+--stacks $stacks} \
     --hash $SCRATCH/out/$name.parquet -- /usr/bin/time -v $X $ARGS -threads $t -out $SCRATCH/out/$name.parquet -out_stats $OUT/$name.json \
     | sed "s/^/$name\t/" >> $OUT/meta_all.tsv
  rm -rf $SCRATCH/out/$name.parquet $SCRATCH/out/$name.axes.parquet
}
for e in $EXP; do case $e in
  S) run S_t1 1
     for r in 1 2 3; do run S_t32_r$r 32; run S_t64_r$r 64; done ;;
  K) run K_t64 64 8,10,12
     run K_t128 128 10,14,18 ;;
esac; done
