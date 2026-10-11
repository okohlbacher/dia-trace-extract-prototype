# Adversarial review: DIATraceExtractor (ODIA-V3) at d0109d7 -- Claude

Scope: `src/*` (not `external/mzpeak`), `CMakeLists.txt`, `tests/CMakeLists.txt`, `docs/EXTRACTION.md`, `README.md`.
Method: full read of every source, code-path tracing, two checks against the OpenMS sources in `OpenMS/`
(precursor m/z selection in `MzMLHandler.cpp`, LOWESS failure modes), and a randomized differential test of the
extraction merge join against a naive per-target search (built from the real `Extractor.cpp` + `BufferPool.cpp` +
the `PeakPool`/`buildIndex` code of `RunGrid.cpp`, under ASan/UBSan; harness in the session scratchpad, nothing
written to the repository).

Findings are ranked most severe first. "Real" = would bite on data a user can plausibly feed in; "theoretical" =
needs contrived input or options.

---

## Findings

### F1. The two mzML parsers key isolation windows on different m/z values, so the grid (and the output) can depend on `-threads` -- HIGH, real for some converters, latent for all

`src/MzMLFast.cpp:133-136,179-181` take the window target from `MS:1000827` (isolation window target) and fall back
to `MS:1000744` (selected ion m/z) only when 827 is absent. The OpenMS path -- `decode()` in
`src/RunGrid.cpp:81-84`, which is what runs for `-threads 1` (`ingest_threads = min(threads,32) = 1` ->
`decode_threads > 1` is false at `RunGrid.cpp:673`), for `ODIA_MZML_PARSER=openms`, for every chunk the fast
parser rejects (`RunGrid.cpp:530-539`), and for `.raw` -- uses `Precursor::getMZ()`. OpenMS sets that to the
**selected ion m/z** whenever it differs from the isolation target
(`OpenMS/src/openms/source/FORMAT/HANDLERS/MzMLHandler.cpp:1807-1820`, default
`PeakFileOptions::precursor_mz_selected_ion_ = true`, `PeakFileOptions.h:264`), and the window edges become
`selected +- offsets` (`RunGrid.cpp:83-84`).

Consequences on a file where the two values differ (common for files whose converter writes the monoisotopic or
scan-filter precursor as selected ion and the window centre as target; identical in the Thermo/msconvert files
used so far, which is why `determinism_t1`/`determinism_t8` pass):
- `-threads 1` vs `-threads >= 2`: different `lane.target/lower/upper` -> different lane order (sorted by target,
  `RunGrid.cpp:149-151`), different `window` column, different nearest-centre assignment (`Extractor.cpp:50-58`),
  different traces. Invariant 1 (byte-identical for any thread count) is violated.
- A run where one chunk falls back to OpenMS (one spectrum with a missing time unit, an integer array, numpress) and
  the rest does not: the fallback chunk's spectra land in *new* lanes (`windowId_`, `RunGrid.cpp:171-187`, matches
  on target within 1e-6 and edges), the "window repeats" rule no longer sees the repeat, the cycle containing that
  chunk never closes until the next genuine repeat and the padded lanes get an empty cell -- a corrupt time axis
  around that chunk with no diagnostic (only `missing_cells`/window count move).

Also inconsistent between the two paths: multiplexed spectra (fast path keeps the *last* `MS:1000827`,
`MzMLFast.cpp:133`; OpenMS keeps the first precursor) -- theoretical.

Fix: pick one rule in both paths (either read 827 preferentially in `decode()` via the meta value
"isolation window target m/z", or set `getOptions().setPrecursorMZSelectedIon(false)` on every `MzMLFile` used in
`RunGrid.cpp:535,693`), and add a determinism test on a file where selected ion != target.

### F2. `-rt_mode auto -mass_calibration off` still applies the measured mass offsets and silently ignores `-mz_offset*` -- MEDIUM, real

`src/DIATraceExtractor.cpp:408-409`: `eopt.ms2_ppm_offset = cal ? cal->ms2_offset_ppm : getDoubleOption_("mz_offset")`.
`cal` is populated whenever the panel future ran (`mode == "auto" || mass_auto`, line 284), so with
`-rt_mode auto -mass_calibration off -mz_offset -2.5 -mz_tol 10 -mz_tol_ms1 5` the extraction uses the calibrated
offsets, not `-2.5`/`0`. The option text (line 154) and README line 55 promise the opposite. The footer
`odia.mz_offset_ppm` shows the calibrated value, so it is visible but wrong. Fix: `mass_auto && cal ? ... : option`.

### F3. The stage-1 mode filter is switched off exactly when it is needed -- MEDIUM, real on short runs / mismatched libraries

`src/Calibration.cpp:523-529`: `bins = min(20, n/10)`, `filter = kept.size() >= 20`; when fewer than 20 anchors sit
within 60 s of the mode curve, *all* stage-1 anchors are used. With 50-80 stage-1 anchors of which 15-19 are true
(a 10-min run, a library of the wrong organism, a wrong `-rt_mode` unit), `robustFit` (`Calibration.cpp:456-488`)
fits LOWESS to a majority of false anchors; its `sigma = 1.4826 x median|residual|` is then large, `cut = 3 sigma`
rejects nothing, `map1` is garbage, the stage-2 window `max(4 sigma1, 30 s)` (line 568) is hundreds of seconds, the
relaxed search finds >= 50 false anchors and the tool finishes with a wrong RT map and no error. Only
`rt_residual_p95` in the log/stats betrays it. The intended behaviour ("so that scattered false anchors cannot pull
the curve", EXTRACTION.md:99-100) is inverted for small samples. Suggest: keep the filter whenever `kept.size()`
>= 10 (the `robustFit` minimum) and warn, or fail when the kept share is < 50 %.

### F4. `-ids` with a non-Parquet library shrinks the calibration panel to 5 % of the *filtered* rows -- MEDIUM, real

`src/Library.cpp:729` loads TSV/PQP/TraML/flat-Parquet libraries whole **with** `keep_ids`/`targets_only` applied,
so `LibraryReader::rows()` is the filtered count. `src/DIATraceExtractor.cpp:290-293` derive `panel_size` and
`keep_every` from it: `-ids` with 1,000 ids on a 7 M-row TSV gives `panel_size = 50`, `keep_every = 10`
(the panel reader then decodes 700 k rows, no `keep_ids`), `copt.panel = 50` (line 335-337) -> at most 50 panel
precursors -> `stage1.size() < 50` throws (`Calibration.cpp:515`) -> with the default `-mz_tol 0` the run exits
`INPUT_FILE_CORRUPT`. Contradicts EXTRACTION.md:87-89 / option text line 170-171 ("never restricted by -ids").
For compact Parquet `rows()` is the file row count and the claim holds.

### F5. Calibration failures that are not `std::runtime_error` bypass the graceful fallback -- LOW/MEDIUM, theoretical-to-rare

`src/DIATraceExtractor.cpp:359-367` catch only `std::runtime_error`. `RtMap::fromPoints` (`src/RtMap.cpp:86`) calls
`TransformationDescription::fitModel`, whose LOWESS throws `OpenMS::Exception::IllegalArgument`
(`TransformationModelLowess.cpp:45-48`) and can throw other OpenMS exceptions for degenerate data (all anchors at
one library RT, span too small); `std::bad_alloc` from the stage-1 tiles and `std::logic_error` also escape. Those
reach `main_` (line 234-241), which rethrows OpenMS exceptions -> the tool aborts even when the user supplied
`-mz_tol/-mz_tol_ms1` and a non-auto `-rt_mode`, i.e. when calibration was optional.

### F6. `massModel` on an empty or tiny sample: tolerance 0 ppm or 0.1 ppm, silently -- LOW/MEDIUM, rare

`src/Calibration.cpp:64-80`: `v.empty()` returns `accuracy = 0` (the 0.1 floor at line 78 is after the early
return); n = 1..few gives `sigma = 0` -> `accuracy = 0.1 ppm`. No sample-size check anywhere (the `>= 200` at
line 547 only chooses *which* anchors, not whether there are enough). MS1 is the exposed level: it contributes at
most one point per anchor (`Calibration.cpp:220-229`), so a run whose MS1 scans exist but carry (almost) no
centroids within +-50 ppm of the panel precursors (profile MS1 that was emptied, a narrow MS1 scan range, MS1
scans of a different polarity) yields `ms1_accuracy_ppm` of 0 or 0.1, which `DIATraceExtractor.cpp:403-406` adopts
as the MS1 tolerance without a warning: all MS1 traces come out empty while the log says "measured". Add a floor on
`m.n` (e.g. 50) below which the level falls back to the search width or errors out, and move the 0.1 floor before the
early return.

### F7. `docs/EXTRACTION.md` and `README.md` statements the code does not support -- LOW each (item 9)

| doc | statement | code |
|---|---|---|
| EXTRACTION.md:65 | output holds "the target m/z after correction" | `src/TraceWriter.cpp:283`: `target = lib.fragment_mz[f]` / `precursor_mz + k*C13/z` -- **uncorrected**; the offset is applied only in `Extractor.cpp:271,290` and never written per row (only the footer's `odia.mz_offset_ppm`). A reader computing "window = target_mz +- tol" from the file gets the uncorrected window. |
| EXTRACTION.md:142 | "The library is never loaded whole" | True only for compact Parquet. `src/Library.cpp:729` loads TSV/PQP/TraML/flat Parquet whole (then slices). Also `parts_ahead = max(4, 2 x sources)` (`DIATraceExtractor.cpp:454`, `Scheduler.cpp:624`) lets up to 2 x (row groups) parts be resident; with 64 k-row row groups that is the whole library. Fine for the shipped libraries (4 and 7 row groups). |
| EXTRACTION.md:86 | panel = "5 % of the library's target precursors with at least 4 fragments" | `DIATraceExtractor.cpp:290-292`: 5 % of **all** library rows (decoys and < 4-fragment entries included), then capped by the eligible count in `Calibration.cpp:505-507`. For a library with 50 % decoys the panel is 10 % of the targets. |
| EXTRACTION.md:99 | "the library RT range is cut into 20 bins" | `Calibration.cpp:414-423`: equal-**count** quantile bins, and `bins = min(20, n/10)` (line 523) -- 20 only from 200 anchors; the filter is skipped entirely below 20 kept anchors (F3). Not mentioned: anchors are kept within 4 x `mode_bandwidth` = 60 s. |
| EXTRACTION.md:112, 128 | "the anchors that pass the strict criteria" feed the mass model; "Every threshold above is a `-calibration:*` option" | `Calibration.cpp:542-544`: strict = correlation and cosine only, **not** `strict_min_fragments`. Not options: the 6 fragments per panel precursor (`copt.top_fragments` is never set from an option, `DIATraceExtractor.cpp:338-357`), the 200-anchor floor, the 50-anchor minimum, the 25 % convergence test, the 0.1 ppm floor, the 2 x half-window candidate spacing. |
| EXTRACTION.md:128, 141 | "about one second on 64 threads", "read in 0.3 s (mzML) ... at 64 threads" | Ingest is capped at 32 threads by default (`DIATraceExtractor.cpp:282`), stage 1 at 32 (`Calibration.cpp:291`). The timings may be right; the thread count is not. |
| EXTRACTION.md:120-123 | "The mass search then narrows to +-3 x the tolerance ... and the anchors are searched again" | Correct, but the loop (`Calibration.cpp:571-591`) runs up to 3 passes and stops on a < 25 % width change -- undocumented; the stop test ignores a moving offset (F11). |
| README.md:57, 60-62 | "one row per trace on the global cycle axis"; snippet indexes `cycle_rt[row.cycle_begin ...]` for `t.iloc[0]` | MS1 rows are on the MS1 scan axis by default (`TraceWriter.cpp:273-277`, lane -2). The snippet is only right because row 0 happens to be a fragment; EXTRACTION.md:78 filters `ms_level == 2`, README does not. |
| README.md:55 | "`-mz_offset*` set them" | Ignored whenever `-rt_mode auto` (F2). |
| EXTRACTION.md:87-89, option text | panel "never restricted by -ids" | Size is, for non-Parquet libraries (F4). |
| EXTRACTION.md:153 "byte-identical"; README "thread determinism" test | `tests/CMakeLists.txt:21-30` + `scripts/compare_parquet.py` compare **table equality** (`pyarrow.Table.equals`), not bytes; only 1 vs 8 threads on one 2.5 MB file; no test covers calibration, mzPeak, `.raw`, `-ms1_axis cycles`, `-aggregation sum` with `-threads`. F1 shows why a second file matters. |

Everything else checked in the two documents matches the code: window assignment (nearest/all), +-60 s, max/sum,
isotope spacing `k x 1.0033548/z` (`Extractor.cpp:14,290`), `-ms1_isotopes 2`, 32 shards, axes lanes -1/-2/window,
footer keys (`odia.input/library/rt_map/mz_tol_ppm/mz_tol_ms1_ppm/mz_offset_ppm/mz_offset_ms1_ppm/aggregation/rt_window_half_s/ms1_mapping`),
log8 semantics, stage-1/2 criteria (5/0.90/0.85, 4/0.80/0.75), +-4 cycles, 5 candidates, +-50 ppm, span 0.15,
3 rejection rounds at max(3 sigma, 3 s), stage-2 +-max(4 sigma, 30 s), median / 1.4826 x MAD / 6 x, 512 MiB trace
pool, 65,536-row parts, 256-precursor units, option defaults in the §6 table, build requirements.

### F8. `-ms1_isotopes >= 255` overflows the `uint8` isotope count and writes past the arena allocation -- LOW, user-triggered

`src/Extractor.cpp:249`: `r.isotopes = uint8(n_iso)` with `n_iso = ms1_isotopes + 1`; `-ms1_isotopes 255` gives
`r.isotopes = 0`, the arena gets `fragments x T` floats (line 265), but the loop at lines 288-292 still pushes
256 MS1 targets whose rows write `v1 + row*M + ...` beyond the allocation (heap/pool overflow; the pool block
belongs to another unit). `setMinInt_` exists, `setMaxInt_` does not (`DIATraceExtractor.cpp:199-200`).

### F9. `PeakPool` capacity estimate -- LOW, theoretical for centroided DIA

`src/RunGrid.cpp:650-656`: `file_bytes / 2` peaks for `.mzML` (x4 otherwise, minimum 2^26). Profile-mode or
numpress-linear + zlib mzML can store a point in < 2 base64 bytes (numpress linear m/z ~0.5-1 byte, slof
intensity 2 bytes, then zlib), which exhausts the pool -> `claim` throws a clear error (`RunGrid.cpp:583-588`), no
corruption. Since the reservation is `MAP_NORESERVE` virtual memory, a flat x4 for all inputs costs nothing. Also
`.mzML.gz` / `.mzML.bz2` take the x4 branch by extension and OpenMS inflates them -- fine, only by luck of the ratio.

### F10. Exceptions inside OpenMP regions terminate the process -- LOW, rare

`src/Calibration.cpp:315-369` (`examineWholeRun`): `TileBuffer::get` throws `std::bad_alloc` (line 307) and the
`std::vector` allocations can throw inside `#pragma omp parallel for` with no try/catch -> `std::terminate`. Same in
`RunGrid.cpp:757-760` (bucket index) and `Library.cpp:563-565` (`concat`). `TileBuffer` has a destructor but
implicit copy operations (double `munmap` if ever copied; `std::vector<TileBuffer> buffers(threads)` does not copy
today). `Pipeline.h:58-59`: `parked.emplace`/`std::optional` outside the try.

### F11. Stage-2 convergence test looks at widths only -- LOW

`src/Calibration.cpp:574-575`: the loop stops when both *widths* change < 25 %; a shift of the *offset* by more than
a sigma between passes does not trigger another pass, so the final pass may have searched a window centred on a
stale offset (the errors stay relative to the uncorrected target, so the model itself is not biased; only the
window may be lopsided). Add `|next.offset - search.offset| < 0.25 x width` to the test.

### F12. `TraceWriter` deletes foreign files -- LOW, real if a user points `-out` at an existing directory

`src/TraceWriter.cpp:119-124`: every `part-*` entry in the output directory is removed, whatever wrote it; a regular
file at `-out` is removed to make room for the directory. Document or restrict to `part-NNN.parquet`.

### F13. `Extractor::plan` with a NaN library RT -- LOW, theoretical

`src/Extractor.cpp:68-72`: `a.rt_center != b.rt_center` with NaN is an inconsistent strict weak ordering ->
`std::stable_sort` UB. A TSV library with `RT = nan` parses (`std::from_chars` accepts "nan", `Library.cpp:425`);
Parquet nulls become 0.0 and are safe. `cycleRange_` then yields the whole run for that precursor.

### F14. `RecyclingPool` statistics and lifetime corner cases -- LOW, profile-only / theoretical

- `src/ArrowPool.cpp:92-98`: `total_`/`count_`/`reuses_` are flushed only when the *net* delta crosses +-1 MiB, so
  `num_allocations()`/`total_bytes_allocated()` can lag without bound (a thread doing balanced alloc/free of small
  Arrow buffers never flushes). `bytes_allocated()`/`max_memory()` are within threads x 1 MiB as documented
  (`ArrowPool.h:21-23`). Only the `ODIA_PROFILE` print at `DIATraceExtractor.cpp:495-500` reads them.
- `ArrowPool.cpp:70-76`: `l.dying` is read after `~Local` ran (formally UB, works on every toolchain in use); libgomp
  worker threads never run TLS destructors at `exit`, so their cached blocks are leaked at exit (harmless).
- `Reallocate` within a class (line 190-193) is correct because `classSize(c) >= n`; `ReleaseUnused` leaves thread
  caches alone, as intended; `classOf`/`classSize` round-trip verified for 4096, 4097, 5120, 5121, 8192.

### F15. Minor / cosmetic

- `src/DIATraceExtractor.cpp:429`: `wcfg.axes = out.empty() ? out_axes : out_axes` (dead ternary).
- `src/DIATraceExtractor.cpp:294-308`: the panel reader hard-codes `top_fragments = 6`, `targets_only = true`; a user
  `-calibration:*` cannot change the panel's fragment count (see F7).
- `src/Extractor.cpp:31`: with `-ms1_axis cycles` and a run without MS1 scans, `ms1Range_` returns `{lo, hi}`, so
  `isotopes x cycles` zero floats are allocated, joined over nothing and written as empty MS1 rows (`TraceWriter`);
  with `-ms1_axis scans` the same run writes MS1 rows of length 0. Inconsistent but harmless.
- `src/TraceWriter.cpp:253`: `ann.ReserveData(rows * 8)` under-estimates "y12^2-H2O" (9 bytes); Arrow grows the
  buffer, output unaffected (the comment at 234-235 says appends never reallocate -- not quite).
- `src/Scheduler.cpp:624`: `parts_ahead` is in *sequence numbers*, so the read-ahead bound is per interleaved
  sequence, not per source -- fine, but the header comment says "library parts loaded ahead".
- `-mz_tol -5` is accepted and means "measured" (`<= 0`, `DIATraceExtractor.cpp:396`).
- `README.md:41-43` example `-rt_mode anchors ... -mz_offset -2.5`: works only because `-rt_mode` is not `auto` (F2).

---

## Items checked and found fine (one line each)

1. **`join<Sum>` vs naive search (`Extractor.cpp:154-211`)** -- fine. Reasoning: targets within a record are sorted by
   `(lo,row)`, consistent with `by_lo = (lo,rec,row)` used by `std::merge`; the forward pointer `p` is the first
   peak `>= t.lo`, monotone because `lo` is non-decreasing over `act`; `seek()` returns a position at or before
   that peak (bucket of `lo`, or `cellEnd` when `lo` exceeds every peak of the arena -- `buckets-1` is the maximum
   bucket of any peak); records with `cycles == 0` get span `{0,0}` and are never merged (`end > c` fails), and the
   `r_out`/`closed` erase uses the span predicate, so non-monotone empty spans do not matter; equal `lo` gives the
   same `p`; empty cells skip; duplicate m/z handled by the `<= hi` scan. Verified empirically: 9,000 random grids
   (1-40 cycles, 0-30 peaks/cell incl. duplicate-heavy and empty cells, 0-60 MS1 scans, 1-12 precursors, RT inside
   and outside the run, +-0..3000 ppm, +-100 ppm offsets, sum and max, both MS1 axes, 0-3 isotopes, unit and charge
   spacing, 1-12 precursors/unit, with and without bucket index of 1-64 buckets, pool and heap arenas) -> 1,547,306
   values, 0 bit-level mismatches, ASan/UBSan clean.
2. **Mass search sign / double application (`Calibration.cpp:123-133, 226-228`; `Extractor.cpp:271,290`)** -- fine:
   the window centre is `mz x (1 + offset)`, errors are `(obs - mz)/mz` against the uncorrected `mz`, the next pass
   centres on the median of those; the extraction applies the same `1 + offset` once. Window can widen back up to
   `search_ppm` (`narrowed`, line 558-560); the loop is bounded by `search_rounds`.
3. **`examineWholeRun` vs `examine`** -- same maxima (`>` tie rule), same fragment-order sums, results re-sorted into
   panel order; `omp_get_thread_num() < threads` holds for the `num_threads(threads)` team.
4. **`robustFit` with < 10 anchors** throws a `runtime_error` that the tool handles (`Calibration.cpp:458-460`).
5. **`Router` cycle closing and lane padding (`RunGrid.cpp:104-141, 189-223`)** -- correct, including partial first
   cycle, windows absent from a cycle, and the duplicate guard.
6. **`loadChunked` boundaries (`RunGrid.cpp:350-556`)** -- chunk `c` runs from `starts[first[c]]` to the next start or
   `</spectrumList`, tags straddling 1 MiB blocks are found by the 16-byte lookahead, comments/DOCTYPE inside the
   run, chromatograms, count mismatch and < 64 spectra all fall back; the per-chunk `claim(total)` is validated per
   spectrum by `decodeSpectrumArrays` (length check, zlib length check) and a failed chunk re-parses via OpenMS.
7. **`MzMLFast`** -- base64 (whitespace, padding), 32/64-bit, zlib, `defaultArrayLength 0`, integer arrays and
   numpress rejected, missing time unit rejected, attribute order independent (offsets are combined after the walk),
   missing isolation window -> selected ion -> else target 0.
8. **`loadMzPeak`** -- one `Spectra` per thread, metadata on the main thread before decoding, `meta[i]` written only
   by the partition owner, empty spectra and `ms_level 0` skipped without claiming, worker exceptions captured and
   rethrown after join. (The reader's "seconds" RT and the index range of the sink are taken on trust; no mzPeak
   test exists.)
9. **Scheduler / Pipeline** -- no deadlock found: the `unwritten` cap is released by writes that depend only on
   encodes, which depend only on commits; the next-commit unit always runs (heap if needed); `orderedParallel`
   waits only on lower indices held by running threads; errors wake all waiters and are rethrown after join.
   `on_part`/`on_unit` run under `committing_`, so `Extractor::account` is single-threaded.
10. **`TraceWriter::encode`** -- Reserve/ReserveData are upper bounds except annotation (F15), under-estimates only
    cost a reallocation; `skip_empty`, log8 (0 reserved, 1..255 over `decades`), `expand_rt`, parallel `close()`
    (independent shards, shared thread-safe pool) are correct; rows are counted once per unit.
11. **`DIATraceExtractor` option interplay** -- `-mz_tol 0` with calibration off errors out (398-406); calibration
    throwing under `-rt_mode auto` errors out; the empty `cal` is never dereferenced (all uses are under `if (cal)`
    or after the `need` check); the stats JSON is well-formed for the values that can occur.
12. **Integer widths** -- `uint32` fragment offsets per 65,536-row part, `uint64` statistics, `uint32 cycle_begin`,
    7 M x 6 fragments / 63 M traces / 7.7 G points all fit; `uint16 fragments` and `uint32 fragment_begin` of a
    whole TSV library overflow only above 65,535 fragments per precursor / 4.29 G transitions (theoretical);
    `toFixed` wraps above 42,949 Th (theoretical).
13. **`BufferPool` / `TraceArena`** -- `blocksFor` mirrors `alloc` exactly (same skip rule, same packing).
14. **Build** -- `CMakeLists.txt` and `tests/CMakeLists.txt` are consistent with the sources; `-Wall -Wextra` only on
    the library target, the tool target compiles without warnings flags.

---

## Suggested order of work

1. F1 (one rule for the window m/z in both parsers; a second determinism fixture).
2. F2 (one-line fix), F4 (size the panel from the file row count for all formats, or read the panel without filters
   and compute the fraction there).
3. F3 and F6 (sample-size floors in the RT and mass models, with warnings).
4. F7 (docs: `target_mz` is uncorrected; qualify "never loaded whole", "5 % of targets", "20 bins", "every threshold";
   README snippet should filter `ms_level == 2`).
5. F5, F8, F10, F12 (robustness), F11, F13-F15 as convenient.
