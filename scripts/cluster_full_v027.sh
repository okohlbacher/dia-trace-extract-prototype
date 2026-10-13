#!/usr/bin/env bash
# V0.2.7 full-library runs (tuned libraries, as in the report), 64 threads, 2 runs each, /usr/bin/time -v.
set -uo pipefail
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3; cd $P; H=$(git rev-parse --short HEAD); O=results/full_v0.2.7_$H; mkdir -p $O
log() { echo "[$(date +%H:%M:%S)] $*"; }
SC=/scratch/$USER/odia-bench; mkdir -p $SC/input $SC/out
for f in library/human_UP000005640_astral_nce25_v0.11.1_tuned_run01.parquet library/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet; do
  d=$SC/input/$(basename $f); [ -s $d ] && [ $(stat -c %s $f) = $(stat -c %s $d) ] || cp $f $d; done
export OMP_WAIT_POLICY=PASSIVE
rm -f $O/*.log $O/*.json
for rep in 1 2; do for lib in human:human_UP000005640_astral_nce25_v0.11.1_tuned_run01.parquet entrapment:proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet; do
  n=${lib%%:*}; f=${lib#*:}
  log "full $n run $rep, node $(hostname), load $(cut -d' ' -f1 /proc/loadavg)"
  /usr/bin/time -v build-stream/DIATraceExtractor -in $SC/input/LFQ_Astral_DIA_15min_50ng_Human_01.mzML -lib $SC/input/$f -rt_mode minutes -threads 64 \
    -out $SC/out/full_$n.parquet -out_stats $O/${n}_$rep.json > $O/${n}_$rep.log 2>&1
  grep -E "^Run grid:|^Calibration:|^Extraction:|^Total:|Elapsed|Maximum resident" $O/${n}_$rep.log | sed 's/^\s*//; s/; memory \(.*\) (peak so far \(.*\) GiB).*/; memory \1 peak \2 GiB/' | cut -c1-160
  rm -rf $SC/out/full_$n.parquet $SC/out/full_$n.axes.parquet
done; done
log "done"
