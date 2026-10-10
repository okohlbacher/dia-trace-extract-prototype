#!/usr/bin/env bash
# Three-way trace benchmark on ONE shared, recalibrated library per run:
#   1. DIA-NN 1.7.12 searches the predicted DIALibGen library            (round 1; cached)
#   2. DIALibGen refine writes DIA-NN's observed RT into the library       -> library/refined/..._runNN.parquet
#      (1% FDR identifications; RT = observed run minutes)
#   3. the refined library is the input of all three tools:
#      DIA-NN 1.7.12 (round 2, --xic), DIATraceExtractor (-rt_mode minutes), OpenSwathWorkflow (RT in s, no iRT)
#   4. compare_traces.py: extractor A (DIA-NN rules) vs DIA-NN and OpenSWATH; extractor B (OpenSWATH rules)
#      vs OpenSWATH; DIA-NN vs OpenSWATH
# Usage: [LIB=predicted.parquet] [DIALIBGEN=path] benchmark_refined.sh 01|02|03 [threads]
#   LIB defaults to the human 0.11.0 library; outputs are keyed by the library name.
set -euo pipefail
RUN=$1; THREADS=${2:-64}
P=/ceph/ibmi/abi/oliver/Claude/ODIA-V3
MZML=$P/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_$RUN.mzML
PRED=${LIB:-$P/library/human_UP000005640_astral_nce25.parquet}
NAME=$(basename $PRED .parquet)
PRED_DIANN=$P/library/diann17/$NAME.tsv   # UniprotID + ProteinName columns (DIA-NN protein groups)
REF=$P/library/refined/${NAME}_shared_run$RUN.parquet
DLG=${DIALIBGEN:-$P/tools/DIALibGen/build/DIALibGen}
PY=$P/tools/pyenv/bin/python
X=$P/build/DIATraceExtractor
B=$P/OpenMS/OpenMS-build/bin
DIANN=$P/tools/DiaNN-1.7.12/diann-1.7.12-xicfix
D1=$P/results/diann17/$NAME/run$RUN    # round 1 (predicted library)
O=$P/results/refined/$NAME/run$RUN     # everything on the refined library
mkdir -p $D1/temp $O/diann/temp $P/library/refined $P/library/diann17

# 0. the predicted library as a DIA-NN 1.7.12 spectral library (TSV, passed with --lib)
if [ ! -s $PRED_DIANN ]; then
  $PY $P/scripts/lib_to_diann17_tsv.py $PRED $PRED_DIANN.part > $PRED_DIANN.log 2>&1 && mv $PRED_DIANN.part $PRED_DIANN
fi

# 1. round 1: DIA-NN 1.7.12 on the predicted library
if [ ! -s $D1/report.tsv ]; then
  ( cd $D1 && $DIANN --f $MZML --lib $PRED_DIANN --out report.tsv --temp $D1/temp --threads $THREADS \
      --verbose 1 --qvalue 0.01 > diann.log 2>&1 )
fi

# 2. recalibrate the library with DIALibGen (observed RT of the 1% FDR identifications)
if [ ! -s $REF ]; then   # DIALibGen refuses to overwrite; refinement is deterministic, so reuse
$PY $P/scripts/diann17_report_to_parquet.py $D1/report.tsv $D1/report.parquet
( export LD_LIBRARY_PATH=$P/tools/onnxruntime-linux-x64-1.31.0/lib DIALIBGEN_MODEL_DIR=$P/tools/DIALibGen/models \
         OPENMS_DATA_PATH=${OPENMS_DATA_PATH:-$(dirname $DLG)/../share/OpenMS}
  /usr/bin/time -v $DLG -mode refine -in $PRED -ids $D1/report.parquet \
      -q_global 1 -q_protein 1 -out $REF -out_report ${REF%.parquet}.residuals.tsv -threads 16 \
      > ${REF%.parquet}.refine.log 2>&1 )
fi
grep -E 'matched|library .* -> ' ${REF%.parquet}.refine.log || true
grep -E 'Spectral library loaded|IDs at 0.01' $D1/diann.log || true

# 3a. the same library in each tool's format
$PY $P/scripts/lib_to_diann17_tsv.py $REF $O/lib_diann17.tsv --rt_scale 1 > /dev/null    # iRT := observed minutes
$PY $P/scripts/lib_to_openswath_tsv.py $REF $O/lib_openswath.tsv --rt_scale 60          # RT in seconds

# 3b. DIA-NN 1.7.12 on the refined library, with XIC export
( cd $O/diann && /usr/bin/time -v $DIANN --f $MZML --lib $O/lib_diann17.tsv --out report.tsv --temp $O/diann/temp \
    --threads $THREADS --verbose 1 --qvalue 0.01 --xic 60 > diann.log 2>&1 )
$PY $P/scripts/diann17_xic_to_parquet.py $O/diann/temp/*.xic $O/diann/diann_xic.parquet
ACC=$(grep -oE 'Optimised mass accuracy: [0-9.]+' $O/diann/diann.log | tail -1 | grep -oE '[0-9.]+$')
FULL=$($PY -c "print(2*float('$ACC'))")
echo "DIA-NN round 2: $(grep -oE 'Number of IDs at 0.01 FDR: [0-9]+' $O/diann/diann.log | tail -1); mass accuracy $ACC ppm"

# 3c. DIATraceExtractor on the refined library (RT = observed minutes); all library fragments, +-60 s
#     A: DIA-NN rules (max), mass offsets measured from the run; B: OpenSWATH rules (sum, no correction, unit isotopes)
/usr/bin/time -v $X -in $MZML -lib $REF -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol $ACC -mz_tol_ms1 $ACC \
    -aggregation max -mass_calibration auto -threads $THREADS -out $O/odiaA.parquet -out_stats $O/odiaA.json > $O/odiaA.log 2>&1
/usr/bin/time -v $X -in $MZML -lib $REF -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol $ACC -mz_tol_ms1 $ACC \
    -aggregation sum -ms1_isotope_spacing unit -threads $THREADS -out $O/odiaB.parquet -out_stats $O/odiaB.json > $O/odiaB.log 2>&1

# 3d. OpenSWATH on the refined library: identity RT (no iRT), +-60 s, same full-width m/z window
/usr/bin/time -v $B/OpenSwathWorkflow -in $MZML -tr $O/lib_openswath.tsv -out_chrom $O/osw.xic -out_features $O/osw.featureXML \
    -rt_extraction_window 120 -mz_extraction_window $FULL -mz_extraction_window_unit ppm \
    -mz_extraction_window_ms1 $FULL -mz_extraction_window_ms1_unit ppm -ms1_isotopes 2 \
    -Calibration:auto_irt:enabled false -threads $THREADS > $O/osw.log 2>&1
rm -f $O/osw.featureXML   # scores are not part of this benchmark
$P/build/OpenSwathXicToTraces $O/osw.xic $O/osw_traces.parquet

# 4. comparisons
$PY $P/scripts/compare_traces.py --odia $O/odiaA.parquet --axes $O/odiaA.axes.parquet --diann $O/diann/diann_xic.parquet \
    --openswath $O/osw_traces.parquet --out $O/cmpA.tsv > $O/cmpA.out 2>&1
$PY $P/scripts/compare_traces.py --odia $O/odiaB.parquet --axes $O/odiaB.axes.parquet \
    --openswath $O/osw_traces.parquet --out $O/cmpB.tsv > $O/cmpB.out 2>&1
echo "== run $RUN (refined library)"
grep -E 'Calibration' $O/odiaA.log | grep -oE 'MS2 offset [^;]*' || true
for f in $O/diann/diann.log $O/odiaA.log $O/osw.log; do
  printf '%-12s %s | %s\n' "$(basename $(dirname $f))/$(basename $f)" "$(grep -E 'Elapsed' $f | grep -oE '[0-9:.]+$')" \
    "$(grep -E 'Maximum resident' $f | grep -oE '[0-9]+$') KiB"
done
echo "-- A (DIA-NN rules) vs DIA-NN and OpenSWATH"; tail -7 $O/cmpA.out
echo "-- B (OpenSWATH rules) vs OpenSWATH"; tail -3 $O/cmpB.out
