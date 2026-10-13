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
