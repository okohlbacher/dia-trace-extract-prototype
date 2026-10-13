#!/usr/bin/env bash
# V0.2.7 cluster benchmarks on an idle node: pull main, rebuild build-stream, thread series (ours only; DIA-NN and
# OpenSWATH logs linked from threads_cb466f0), full libraries at 64 threads (2 runs each), all with /usr/bin/time -v.
set -uo pipefail
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3; cd $P
log() { echo "[$(date +%H:%M:%S)] $*"; }
log "node $(hostname), load $(cut -d' ' -f1 /proc/loadavg)"
git pull --ff-only origin main 2>&1 | tail -1; git log --oneline -1
[ "${SKIP_BUILD:-0}" = 1 ] || cmake --build build-stream -j 48 --target DIATraceExtractor > results/build_v0.2.7.log 2>&1 || { tail -20 results/build_v0.2.7.log; exit 1; }
OMP_WAIT_POLICY=PASSIVE build-stream/DIATraceExtractor --help 2>&1 | grep -m1 "Version:"
H=$(git rev-parse --short HEAD)
log "thread series -> results/threads_v0.2.7_$H"
ODIA_ONLY=1 REF=$P/results/threads_cb466f0 bash scripts/bench_threads.sh results/threads_v0.2.7_$H 1 2 4 8 16 32 64 > results/threads_v0.2.7_$H.log 2>&1
tail -30 results/threads_v0.2.7_$H.log
SC=/scratch/$USER/odia-bench; mkdir -p $SC/input $SC/out results/full_v0.2.7_$H
for f in library/human_UP000005640_astral_nce25.parquet library/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet; do
  d=$SC/input/$(basename $f); [ -s $d ] && [ $(stat -c %s $f) = $(stat -c %s $d) ] || cp $f $d; done
export OMP_WAIT_POLICY=PASSIVE
for rep in 1 2; do for lib in human:human_UP000005640_astral_nce25.parquet entrapment:proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet; do
  n=${lib%%:*}; f=${lib#*:}
  log "full $n run $rep, load $(cut -d' ' -f1 /proc/loadavg)"
  /usr/bin/time -v build-stream/DIATraceExtractor -in $SC/input/LFQ_Astral_DIA_15min_50ng_Human_01.mzML -lib $SC/input/$f -rt_mode minutes -threads 64 \
    -out $SC/out/full_$n.parquet -out_stats results/full_v0.2.7_$H/${n}_$rep.json > results/full_v0.2.7_$H/${n}_$rep.log 2>&1
  grep -E "Total:|Elapsed|Maximum resident" results/full_v0.2.7_$H/${n}_$rep.log | sed 's/^\s*//'
  rm -rf $SC/out/full_$n.parquet $SC/out/full_$n.axes.parquet
done; done
log "done"
