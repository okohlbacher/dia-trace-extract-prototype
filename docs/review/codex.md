Reviewed `d0109d7`; no files changed. Findings ranked by severity. Native tests were not run; an in-memory Python model of the join passed 10,000 randomized cases, and the floating-point counterexample below was verified.

1. **`src/Scheduler.h:128` — high; bites on real worker failures.**  
   Recycling members are destroyed before `loaded_`, `encode_queue_`, and `sink_queue_`. Their surviving `Part` objects have a deleter that locks `recycle_mtx_` and appends to `free_parts_` (`src/Scheduler.cpp:241`), both already destroyed. Concrete state: a multi-part extraction encounters a Parquet write error while another loaded part remains unissued. `run()` rethrows, then scheduler destruction accesses destroyed objects instead of propagating the error cleanly.

2. **`src/Extractor.cpp:249` — high; adversarial option input, unlikely in normal use.**  
   `-ms1_isotopes 255` produces `n_iso == 256`, but `r.isotopes` truncates to zero. Allocation reserves no MS1 values; target construction still creates 256 isotope rows, and the join writes beyond the allocation. Similarly, `-top_fragments 0` with 65,536 fragments truncates `r.fragments` while constructing every fragment target. Neither limit is validated.

3. **`src/Calibration.cpp:431` — high; invalid option input, not ordinary data.**  
   `-calibration:mode_bandwidth -1` is accepted without a lower bound. Then `y[i] - y[k] > 2 * bandwidth` remains true even at `k == i`; `k` advances beyond the vector. A run producing at least 50 stage-1 anchors reaches this out-of-bounds access.

4. **`src/DIATraceExtractor.cpp:292` — medium; bites on real noncompact libraries with `-ids`.**  
   Panel size uses `library.rows()`, which is already filtered for TSV, TraML, PQP, and noncompact Parquet (`src/Library.cpp:729`). Concrete input: a 100,000-precursor TSV restricted to 100 IDs. The default panel becomes five precursors, sampled from the unrestricted calibration library, so stage 1 cannot reach its required 50 anchors. Without `-ids`, the panel is 5,000. Thus `-ids` changes—and can break—calibration despite the stated invariant.

5. **`src/Calibration.cpp:543` — medium; bites on real relaxed anchors.**  
   The strict mass-anchor predicate checks correlation and cosine but omits the strict fragment-count requirement. Concrete state: 200 used stage-2 anchors have exactly four observed fragments, correlation 0.95, and cosine 0.90. They activate `strict_only` and supply the mass model although none passes the five-fragment strict criterion. Float storage can also round an accepted correlation just above 0.90 below the double threshold during this recheck.

6. **`src/MzMLFast.cpp:224` — medium; valid adversarial data, uncommon in ordinary centroided runs.**  
   Serial ingest sorts original double m/z before quantization; fast ingest sorts only quantized m/z. Concrete unsorted spectrum:

   | Original m/z | Intensity |
   |---|---:|
   | 500.000004 | 100,000,000 |
   | 500.000001 | 4 |
   | 500.000002 | 4 |

   All quantize to the same fixed-point m/z. With `-aggregation sum`, serial order sums to **100,000,008**; fast order sums to **100,000,000**. Put this spectrum in an otherwise fast-parser-compatible mzML containing at least 64 spectra. Changing from one ingest thread to multiple threads changes output values, violating thread determinism.

7. **`src/RunGrid.cpp:655` — medium; possible on real highly compressed data.**  
   Compressed file size cannot bound decoded peak count. Concrete input: a zlib-compressed `.mzML` smaller than 128 MiB containing 70 million peaks. Capacity is only 67,108,864 peaks, so ingest fails with pool exhaustion. Repetitive arrays can readily construct this state; the comment’s minimum base64 characters per compressed peak is false. The non-mzML multiplier has the same fundamental limitation.

8. **`src/MzMLFast.cpp:133` — medium; bites on multi-precursor/multi-scan metadata.**  
   Metadata is collected without XML scope and overwritten by later matching CV terms. Concrete spectrum: two precursors with isolation targets 400 and 600 Th. Fast ingest routes it to 600; OpenMS fallback uses the first precursor and routes it to 400 (`src/RunGrid.cpp:81`). Multiple scan-start-time terms similarly select the last time. Parser choice therefore changes the grid on such data.

9. **`src/RtMap.cpp:65` — medium; bites when reusing exported calibration anchors.**  
   `fromAnchors()` ignores the exported `used` column and fits every numeric row. Concrete input: an `out_calibration` TSV containing 50 used anchors on `run_rt = library_rt` and 50 rejected anchors hundreds of seconds away. Loading it through `-rt_anchors` reinstates the rejected observations and changes the map. The exported file is syntactically usable, but does not preserve the calibrated fit.

10. **`src/Calibration.cpp:574` — medium; possible with changing centroid populations.**  
    Convergence compares widths only, ignoring offsets. Concrete round state: the search has offset 0 and width 18 ppm; the new model has offset +12 ppm and accuracy 6 ppm for both levels. Both next widths remain 18, so the loop stops without searching around +12. Extraction nevertheless uses +12. This is premature convergence when narrowing changes which centroid population wins.

11. **`src/Extractor.cpp:255` — medium; bites on MS1-only portions of the acquisition.**  
    An empty MS2 cycle range suppresses independent MS1 extraction. Concrete run: MS1 scan at 0 s, first cycle at 10 s; precursor center 0 s, half-window 1 s. The MS1 scan is inside the requested window, but the record returns early with zero MS1 points. `ms1Range_()` independently enforces the same unwanted MS2 dependency at line 33.

12. **`src/TraceWriter.cpp:388` — medium; bites on real storage failures.**  
    TSV `fwrite()` and `fclose()` results are ignored. A TSV-only extraction to a full filesystem can report successful rows and return success with truncated output. Statistics output likewise does not check opening or writing (`src/DIATraceExtractor.cpp:507`).

13. **`src/Calibration.cpp:315` — medium; bites under allocation failure.**  
    `examineWholeRun()` lets exceptions escape an OpenMP worker. Concrete state: `TileBuffer::get()` cannot map the requested tile, throws `bad_alloc`, and escapes the parallel region. The outer TOPP exception handling does not provide graceful propagation for this path.

14. **`tests/CMakeLists.txt:30` — medium; real verification gap.**  
    The determinism test compares Arrow table values, not bytes. Two datasets with identical tables but different footer metadata, compression, or row-group layout pass. Consequently the current test does not enforce the byte-identical invariant.

15. **`tests/CMakeLists.txt:10` — medium; real parity-test configuration defect.**  
    The purported OpenSWATH-equivalent extraction leaves `aggregation=max`, `window_assignment=nearest`, and mass calibration enabled. Concrete cell with two in-window intensities 3 and 4 gives extractor value 4 versus OpenSWATH’s sum 7. Overlapping windows or successful mass calibration introduce further differences.

16. **`src/DIATraceExtractor.cpp:102` — low; bites on valid unusual filenames.**  
    JSON escaping handles quotes and backslashes but not control characters. An input or library filename containing a tab or newline produces invalid statistics JSON.

Additional documentation discrepancies, all **low severity** unless covered by a higher-ranked finding above:

| Location | Concrete unsupported statement/state | Real or theoretical |
|---|---|---|
| `docs/EXTRACTION.md:10` | Default `rt_mode=linear` leaves RT unchanged; the tool does not measure all required RT information by default. | Real default behavior |
| `docs/EXTRACTION.md:19` | “Every trace … shares one time axis” contradicts default MS1 scan-axis output. | Real |
| `docs/EXTRACTION.md:65` | `target_mz` is described as corrected. Writer lines 293 and 321 emit the uncorrected library/isotope target; a +10 ppm correction to 500 Th still exports 500, not 500.005. | Real |
| `docs/EXTRACTION.md:86` | Panel fraction is applied to total rows, not eligible target precursors. A compact library with 50% decoys and eligible sampled targets selects roughly 10% of eligible targets. Row pre-sampling also prevents a guaranteed globally RT-stratified panel. | Real |
| `docs/EXTRACTION.md:92` | Candidates are not simply the five largest local maxima: maxima within eight cycles of a previously selected candidate are excluded by default. | Real |
| `docs/EXTRACTION.md:99` | Twenty mode bins are not guaranteed: 50 anchors use five bins. Filtering is skipped when fewer than 20 points survive. Rejection can subsequently re-enable mode-rejected anchors (`Calibration.cpp:483`). | Real |
| `docs/EXTRACTION.md:111` | “Pass the strict criteria” omits the implementation’s missing fragment-count check; see finding 5. | Real |
| `docs/EXTRACTION.md:116` | Tolerance is `max(6 × width, 0.1 ppm)`, not always `6 × width`. Equal mass errors produce zero width but 0.1 ppm tolerance. The same omission appears at `README.md:53`. | Real degenerate data |
| `docs/EXTRACTION.md:118` | Subsequent search width is capped at the initial search width; a 20 ppm tolerance requests 60 ppm but searches only 50 ppm. | Real |
| `docs/EXTRACTION.md:128` | Not every stated threshold has a calibration option: six calibration fragments are hardcoded, and the 50/200-anchor floors are fixed. | Real |
| `docs/EXTRACTION.md:138` | Parallel chunk parsing is conditional: one ingest thread, fewer than 64 spectra, compression, or unsupported layout uses streaming ingest. | Real |
| `docs/EXTRACTION.md:142` | “Library is never loaded whole” is false for TSV, TraML, PQP, and noncompact Parquet (`Library.cpp:729`). | Real |
| `docs/EXTRACTION.md:145` | A lane’s cells are not necessarily contiguous in the peak pool; concurrent claims interleave lanes/chunks. Overlapping target windows also rescan peaks during aggregation, despite “walked once.” | Real |
| `README.md:42` | The fixed-offset example leaves mass calibration enabled, which overrides both supplied offsets when calibration succeeds. | Real |
| `README.md:57` | Output is not wholly on the global cycle axis. Its Python example at line 65 can select an MS1 row and apply scan indices to cycle times. | Real |

The documented defaults, isotope spacing, output schema/footer keys, and basic window assignment otherwise match the CLI.

The join’s equal-`lo` ordering, duplicate peaks, empty cells, above-all-peaks targets, zero spans, and scan-axis spans look sound; the randomized model agreed with naive lookup. Mass-offset signs and uncorrected error recording are correct; narrowing can widen again up to its initial cap. Chunk-boundary lookahead, ordinary 32/64-bit zlib decoding, minute/second handling, and mzPeak reader ownership/worker exception collection look sound. Scheduler issue ordering and error wakeups look sound apart from destruction. ArrowPool’s singleton lifetime, same-class reallocation, statistics bound, and cached-block safety look sound. Writer reserve underestimates grow safely; ordinary log8, `skip_empty`, `expand_rt`, and parallel shard closing look sound. Empty-calibration fallback and zero-tolerance/off checks avoid an empty `cal` dereference.
