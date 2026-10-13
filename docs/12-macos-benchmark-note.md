# macOS benchmark note (MacBook Pro, Apple M4 Max, 2026-10-13)

Reply to docs/11-macos-benchmark-handoff.md. Branch `macos` (commits 5525cdb, fff45ea, d7a5ff1 on top of V0.2.2);
results tarball `odia-macos-benchmark-20261013-1027.tar.gz` and `bench.log` on Ceph under
`ODIA-V3/results/macos-benchmark/`.

## What aborted, and why

Two separate defects, neither an exit-time destructor race of the kind the handoff suspected first.

1. **`--help` → "Abort trap: 6"**: an uncaught `ElementNotFound` from `TOPPBase::printUsage_`. Since
   `calibration:mass_model` became a non-advanced option (2026-10-12), plain `--help` reaches the `calibration`
   subsection, which the tool never registered with `registerTOPPSubsection_`. `--helphelp` aborted on every build,
   Linux included (verified on the cluster binary). The cluster binary of 2026-10-11 predates the change, which is why
   it still answered `--help` with exit 0. Fix: register the subsection (5525cdb). Not macOS-specific.
2. **Every real run → "Trace/BPT trap: 5" after all output was written** (`libc++abi: terminating due to uncaught
   exception of type std::system_error: mutex lock failed: Invalid argument`). Crash report: `RecyclingPool::Local::~Local`
   ← `dyld::ThreadLocalVariables::finalizeList` ← `_pthread_exit`. OpenMP workers exit after `main()` returns; their
   `thread_local` caches hand blocks back through the pool's mutex, but the function-static pool was already destroyed.
   glibc tolerates locking a destroyed mutex, libc++ throws. Fix: the pool is `new`'d once and never deleted (fff45ea);
   extraction unchanged, ctest 7/7 on macOS.

The `OMP_WAIT_POLICY` re-exec is not involved (both aborts reproduced with the variable set).

## Other differences from Linux

- ctest: `parity_with_openswath` and `determinism_compare` were "Not Run" because `tools/pyenv` did not exist and
  Homebrew's python refuses `pip install --user` (PEP 668). `bench_macos.sh tool` now creates the venv with `uv`.
- BSD `date` has no `-Is` (machine.txt line fixed; power source recorded).
- OpenMP runtime: Homebrew libomp (LLVM), not libgomp. High system time at 16 threads (19–22 s sys for a 3.3 s run) is
  macOS page-fault/mmap cost of the ingest buffers; worth a look if the laptop numbers matter beyond this note.
- No GPU path exists in the tool; all runs are CPU/OpenMP.

## Machine

Apple M4 Max, 16 physical cores (12 performance + 4 efficiency), 16 logical, 128 GiB, macOS 26.6.2, inputs on the
internal SSD. **The whole series ran on battery (58 % → 45 %, no Low Power Mode)**; repeat 2 was never slower than
repeat 1 by more than 6 %, so no throttling is visible, but a mains re-run is one `bash bench_macos.sh bench` away.
Tool V0.2.2-2-gfff45ea, OpenMS 10b0a7af09, `OMP_WAIT_POLICY=PASSIVE`.

## Results (run 01, shared library of 82,333 precursors, all fragments; wall s / peak RSS GiB, mean of 2 repeats)

| threads | ours A | ours B |
|---|---|---|
| 1 | 41.4 / 1.98 | 41.3 / 2.05 |
| 2 | 5.4 / 1.94 | 5.2 / 2.01 |
| 4 | 4.4 / 1.97 | 4.3 / 2.04 |
| 8 | 3.9 / 2.02 | 3.9 / 2.09 |
| 12 | 3.8 / 2.06 | 3.8 / 2.13 |
| 16 | 3.3 / 2.11 | 3.4 / 2.18 |

Full 7.0 M-precursor library, 16 threads: 14.9 s and 15.7 s, 3.9 GiB. OpenSwathWorkflow, 16 threads: 120 s, 12.7 GiB.
Cluster reference (2 × EPYC 9654, docs/10 §6): ours A 4.1 s at 4, 2.5 s at 8, 1.3–1.4 s at 32–64; full library 9.3 s at
64; OpenSWATH 227 s at 4, 182 s at 64.

**Anomaly to look at:** the 1-thread runs are 8× slower than 2 threads, entirely in `stage_ingest` (38.0 s wall at 1
thread vs 3.6 s at 2 and 2.9 s at 16; extraction scales normally: 2.7 → 1.4 → 0.25 s). `-threads 1` evidently takes a
different, serial ingest path (the per-spectrum parser?) rather than the parallel one with one worker. Not a macOS
issue as far as I can tell; the cluster series starts at 4 threads so it was never visible there.

## Parallel-efficiency analysis of the laptop run (2026-10-13, after the benchmark)

Method (parallel-efficiency skill): phase table from the `-out_stats` files, speed-up and Karp–Flatt serial fraction,
then attribution with `sample` stack histograms, the tool's `ODIA_PROFILE=1` counters, a read-throughput
micro-benchmark on the input file, and an `OMP_WAIT_POLICY` control. Every change was accepted only with
byte-identical traces and axes against the V0.2.3 binary at 1 and 16 threads (`scripts/compare_parquet.py`) and
ctest 7/7. Commit 0ae2482 on `macos`.

### Before: extraction scaled, ingest did not

| threads | ingest wall / CPU (s) | active cores | extract wall / CPU (s) | active cores | total wall | user / sys (s) |
|---|---|---|---|---|---|---|
| 1 | 38.2 / 38.3 | 1.0 | 2.71 / 2.71 | 1.0 | 41.3 | 40.5 / 1.1 |
| 2 | 3.66 / 4.66 | 1.3 | 1.39 / 2.78 | 2.0 | 5.3 | 6.0 / 2.0 |
| 4 | 3.47 / 6.21 | 1.8 | 0.73 / 2.87 | 3.9 | 4.4 | 6.2 / 3.5 |
| 8 | 3.39 / 11.4 | 3.4 | 0.37 / 2.92 | 7.9 | 3.9 | 6.4 / 8.7 |
| 16 | 2.89 / 24.1 | 8.3 | 0.25 / 3.50 | 14.3 | 3.2 | 7.3 / 21.1 |

Extraction: 14.3 active cores at 16 threads, nothing to do. Ingest: 89 % of the wall at 16 threads, flat from 2 to
16 threads while its CPU time grew 5× and system time 2 → 21 s. Karp–Flatt serial fraction of the whole run 0.26 at
16 threads against the 2-thread point.

### Three causes, in the order they were found

1. **Small `read()` syscalls serialised by the kernel (macOS only).** Stack samples at 16 threads put the busy leaf
   in `read` (2857 samples) ahead of the decoder (1221), via `basic_filebuf::underflow` → `fread`. libc++'s
   `ifstream::read` refills a 4 KiB buffer with one syscall per 4 KiB; libstdc++ reads large requests directly, so
   the cluster never saw it. The two passes over the 5.8 GB file (mark scan, chunk parse) made ~2.9 M syscalls on one
   descriptor. Micro-benchmark (`pread` of the file from the page cache, 16 threads): 4 KiB blocks 22.5 s sys and
   1.58 s wall; 1 MiB blocks 0.6 s sys and 0.05 s wall. The tool's 21 s of system time matched. Refuted on the way:
   page faults (0.08 M, same at every thread count), OpenMP waiting (`OMP_WAIT_POLICY=ACTIVE` moved CPU, not wall),
   raw read bandwidth (132 GB/s at 1 MiB). Fix: `pread` into the existing buffers. System time 21 → 1.3 s.
2. **`-threads 1` used a different parser.** The chunked fast parser was gated on `decode_threads > 1`; one thread fell
   back to the OpenMS XML parser (38 s instead of 2.8 s). Fix: the chunked parser at one thread too.
3. **The in-order commit gate.** After 1 and 2, ingest still took 1.06 s wall for 2.2 thread-s of work: the profile
   counter `ingest.3_chunks.ahead_wait` was 13.4 thread-s, i.e. each of 16 workers waited 0.84 s at
   `orderedParallel`'s look-ahead bound of 32 chunks (commit itself: 12 ms total). A parked fast-path chunk holds only
   metadata, so the bound is unnecessary there. Sweep of the bound at 16 threads: 32 → 1.06 s, 64 → 0.62 s,
   128 → 0.45 s, 512 → 0.33 s, unbounded → 0.28 s; peak RSS +40 MB. Fix: unbounded for the fast path, the OpenMS
   fallback keeps the bound.

### After

| threads | real (s) | user / sys (s) | ingest wall / CPU | extract wall | total wall | peak RSS GiB |
|---|---|---|---|---|---|---|
| 1 | 6.52 | 5.53 / 0.78 | 2.82 / 2.87 | 2.86 | 6.13 | 1.93 |
| 2 | 3.19 | 5.60 / 0.77 | 1.45 / 2.93 | 1.43 | 3.15 | 1.95 |
| 4 | 1.69 | 5.68 / 0.84 | 0.75 / 3.00 | 0.73 | 1.64 | 1.99 |
| 8 | 0.96 | 5.85 / 0.96 | 0.39 / 3.13 | 0.38 | 0.91 | 2.05 |
| 12 | 0.75 | 6.20 / 1.16 | 0.29 / 3.43 | 0.28 | 0.70 | 2.08 |
| 16 | 0.70–0.73 | 6.7 / 1.3 | 0.27 / 3.75 | 0.27 | 0.65 | 2.17 |

Speed-up 1 → 16 threads 9.4× (12 performance + 4 efficiency cores; the 2 → 16 speed-up is 4.8× for 8× the threads).
Ingest 2.82 → 0.27 s (10.4×). Against the cluster (2 × EPYC 9654): 0.70 s at 16 laptop threads vs 1.3–1.4 s at 32–64
cluster threads with the V0.2.1 binary; the cluster has not been re-measured with this commit.
Full library (7.0 M precursors), 16 threads: 15.3 → 13.8 s, 4.0 GiB; ingest 0.34 s, extraction 9.55 s at 11.6
active cores, ~3.9 s in calibration panel and final drain.

### Still open

- Linux must be re-measured with 0ae2482: cause 1 is macOS-specific, causes 2 and 3 are not (the look-ahead gate
  should also show on the cluster at 32–64 threads; the counter to read is `ingest.3_chunks.ahead_wait`).
- Full-library run: extraction at 11.6 of 16 cores and ~3.9 s outside ingest/extraction are the next targets.
- Process start and exit add 0.05–0.4 s to `real` beyond `total` (dyld of the OpenMS/Arrow dylibs, unmapping 2 GiB).
- Measurements were taken on battery; repeat spread ≤ 6 %.
