# Adversarial review: DIATraceExtractor (ODIA-V3), commit d0109d7

Review only. Do not modify, create or delete any file. Report findings as `file:line`, each with a concrete
failing input or state, a severity (high / medium / low), and whether it would actually bite on real data or is
theoretical. If something is fine, say so in one line rather than padding. Rank the findings, most severe first.

## What the program is

A C++23 OpenMS TOPP tool that extracts DIA traces (fragment XICs and MS1 isotope XICs) for every precursor of a
predicted spectral library from one DIA run, after calibrating the retention-time map and the mass offsets and
tolerances from the run itself. No scoring. Sources: `src/*.cpp`, `src/*.h`; build `CMakeLists.txt`; tests
`tests/CMakeLists.txt`; user documentation `docs/EXTRACTION.md` (and `README.md`). The mzPeak library under
`external/mzpeak` is third-party: do not review it, only our use of it.

## Invariants the code must keep

1. The output Parquet is byte-identical for any thread count (ordered commit of work units; `tests/`).
2. m/z is uint32 fixed point in 1e-5 Th (`toFixed` = lround); every window bound is computed with it, so a
   centroid is inside a window iff `lo <= mz <= hi` in fixed point.
3. The run grid: one row per isolation window (same target within 1e-6 Th and same edges), one column per DIA
   cycle; a cycle ends when a window repeats; `cycle_rt` = mean of the cycle's MS2 times; MS1 spectra on their
   own axis (`-ms1_axis scans`, default) or interpolated at `cycle_rt`.
4. Peaks live in one `PeakPool` (mmap MAP_NORESERVE); `claim(n)` is an atomic add; a decoder writes only the
   range it claimed; cells of the grid are (begin, count) into the pool.
5. Library RT -> run seconds via `RtMap`; DIALibGen tuned libraries carry run minutes (`-rt_mode minutes`).
6. Calibration (`src/Calibration.cpp`): panel = `panel_fraction` (5%) of target precursors with >= 4 fragments,
   spread over library RT; stage 1 searches the whole run with strict criteria (>= 5 fragments at the apex,
   mean pairwise Pearson >= 0.90 over +-4 cycles, cosine of sqrt intensities vs library >= 0.85), stage 2 within
   +-max(4 sigma, 30 s) of the stage-1 LOWESS map with relaxed criteria (4, 0.80, 0.75). Mass model: apex mass
   errors in ppm relative to the UNCORRECTED library m/z; offset = median; width = 1.4826 x MAD; tolerance =
   `accuracy_sigma` (6) x width. The mass search starts at +-50 ppm and later passes search +- 3 x tolerance
   around the measured offset (the bounds are shifted by the offset; the recorded errors stay relative to the
   uncorrected target). Mass model from the anchors that pass the strict criteria when >= 200 of them exist.
7. Extraction (`src/Extractor.cpp`): a unit = up to 256 precursors of ONE isolation window sorted by RT; the
   merge join `join<Sum>` must give exactly the same values as a `lower_bound` per target and cell followed by
   max (or sum) over `mz <= hi`.
8. mzPeak (`loadMzPeak` in `src/RunGrid.cpp`): one `MzPeak::Spectra` per thread; `partitions(4 x threads)` cover
   every spectrum once; the sink gets (index, mz span, intensity span) valid only during the call; metadata is
   read on the main thread before decoding.

## What I most want attacked (numbered)

1. `Extractor.cpp` `join`: any input where the sliding active set, the `seek` bucket jump or the ordering of
   targets with equal `lo` produces a value different from the naive per-target search (duplicate m/z values,
   empty cells, a target whose window lies above every peak of a cell, spans that open and close in the same
   cell, records with zero cycles, MS1 spans on the scan axis).
2. `Calibration.cpp`: the iterative mass search (`narrowed`, the round loop, `MassSearch` offsets applied in
   `fragmentBounds` and the MS1 search) -- sign errors, an offset applied twice, a window that cannot widen, a
   convergence test that never stops or stops too early; `massModel` on degenerate input (all errors equal, 1
   point, n < 50); `examineWholeRun` tile buffers and the OpenMP thread count; the adaptive `bins` and the
   `kept.size() >= 20` mode-filter skip; `robustFit` with < 10 usable anchors.
3. `RunGrid.cpp`: `PeakPool` capacity estimate (file bytes / 2, x4 for non-mzML) -- an input that exhausts it;
   `Router` cycle closing and lane padding; `loadChunked` chunk boundaries (a `<spectrum>` split across chunks,
   the last chunk, headers); `loadMzPeak` indexing `meta[i]` from the sink, empty spectra, exceptions thrown in a
   worker, spectra with `ms_level` 0.
4. `MzMLFast.cpp`: base64/zlib decoding edge cases, 32-bit vs 64-bit arrays, `defaultArrayLength` 0, a missing
   isolation window, the unit of `scan start time` (minutes vs seconds), attributes in unexpected order.
5. `Scheduler.cpp` / `Pipeline.h`: deadlock or starvation (in-order issue, the unwritten cap, the ordered
   commit window), exception propagation out of a worker, `ahead_wait` on error.
6. `ArrowPool.cpp`: the thread-local front end (`Local`): destruction order at thread and process exit, the
   `dying` flag, statistics batching making `bytes_allocated()`/`max_memory()` wrong by more than threads x 1 MiB,
   `Reallocate` within a size class, `ReleaseUnused` while threads hold cached blocks.
7. `TraceWriter.cpp` `encode`: the `Reserve`/`ReserveData` estimates (they may under-estimate; must never make
   the output wrong), `skip_empty`, log8 encoding, `expand_rt`; parallel `close()`.
8. `DIATraceExtractor.cpp`: option interplay -- `-mz_tol 0` or `-mz_tol_ms1 0` with `-mass_calibration off`,
   `-rt_mode auto` when calibration throws, the graceful fallback when calibration finds no anchors (does
   anything dereference the empty `cal`?), `-ids` restricting extraction but not the panel, the stats JSON.
9. **Documentation congruence:** compare `docs/EXTRACTION.md` and `README.md` against the code: every default,
   threshold and claimed behaviour (window assignment, aggregation, isotope spacing, calibration criteria and
   numbers, output columns and footer keys, the parallel layout). List every statement that the code does not
   support.
10. Anything else: undefined behaviour, integer overflow (uint32 indices, 7 M precursors x 6 fragments, 63 M
    traces, 7.7 G points), unchecked narrowing casts, races.
