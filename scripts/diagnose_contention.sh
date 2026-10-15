#!/usr/bin/env bash
# Contention diagnosis experiments (ODIA_PROFILE=1, inputs/outputs on /scratch):
#  A  thread series (per-task wall/CPU/faults/context switches, core frequency timeline)
#  B  one process vs several concurrent processes (no shared locks/allocator; shared hardware only)
#  C  task granularity (precursors per unit)
#  D  SMT on one socket: 56 threads one per core vs 112 threads with both hardware threads
# Usage: [SCRATCH=dir] diagnose_contention.sh OUT_DIR [A B C D]
set -uo pipefail
OUT=$(realpath -m "$1"); shift
EXP=${*:-A B C D}
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
X=${X:-$P/build-stream/DIATraceExtractor}
SCRATCH=${SCRATCH:-/scratch/$USER/odia-bench}
mkdir -p $SCRATCH/input $SCRATCH/out $OUT
stage() { local src=$1 dst=$SCRATCH/input/$(basename $1)
  if [ ! -s $dst ] || [ $(stat -c %s $src) != $(stat -c %s $dst) ]; then cp $src $dst.part && mv $dst.part $dst; fi; echo $dst; }
MZML=$(stage $P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_01.mzML)
LIB=$(stage $P/library/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet)
ARGS="-in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -mz_tol_ms2 6 -mz_tol_ms1 0 -mz_calibration auto"
run() {   # name threads [extra tool args] -- [prefix command...]
  local name=$1 t=$2; shift 2
  local extra=() pre=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do extra+=("$1"); shift; done
  [ "${1:-}" = "--" ] && shift
  pre=("$@")
  echo "$(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg) :: $name" >> $OUT/runs.log
  ODIA_PROFILE=1 ODIA_PROFILE_TIMELINE=$OUT/$name.timeline.tsv /usr/bin/time -v "${pre[@]}" $X $ARGS -threads $t "${extra[@]}" \
      -out $SCRATCH/out/$name.parquet -out_stats $OUT/$name.json > $OUT/$name.log 2>&1
  rm -rf $SCRATCH/out/$name.parquet $SCRATCH/out/$name.axes.parquet
}
for e in $EXP; do case $e in
  A) for t in 8 16 32 64 96 128; do run A_t$t $t; done ;;
  B) run B_1x64 64
     run B_2x32_a 32 & run B_2x32_b 32 & wait
     for i in 1 2 3 4; do run B_4x16_$i 16 & done; wait
     run B_2x32_node0 32 -- numactl --cpunodebind=0 --membind=0 & run B_2x32_node1 32 -- numactl --cpunodebind=1 --membind=1 & wait
     run B_1x32_node0 32 -- numactl --cpunodebind=0 --membind=0 ;;
  C) for u in 64 1024; do run C_u${u}_t64 64 -unit_precursors $u; done ;;
  D) run D_56_onepercore 56 -- taskset -c 0-55
     run D_112_smt_socket0 112 -- taskset -c 0-55,112-167
     run D_112_onepercore_both 112 -- taskset -c 0-111 ;;
esac; done
