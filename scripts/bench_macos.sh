#!/usr/bin/env bash
# DIATraceExtractor benchmark on a MacBook (Apple silicon, macOS 14+): build OpenMS develop and the tool, fetch the
# inputs from the IBMI cluster, run the thread series and the full-library runs, pack the results.
#
#   bash bench_macos.sh all                 # everything below, in order (first run: 1-3 h, most of it vcpkg + OpenMS)
#   bash bench_macos.sh deps|openms|tool|inputs|bench|pack   # one phase; every phase is idempotent and resumable
#
# Settings (environment): WORK=~/odia-bench (everything lives here), CLUSTER=kohlbach@data (rsync source, reached
# through -J kohlbach@hive.cs.uni-tuebingen.de; CLUSTER= to skip and place the inputs by hand), THREADS="1 2 4 8 ..."
# (default: 1, 2, 4, ... up to the physical core count, plus the performance-core count), RUN_OSW=1 adds one
# OpenSwathWorkflow run at the maximum thread count (10-30 min), X=<binary> benchmarks that binary instead of the one
# built here (e.g. the signed release from Homebrew: X=$(brew --prefix)/bin/DIATraceExtractor bash bench_macos.sh bench).
set -euo pipefail
WORK=${WORK:-$HOME/odia-bench}; CLUSTER=${CLUSTER-kohlbach@data}; JUMP=${JUMP:-kohlbach@hive.cs.uni-tuebingen.de}
REPO=https://github.com/okohlbacher/dia-trace-extract-prototype.git; TAG=${TAG:-V0.3.0}
OPENMS_COMMIT=${OPENMS_COMMIT:-10b0a7af09}   # the OpenMS develop commit the cluster benchmarks were built against
REMOTE_INPUTS=/ceph/ibmi/abi/oliver/Claude/ODIA-V3/benchmark-data/laptop-inputs/
mkdir -p $WORK; cd $WORK
log() { echo "[$(date +%H:%M:%S)] $*"; }
# Homebrew's bin directory is not on PATH in a plain bash shell (Apple silicon: /opt/homebrew/bin); CMake then finds
# neither Ninja nor a compiler. Put it there and name the compiler and the build program explicitly.
for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x $b ] && eval "$($b shellenv)" && break; done
toolchain() {
  CC_=$(xcrun -f clang) CXX_=$(xcrun -f clang++) NINJA_=$(command -v ninja || true)
  [ -n "$NINJA_" ] || { log "ninja not found; run: bash $0 deps"; exit 1; }
  echo "-DCMAKE_C_COMPILER=$CC_ -DCMAKE_CXX_COMPILER=$CXX_ -DCMAKE_MAKE_PROGRAM=$NINJA_"
}

deps() {
  xcode-select -p > /dev/null 2>&1 || { log "installing the Xcode command line tools; re-run when done"; xcode-select --install; exit 1; }
  command -v brew > /dev/null || { log "Homebrew is needed: https://brew.sh"; exit 1; }
  brew install cmake ninja pkg-config autoconf automake autoconf-archive libtool libomp python@3.12 rsync uv
  log "deps ok: $(cmake --version | head -1), libomp at $(brew --prefix libomp)"
}

openms() {
  if [ ! -d OpenMS ]; then git clone --filter=blob:none https://github.com/OpenMS/OpenMS.git; fi
  cd OpenMS
  git fetch -q origin && git checkout -q $OPENMS_COMMIT
  git submodule update --init --depth 1 vcpkg
  [ -x vcpkg/vcpkg ] || ./vcpkg/bootstrap-vcpkg.sh -disableMetrics
  # the mzPeak reader needs Boost.JSON and Boost.ProgramOptions, the instrument files yaml-cpp (from 0.3.0), all from
  # the same vcpkg tree
  grep -q yaml-cpp vcpkg.json || python3 - <<'EOF'
import json; m = json.load(open("vcpkg.json"))
deps = m["dependencies"]; deps += [d for d in ("boost-json", "boost-program-options", "yaml-cpp") if d not in deps]
json.dump(m, open("vcpkg.json", "w"), indent=2)
EOF
  # from 0.3.0 the tool needs the TOPPBase patch (patches/README.md of the tool)
  if ! grep -q setDeprecated_ src/openms_cli/include/OpenMS/APPLICATIONS/TOPPBase.h; then
    curl -fsSL https://raw.githubusercontent.com/okohlbacher/dia-trace-extract-prototype/$TAG/patches/openms-toppbase-deprecated-options.patch | git apply
  fi
  bash tools/ci/deps-macos.sh --skip-doc-deps --skip-gui-deps || true
  cmake --preset macos-arm64-release $(toolchain) -DOpenMP_ROOT=$(brew --prefix libomp) -DWITH_GUI=OFF -DOPENMS_BUILD_TESTS=OFF 2>&1 | tail -3
  cmake --build build/macos-arm64-release -j $(sysctl -n hw.physicalcpu) --target OpenMS OpenMS_CLI OpenSwathWorkflow TextExporter FileConverter > build/macos-arm64-release/build.log 2>&1 \
    || { grep -E -B3 -A12 "error:|Error " build/macos-arm64-release/build.log | head -120; log "OpenMS build failed (full log: $PWD/build/macos-arm64-release/build.log)"; exit 1; }
  cd ..; log "OpenMS built: $(ls OpenMS/build/macos-arm64-release/bin | head -3 | tr '\n' ' ')"
}

tool() {
  if [ ! -d dia-trace-extract-prototype ]; then git clone $REPO; fi
  cd dia-trace-extract-prototype; git fetch -q --tags --force; git checkout -q $TAG; git submodule update --init
  O=$WORK/OpenMS/build/macos-arm64-release; V=$O/vcpkg_installed/arm64-osx-dynamic-sysroot
  cmake -S . -B build -G Ninja $(toolchain) -DCMAKE_BUILD_TYPE=Release -DOpenMP_ROOT=$(brew --prefix libomp) -DOPENMS_BIN=$O/bin \
        -DCMAKE_PREFIX_PATH="$O;$V" 2>&1 | tail -2
  cmake --build build -j $(sysctl -n hw.physicalcpu) --target DIATraceExtractor > build/build.log 2>&1 \
    || { grep -E -B3 -A12 "error:|Error " build/build.log | head -120; log "tool build failed (full log: $PWD/build/build.log)"; exit 1; }
  ./build/DIATraceExtractor --help > /dev/null && log "DIATraceExtractor built"
  # the comparison tests run scripts/*.py through tools/pyenv (Homebrew python refuses pip --user, PEP 668)
  [ -x tools/pyenv/bin/python ] || { uv venv -q tools/pyenv && uv pip install -q --python tools/pyenv/bin/python pyarrow pandas numpy; }
  ctest --test-dir build 2>&1 | tail -2 || log "ctest did not pass fully (see build/Testing); the benchmark can still run"
  cd ..
}

inputs() {
  mkdir -p input
  if [ -n "$CLUSTER" ]; then
    log "fetching the inputs from $CLUSTER (6.9 GB, once)"
    rsync -avL --progress -e "ssh -J $JUMP" $CLUSTER:$REMOTE_INPUTS input/
  fi
  for f in LFQ_Astral_DIA_15min_50ng_Human_01.mzML proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01_ids_run01.parquet \
           proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet lib_openswath.tsv; do
    [ -s input/$f ] || { log "missing input/$f (copy it from $REMOTE_INPUTS)"; exit 1; }
  done
  log "inputs ok: $(du -sh input | cut -f1)"
}

bench() {
  X=${X:-$WORK/dia-trace-extract-prototype/build/DIATraceExtractor}; OUT=$WORK/results; mkdir -p $OUT; cd $OUT
  M=$WORK/input/LFQ_Astral_DIA_15min_50ng_Human_01.mzML
  LIB=$WORK/input/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01_ids_run01.parquet
  FULL=$WORK/input/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet
  PHYS=$(sysctl -n hw.physicalcpu); PERF=$(sysctl -n hw.perflevel0.physicalcpu 2> /dev/null || echo $PHYS)
  if [ -z "${THREADS:-}" ]; then THREADS=""; t=1; while [ $t -lt $PHYS ]; do THREADS="$THREADS $t"; t=$((t * 2)); done
    THREADS="$THREADS $PHYS"; [ $PERF -ne $PHYS ] && THREADS="$THREADS $PERF"; THREADS=$(echo $THREADS | tr ' ' '\n' | sort -nu | tr '\n' ' '); fi
  { echo "date $(date "+%Y-%m-%dT%H:%M:%S%z")"; pmset -g batt | tail -1; sw_vers; sysctl -n machdep.cpu.brand_string; sysctl hw.physicalcpu hw.logicalcpu hw.perflevel0.physicalcpu hw.perflevel1.physicalcpu hw.memsize 2> /dev/null
    echo "tool $X: $($X --help 2>&1 | grep -m1 '^Version')"; echo "threads: $THREADS"; } | tee machine.txt
  export OMP_WAIT_POLICY=PASSIVE
  for rep in 1 2; do for t in $THREADS; do
    log "shared library, $t threads, repeat $rep"
    /usr/bin/time -l $X -in $M -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -threads $t \
        -out /tmp/odiaA.parquet -out_stats odiaA_t${t}_r$rep.json > odiaA_t${t}_r$rep.log 2>&1 || log "  ours A failed (see odiaA_t${t}_r$rep.log)"
    /usr/bin/time -l $X -in $M -lib $LIB -rt_mode minutes -rt_window 60 -top_fragments 0 -mz_tol_ms2 5.9051 -mz_tol_ms1 5.9051 -scheme B -threads $t \
        -out /tmp/odiaB.parquet -out_stats odiaB_t${t}_r$rep.json > odiaB_t${t}_r$rep.log 2>&1 || log "  ours B failed"
    rm -rf /tmp/odiaA.parquet /tmp/odiaB.parquet /tmp/odiaA.axes.parquet /tmp/odiaB.axes.parquet
  done; done
  for rep in 1 2; do
    log "full entrapment library (7.0 M precursors), $PHYS threads, repeat $rep"
    /usr/bin/time -l $X -in $M -lib $FULL -rt_mode minutes -threads $PHYS -out /tmp/full.parquet -out_stats full_r$rep.json > full_r$rep.log 2>&1 || log "  full-library run failed (memory? see full_r$rep.log)"
    rm -rf /tmp/full.parquet /tmp/full.axes.parquet
  done
  if [ "${RUN_OSW:-0}" = 1 ]; then
    log "OpenSwathWorkflow, $PHYS threads (this takes a while)"
    /usr/bin/time -l $WORK/OpenMS/build/macos-arm64-release/bin/OpenSwathWorkflow -in $M -tr $WORK/input/lib_openswath.tsv \
        -out_chrom /tmp/osw.xic -out_features /tmp/osw.featureXML -rt_extraction_window 120 -mz_extraction_window 11.8102 \
        -mz_extraction_window_unit ppm -mz_extraction_window_ms1 11.8102 -mz_extraction_window_ms1_unit ppm -ms1_isotopes 2 \
        -Calibration:auto_irt:enabled false -threads $PHYS > osw_t$PHYS.log 2>&1 || log "  OpenSWATH failed"
    rm -f /tmp/osw.xic /tmp/osw.featureXML
  fi
  # summary: wall seconds and peak RSS per run ("real" and "maximum resident set size" from /usr/bin/time -l)
  { printf 'run\twall_s\tpeak_rss_gib\n'
    for f in odiaA_*.log odiaB_*.log full_*.log osw_*.log; do [ -s $f ] || continue
      w=$(grep -E '^\s*[0-9.]+ real' $f | awk '{print $1}'); r=$(grep 'maximum resident set size' $f | awk '{print $1}')
      printf '%s\t%s\t%s\n' "${f%.log}" "$w" "$(echo "$r" | awk '{printf "%.2f", $1/1073741824}')"; done; } | tee summary.tsv
  cd ..
}

pack() {
  T=$WORK/odia-macos-benchmark-$(date +%Y%m%d-%H%M).tar.gz
  tar czf $T -C $WORK results
  log "results packed: $T  (send this file back)"
}

case ${1:-all} in
  all) deps; openms; tool; inputs; bench; pack ;;
  deps|openms|tool|inputs|bench|pack) $1 ;;
  *) echo "usage: $0 [all|deps|openms|tool|inputs|bench|pack]"; exit 1 ;;
esac
