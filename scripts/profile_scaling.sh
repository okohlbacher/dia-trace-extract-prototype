#!/usr/bin/env bash
# Instrumented scaling runs of DIATraceExtractor (ODIA_PROFILE=1): stage times, profile counters, scheduler
# idle reasons and a 100 ms timeline per run; plus NUMA placement variants.
# Usage: [SCRATCH=dir] profile_scaling.sh OUT_DIR [LIB] [threads...]   (OUT_DIR: logs, timelines, stats)
set -uo pipefail
OUT=$(realpath -m "$1"); shift
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
LIB=${1:-$P/library/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet}; shift || true
THREADS=${*:-8 16 32 48 64 96 128}
X=${X:-$P/build-stream/DIATraceExtractor}
# Inputs and trace output on local /scratch (not Ceph): the inputs are copied once (size-checked).
SCRATCH=${SCRATCH:-/scratch/$USER/odia-bench}
mkdir -p $SCRATCH/input $SCRATCH/out
stage() { local src=$1 dst=$SCRATCH/input/$(basename $1)
  if [ ! -s $dst ] || [ $(stat -c %s $src) != $(stat -c %s $dst) ]; then cp $src $dst.part && mv $dst.part $dst; fi
  echo $dst; }
MZML=$(stage $P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_01.mzML)
LIB=$(stage $LIB)
ARGS="-in $MZML -lib $LIB -rt_mode minutes -rt_window 60 -mz_tol 6 -mz_tol_ms1 0 -aggregation max -mass_calibration auto"
mkdir -p $OUT
run() {   # name threads [numactl args...]
  local name=$1 t=$2; shift 2
  echo "$(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg) :: $name" >> $OUT/runs.log
  ODIA_PROFILE=1 ODIA_PROFILE_TIMELINE=$OUT/$name.timeline.tsv /usr/bin/time -v "$@" $X $ARGS -threads $t \
      -out $SCRATCH/out/out.parquet -out_stats $OUT/$name.json > $OUT/$name.log 2>&1
  rm -rf $SCRATCH/out/out.parquet $SCRATCH/out/out.axes.parquet
}
for t in $THREADS; do run t$t $t; done
[ "${NO_NUMA:-0}" = 1 ] && exit 0
for t in 32 56; do
  run numa_node0_t$t $t numactl --cpunodebind=0 --membind=0
  run numa_interleave_t$t $t numactl --interleave=all
done
