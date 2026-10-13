# Handoff: DIATraceExtractor benchmark on the MacBook Pro (macOS, Apple silicon)

For a local agent on the laptop. Goal: make `DIATraceExtractor` run cleanly on macOS, run the benchmark, and send
the results tarball back. Fix the code where macOS needs it; keep every fix portable (Linux is the primary
platform; the cluster build must stay byte-identical in its output). Commit fixes on a branch `macos` and push.

## 1. State on the laptop (2026-10-13)

- Work directory `~/odia-bench/` (created by `scripts/bench_macos.sh`, which lives in the repo and in `/tmp/bench/`).
  - `OpenMS/`: OpenMS develop at commit 10b0a7af09, built with `cmake --preset macos-arm64-release`
    (`OpenMS/build/macos-arm64-release/`, vcpkg tree under `vcpkg_installed/arm64-osx-dynamic-sysroot/`,
    `vcpkg.json` extended with `boost-json` and `boost-program-options`). Built fine.
  - `dia-trace-extract-prototype/`: the tool, checked out at tag `V0.2.2` (commit 48fd5b0), submodule
    `external/mzpeak` (okohlbacher/mzpeak-openms, daa8d1d). Builds (`build/DIATraceExtractor`).
  - `input/`: empty or partial. The transfer failed because the hive gateway was under maintenance.
- Script phases: `bash bench_macos.sh deps|openms|tool|inputs|bench|pack` (idempotent; `all` runs them in order).
  Settings via environment: `WORK`, `CLUSTER` (default `kohlbach@data`), `JUMP` (default
  `kohlbach@hive.cs.uni-tuebingen.de`; use `kohlbach@sshgw.cs.uni-tuebingen.de` while hive is down), `THREADS`,
  `RUN_OSW=1`.

## 2. The open defect: the binary aborts at exit

`./build/DIATraceExtractor --help` prints the full help and then dies with `Abort trap: 6`. The ctest suite fails
for the same reason (the tests run the binary). Work is done before the abort, so outputs are probably complete,
but the exit code is non-zero and the benchmark script marks every run as failed.

What is known:
- `lldb --batch -o run -- ./build/DIATraceExtractor --help` stops at an `exec`: `main()` re-executes the binary
  once when `OMP_WAIT_POLICY` is unset (src/DIATraceExtractor.cpp, end of file; on macOS via
  `_NSGetExecutablePath`). Run lldb with `OMP_WAIT_POLICY=PASSIVE` set to skip the re-exec and reach the abort:
  `OMP_WAIT_POLICY=PASSIVE lldb --batch -o run -o "bt 30" -- ./build/DIATraceExtractor --help`
- First check whether the re-exec itself is the problem: `OMP_WAIT_POLICY=PASSIVE ./build/DIATraceExtractor --help
  > /dev/null; echo $?`. If this exits 0, disable the re-exec on `__APPLE__` (let the script export the variable)
  and you are done with this item.
- Otherwise the likely suspects are static/thread-local destructors at exit:
  - `src/ArrowPool.cpp`: `RecyclingPool` (function-static `arrowPool()`), its `thread_local Local` front-end cache
    whose destructor hands blocks back to the pool (`~Local` sets `dying`, then `putLarge_`), and `ReleaseUnused`
    (`munmap` of cached blocks). Destruction order of the TLS object vs the pool static differs between libc++ and
    glibc. A `std::terminate` from a destructor or a `munmap` of an already-released block would abort.
  - OpenMP runtime (Homebrew libomp) teardown with threads parked in a passive wait.
  - Arrow's default memory pool released after ours (`releaseFreedMemory()` in src/DIATraceExtractor.cpp).
  - The crash report in `~/Library/Logs/DiagnosticReports/DIATraceExtractor-*.ips` names the aborting frame if
    lldb does not.
- Acceptable fixes: make the destructors order-independent (e.g. detach the thread-local cache from the pool at
  exit via `std::atexit` ordering, or leak the pool deliberately on exit, `new`'d and never destroyed), or skip the
  re-exec on macOS. Not acceptable: catching the abort or changing the extraction.

## 3. Other macOS specifics already handled (V0.2.2)

Guarded with `#ifdef __linux__` / `__APPLE__`: `MAP_NORESERVE`, `MAP_POPULATE`, `MADV_HUGEPAGE`, `/proc/self/status`
and `/proc/self/task` reads (RSS via `mach task_info` on macOS), `RUSAGE_THREAD`, `prctl`, `malloc_trim`,
`mallinfo2`, the `/proc/self/exe` re-exec. `ru_maxrss` is bytes on macOS, KiB on Linux (handled).

## 4. Inputs (6.5 GB, from the cluster)

Source: `/ceph/ibmi/abi/oliver/Claude/ODIA-V3/benchmark-data/laptop-inputs/` on any IBMI node (symlinks; copy
with `rsync -L`). Files: `LFQ_Astral_DIA_15min_50ng_Human_01.mzML` (5.8 GB),
`proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01_ids_run01.parquet` (14 MB, the shared ID-restricted
library), `proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01.parquet` (944 MB, the full library),
`lib_openswath.tsv` (145 MB, OpenSWATH's copy of the shared library).

```bash
JUMP=kohlbach@sshgw.cs.uni-tuebingen.de bash /tmp/bench/bench_macos.sh inputs     # or hive when it is back
```

## 5. The benchmark (what the numbers are compared against)

Cluster reference (2 × AMD EPYC 9654, Linux, V0.2.1 binary, run 01, shared library of 82,333 precursors, all
fragments, `docs/10-release-report-v0.2.md` §6): ours A 4.1 s at 4 threads, 2.5 s at 8, 1.7 s at 16, 1.3–1.4 s at
32–64, peak RSS 2.4–2.7 GiB; full 7 M-precursor library at 64 threads 9.3 s, 4.8 GiB; OpenSwathWorkflow 227 s at
4 threads, 182 s at 64.

`bash bench_macos.sh bench` runs, with `OMP_WAIT_POLICY=PASSIVE`:
- ours A (defaults) and ours B (`-scheme B -mz_tol 5.9051 -mz_tol_ms1 5.9051`) on run 01 with the shared library,
  `-rt_mode minutes -rt_window 60 -top_fragments 0`, at 1, 2, 4, … physical cores plus the performance-core
  count, two repeats each, timed with `/usr/bin/time -l` (wall from "real", peak RSS from "maximum resident set
  size", bytes);
- the full library (`-rt_mode minutes`, top 6 fragments) at all physical cores, twice (about 5 GB RAM);
- with `RUN_OSW=1`, one OpenSwathWorkflow run at all physical cores (10–30 min).
It writes `~/odia-bench/results/` (`machine.txt` with `sw_vers`, CPU, core and memory counts; one `.log` and
`.json` per run; `summary.tsv`). `bash bench_macos.sh pack` tars it into `~/odia-bench/odia-macos-benchmark-<date>.tar.gz`.

Sanity checks before trusting the numbers: every `odiaA_*.log` ends with a `Total:` line and `DIATraceExtractor
took`; the `.json` files exist; exit codes are 0 (after the abort is fixed); the laptop is on mains power and not
thermally throttled (repeat 2 should not be slower than repeat 1 by more than ~10%). Also record whether the
mzML lives on the internal SSD.

## 6. Deliverables

1. Branch `macos` pushed to okohlbacher/dia-trace-extract-prototype with the fix(es), each commit explaining the
   macOS behaviour it works around; Linux must still build and pass `ctest` (the cluster session will verify).
2. The results tarball (section 5), plus the `bench.log` of the run.
3. A short note: what aborted and why, anything else that differed from Linux (warnings, ctest results, the
   OpenMP runtime used), and the laptop's core/memory configuration.

## 7. Repository pointers

- Build: `README.md`; options: `docs/EXTRACTION.md`; results and context: `docs/10-release-report-v0.2.md`.
- The script: `scripts/bench_macos.sh` (main branch; `--force`-fetches tags, prints compile errors on failure).
- Tests: `ctest --test-dir build` (7 tests; need the OpenMS build's `bin/OpenSwathWorkflow` and `TextExporter`,
  found through `OpenMS_DIR`).
- Cluster side contact: the ODIA-V3 Claude session on the IBMI cluster (this document's author) picks up the
  branch and the tarball and folds the numbers into the report.
