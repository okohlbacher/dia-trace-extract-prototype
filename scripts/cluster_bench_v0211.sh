#!/usr/bin/env bash
# Every measurement of the release report with the V0.2.11 binary, on one idle node (results/v0.2.11/):
#   1. Astral thread series, ours A and B, 1-64 threads (DIA-NN and OpenSWATH logs linked from threads_cb466f0);
#   2. Astral full libraries, 64 threads, 2 runs each;
#   3. Astral pair table and galleries (§3): ours A and B re-extracted, checked against results/gallery_v02 (cb466f0)
#      with scripts/same_traces.py, compared with DIA-NN and OpenSWATH (cmpA.tsv, cmpB.tsv);
#   4. timsTOF diaPASEF A01/B01 (§3, §6): all precursors, DIA-NN's IDs at the defaults and at DIA-NN's accuracy,
#      the MS1 probes; compared with DIA-NN's XICs.
# The rule emulations (P, A-V0.1, DIA-NN's tolerance, the MS1 probes) keep the constant tolerance (-mz_tol_model
# constant, the default before V0.5.0).
# The tolerance screens extract into a directory per binary (the screen reuses extractions it finds; until V0.4.0
# they ran in one directory, so V0.4.0's screens reused V0.2.11's extractions).
# usage: [X=binary] [STEPS="1 2 3 4"] cluster_bench_v0211.sh
set -uo pipefail
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3; cd $P
X=${X:-$P/bin-releases/DIATraceExtractor-0.2.11}; export X
STEPS=${STEPS:-1 2 3 4}
O=${O:-$P/results/v0.2.11}; mkdir -p $O/full $O/astral $O/diapasef
SC=/scratch/$USER/odia-bench; mkdir -p $SC/input/diapasef $SC/out/v0211
PY=$P/tools/pyenv/bin/python
log() { echo "[$(date +%H:%M:%S)] $*"; }
stage() { local d=$2/$(basename $1); if [ ! -s $d ] || [ $(stat -c %s $1) != $(stat -c %s $d) ]; then cp $1 $d.part && mv $d.part $d; fi; }
timed() { local name=$1; shift; /usr/bin/time -v "$@" > $name.log 2>&1; grep -E "^Total:|Elapsed|Maximum resident" $name.log | sed 's/^\s*//' | cut -c1-150; }
export OMP_WAIT_POLICY=PASSIVE
log "node $(hostname), $(nproc) CPUs, load $(cut -d' ' -f1 /proc/loadavg); $($X --help 2>&1 | grep -m1 Version:); sha256 $(sha256sum $X | cut -c1-12)"
MZML=$SC/input/LFQ_Astral_DIA_15min_50ng_Human_01.mzML
stage $P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_01.mzML $SC/input
LIBIDS=$SC/input/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01_ids_run01.parquet
stage $P/library/refined/$(basename $LIBIDS) $SC/input
ACC=5.9051

if [[ " $STEPS " == *" 1 "* ]]; then
  log "1. thread series"
  SCRATCH=$SC ODIA_ONLY=1 REF=$P/results/threads_cb466f0 bash scripts/bench_threads.sh $O/threads 1 2 4 8 16 32 64 > $O/threads.log 2>&1
  for t in 1 2 4 8 16 32 64; do for a in A B; do
    printf "t%-3s %s  " $t $a; grep -E "Elapsed|Maximum resident" $O/threads/t$t/odia$a.log | sed 's/^\s*//' | tr '\n' ' '; echo; done; done
fi

if [[ " $STEPS " == *" 2 "* ]]; then
  log "2. full libraries"
  for f in library/human_UP000005640_astral_nce25_v0.11.1_tuned_run01.parquet library/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet; do stage $f $SC/input; done
  for rep in 1 2; do for lib in human:human_UP000005640_astral_nce25_v0.11.1_tuned_run01.parquet entrapment:proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet; do
    n=${lib%%:*}; f=${lib#*:}; log "full $n run $rep, load $(cut -d' ' -f1 /proc/loadavg)"
    timed $O/full/${n}_$rep $X -in $MZML -lib $SC/input/$f -rt_mode minutes -threads 64 -out $SC/out/v0211/full_$n.parquet -out_stats $O/full/${n}_$rep.json
    rm -rf $SC/out/v0211/full_$n.parquet $SC/out/v0211/full_$n.axes.parquet
  done; done
fi

if [[ " $STEPS " == *" 3 "* ]]; then
  log "3. Astral pair table"
  G=$SC/out/v0211/gallery; mkdir -p $G
  timed $O/astral/odiaA $X -in $MZML -lib $LIBIDS -rt_mode minutes -rt_window 60 -top_fragments 0 -threads 32 -out $G/odiaA.parquet -out_stats $O/astral/odiaA.json
  timed $O/astral/odiaB $X -in $MZML -lib $LIBIDS -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol_ms2 $ACC -mz_tol_ms1 $ACC -scheme B -threads 32 -out $G/odiaB.parquet -out_stats $O/astral/odiaB.json
  for a in A B; do for s in "" .axes; do
    printf "odia$a$s vs gallery_v02 (cb466f0): "; $PY scripts/same_traces.py $G/odia$a$s.parquet results/gallery_v02/odia$a$s.parquet; done; done | tee $O/astral/same_as_cb466f0.txt
  for a in A B; do
    $PY scripts/compare_traces.py --odia $G/odia$a.parquet --axes $G/odia$a.axes.parquet --diann results/gallery_v02/diann/diann_xic.parquet \
      --openswath results/gallery_v02/osw_traces.parquet --out $O/astral/cmp$a.tsv > $O/astral/cmp$a.out 2>&1; cat $O/astral/cmp$a.out | tail -8; done
fi

if [[ " $STEPS " == *" 4 "* ]]; then
  log "4. diaPASEF"
  D=$SC/out/v0211/diapasef; mkdir -p $D
  for r in A01:ttSCP_diaPASEF_Condition_A_Sample_Alpha_01_11494 B01:ttSCP_diaPASEF_Condition_B_Sample_Alpha_01_11496; do
    s=${r%%:*}; src=$P/benchmark-data/diaPASEF/${r#*:}.mzpeak; in=$SC/input/diapasef/$s.mzpeak
    [ -s $in ] && [ $(stat -c %s $src) = $(stat -c %s $in) ] || cp $src $in
    ids=results/diapasef/diann_${s}_ids.txt; XIC=results/diapasef/diann_${s}_xic.parquet
    run() { local n=$1; shift; timed $O/diapasef/odia_${s}_$n $X -in $in -lib library/hye_timstof.parquet -rt_mode auto -threads 32 -out $D/${s}_$n.parquet "$@"
      $PY scripts/compare_traces.py --odia $D/${s}_$n.parquet --axes $D/${s}_$n.axes.parquet --diann $XIC --out $O/diapasef/cmp_${s}_$n.tsv > $O/diapasef/cmp_${s}_$n.out 2>&1
      grep odia_vs_diann $O/diapasef/cmp_${s}_$n.out; }
    log "$s all precursors"
    timed $O/diapasef/odia_${s}_all $X -in $in -lib library/hye_timstof.parquet -rt_mode auto -threads 32 -out $D/${s}_all.parquet \
      -out_stats $O/diapasef/odia_${s}_all.json -out_calibration $O/diapasef/odia_${s}_all.anchors.tsv
    rm -rf $D/${s}_all.parquet $D/${s}_all.axes.parquet
    # DIA-NN's IDs with all library fragments (as archived; top 6 only for the uncorrected-centre MS1 probe)
    log "$s on DIA-NN's IDs"; run idsdef -ids $ids -top_fragments 0
    run idsP -ids $ids -top_fragments 0 -mz_tol_ms2 11.8 -mz_tol_ms1 11.8 -mz_tol_model constant
    if [ $s = A01 ]; then
      run idsconst -ids $ids -top_fragments 0 -calibration:mz_model constant
      for w in 4 7 20 30; do run ms1_$w -ids $ids -top_fragments 0 -mz_tol_ms2 11.8 -mz_tol_ms1 $w -mz_tol_model constant; done
      for w in 11.8 20; do run ms1raw$w -ids $ids -mz_calibration off -mz_offset_ms2 -4.6 -mz_tol_ms2 10.8 -mz_offset_ms1 0 -mz_tol_ms1 $w; done
    fi
    rm -rf $D/${s}_*.parquet
  done
fi
if [[ " $STEPS " == *" 5 "* ]]; then
  log "5. Astral rules A-V0.1 and P (the archived runs are from cbad9dd, whose calibration differs; same rules here)"
  G=$SC/out/v0211/rules; mkdir -p $G
  timed $O/astral/odiaAV01 $X -in $MZML -lib $LIBIDS -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol_ms2 6 -mz_tol_ms1 2.114775 \
    -calibration:mz_model constant -mz_tol_model constant -threads 32 -out $G/odiaAV01.parquet -out_stats $O/astral/odiaAV01.json
  timed $O/astral/odiaP $X -in $MZML -lib $LIBIDS -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol_ms2 $ACC -mz_tol_ms1 $ACC -mz_tol_model constant \
    -threads 32 -out $G/odiaP.parquet -out_stats $O/astral/odiaP.json
  for a in AV01 P; do
    $PY scripts/compare_traces.py --odia $G/odia$a.parquet --axes $G/odia$a.axes.parquet --diann results/gallery_v02/diann/diann_xic.parquet \
      --openswath results/gallery_v02/osw_traces.parquet --out $O/astral/cmp$a.tsv > $O/astral/cmp$a.out 2>&1; grep odia_vs $O/astral/cmp$a.out; done
  rm -rf $G
fi

if [[ " $STEPS " == *" 6 "* ]]; then
  log "6. tolerance screens"
  mkdir -p $O/tolscreen
  $PY scripts/tolerance_screen.py --extractor $X --run $MZML --lib library/refined/proteobench_entrapment_astral_nce25_v0.11.1_shared_run01.parquet \
    --ids results/tolscreen/astral_run01_ids.txt --sigma-ms2 0.92 --sigma-ms1 0.40 --rt_mode minutes --scratch $SC/out/tolscreen_$(sha256sum $X | cut -c1-12) --out $O/tolscreen/astral_run01 > $O/tolscreen/astral_run01.out 2>&1
  $PY scripts/tolerance_screen.py --extractor $X --run $SC/input/diapasef/A01.mzpeak --lib library/hye_timstof.parquet --ids results/diapasef/diann_A01_ids.txt \
    --sigma-ms2 2.66 --sigma-ms1 3.38 --rt_mode auto --scratch $SC/out/tolscreen_$(sha256sum $X | cut -c1-12) --out $O/tolscreen/timstof_A01 > $O/tolscreen/timstof_A01.out 2>&1
  tail -3 $O/tolscreen/*.out
fi
log "done"
