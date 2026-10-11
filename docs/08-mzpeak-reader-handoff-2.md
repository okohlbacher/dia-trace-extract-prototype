# Handoff 2: mzPeak reader for DIATraceExtractor — measurements and open requests (2026-10-11)

For the agent working on okohlbacher/OpenMS-mzPeakRW. Follows `docs/04-mzpeak-reader-handoff.md` (requirements R1–R2);
this note adds what we measured with the reader as it is and what now blocks us.

## 1. State on our side

- We use **only** the OpenMS-mzPeakRW reader (`MzPeakFile`, vendored from 0525e34 and ported to OpenMS develop /
  `std::string`, in `src/mzpeak/`). Upstream `main` = tag `mzpeakrw-2026-10-10` = 4ad7164; nothing newer to pick up.
- Our ingest now writes peaks straight into one global, memory-mapped peak pool (m/z as uint32 fixed point 1e-5 Th,
  intensity float). Every input path claims a contiguous range per spectrum (one atomic add) and decodes into it.
- mzML for comparison (same run, 64 threads, warm cache): **ingest 0.27–0.33 s**, total run 11.9 s.

## 2. Measurements with the current reader

File: fork-written point layout, `results/mzpeak/run01_openms.mzpeak` (1.08 GB, 135 row groups, 141 M peaks),
64 threads, warm page cache, node `data` (2 × 56 cores).

| | ingest wall | notes |
|---|---|---|
| `transform()` + our consumer (now direct into the pool) | **8.0–8.3 s** | consumer callback 1.3 s; reader ≈ 6.7 s, single-threaded (108% CPU) |
| reader alone on a producer thread, conversion on 16 other threads | 8.3–9.2 s | reader thread alone 7.1–7.7 s → no gain; dropped |
| mzML fast path | 0.27–0.33 s | |

- Output via mzPeak is byte-identical between our variants (hash 6c405adf555fb43f).
- `getSpectrum(i)` reads every row group whose `spectrum_index` range contains `i`: one full row-group decode per
  spectrum. Unusable for bulk ingest, and not thread-safe (shared `FileReader`, lazy index).
- So the floor is the serial `transform()`: the row-group decode, `addSpectraFromTable_` building `MSSpectrum`/`Peak1D`
  objects, and the `NativeID "index=N"` round trip in the merge.

## 3. Defects (blocking)

1. **mzpeak-convert 0.17.2 files read 0 spectra** (e.g.
   `benchmark-data/DIA-astral-entrapment/mzpeak-0.17.2/LFQ_Astral_DIA_15min_50ng_Human_01.mzpeak`). What differs from the
   fork layout:
   - `data_kind` is `"data_arrays"` (underscore), not `"data arrays"`. Both `openFile()` and `transform()` match only the
     spaced form.
   - Metadata is flat (`index`, `ms_level`, `time`, `spectrum_representation`, ...) instead of a `spectrum` struct.
     `time` is in **minutes** (`column_mapping` unit `UO:0000031`).
   - Precursors live in their own table (`data_kind "precursors"`: `source_index`, `precursor_index`, `isolation_window`
     {`isolation_window_target`, `_lower_offset`, `_upper_offset`}).
   - Signal is the **chunked** layout (`chunk` struct: `spectrum_index`, `mz_chunk_start`, `mz_chunk_end`,
     `mz_chunk_values`, `chunk_encoding`, `intensity` list, `mz_numpress_linear_bytes`). Every chunk in this file is
     `MS:1002312`. Decoded numpress values of 0 are null markers. Delta (`MS:1003089`) and plain chunks hold the start
     value in `mz_chunk_start`, **excluded** from `mz_chunk_values`; see `chunk_series.rs` in mzpeak_prototyping
     (vendored in mzPeakConverter) for `decode_arrow` and `null_delta_decode`.
2. **Empty spectra are dropped.** The point file has 292,883 MS2 spectra; `transform()` emits 292,860. The 23 spectra
   without peak rows exist in the metadata but never reach the consumer, because the streaming merge only emits spectra
   that have rows. We need them: our cycle detection counts them. Emit metadata-only spectra in index order between
   the signal-carrying ones.

## 4. What we need (priority order)

1. **Fix §3.1 and §3.2.** Acceptance: run 01 from 0.17.2 gives 2,928 MS1 + 292,883 MS2 spectra. MS2 m/z must be within
   0.5 ppm of the mzML, and intensities equal as float. Isolation windows equal the mzML's to float precision.
2. **The partitioned, thread-safe read API from handoff 1, R2** (same shape; names up to you):
   - `open()` reads the ZIP directory, index and metadata once. After that, all `const` methods are thread-safe.
   - `partitions()`: facet plus row-group range, covering only complete spectra.
   - `decode(partition, sink)`: each call uses its own `parquet::arrow::FileReader` over the same stored entry.
     Stored entries are read in place, memory-mapped, without `TempDir` extraction. No global locks.
   - The sink receives `(spectrum_index, span<const double> mz, span<const float> intensity)`. No `MSSpectrum`, no
     `Peak1D`, no NativeID string parsing.
   - A metadata accessor per spectrum: index, ms_level, time in seconds, isolation target/lower/upper,
     representation, n_points. Spectra without signal rows must be included.

   We will write straight from the sink into our pool. Target: **ingest ≤ 0.5 s at 32–64 threads** for run 01. The
   pyarrow estimate for the Parquet decode alone is 0.31 s (peaks) + 0.12 s (data) at 32 threads.
3. Optional: a **centroided MS1** facet, or read-side centroiding, so that MS1 from 0.17.2 files is comparable to the
   centroided mzML MS1. This is converter-side; see `docs/05-mzpeak-converter-handoff.md`.

## 5. How we will test

- `DIATraceExtractor -in <file.mzpeak> ... -threads 64` with `ODIA_PROFILE=1`, which prints the ingest breakdown
  (`ingest.mzpeak_callback` etc.).
- Equivalence: trace hash of the fork point-layout file against the current build (6c405adf555fb43f). For 0.17.2,
  MS2-only comparison against the mzML run within the numpress tolerance.
- Code: `src/RunGrid.cpp` (mzPeak branch of `RunGrid::load`); repo okohlbacher/dia-trace-extract-prototype.

## 6. Outcome (2026-10-11, tag `partitioned-read-2026-10-11`)

- Integrated as a git submodule (`external/mzpeak`), built against OpenMS's Arrow 24; the vendored OpenMS-mzPeakRW
  reader is gone. The extractor opens the archive once, reads the metadata once, and decodes `partitions(4 x threads)`
  with one `Spectra` per thread straight into its peak pool (`src/RunGrid.cpp`, `loadMzPeak`).
- Both §3 defects are fixed: the 0.17.2 and 0.18.0 files read 2,928 MS1 + 292,883 MS2 spectra including the 23 empty
  ones; the cycle/window grid is identical to the mzML path's (217 empty cells).
- `run01_openms.mzpeak` regenerated with `MzMLToMzPeak` on the library's `RunArchiveWriter` (seconds in, minutes
  stored): traces byte-identical to the mzML path (1,205,873 rows, ID library; axes equal to 1e-12 s).
- Timing, run 01, warm cache, node `data` (2 x 56 cores):

  | file | ingest wall | open | metadata | decode | CPU |
  |---|---|---|---|---|---|
  | point layout (our writer), 64 threads | 0.85 s | 0.17 s | 0.20 s | 0.47 s | 12.1 s |
  | point layout, 32 / 16 / 8 threads | 0.86 / 1.02 / 1.42 s | | | 0.48 / 0.66 / 1.06 s | |
  | 0.18.0 (chunked numpress, vendor-centroided MS1), 64 threads | 3.6 s | 1.1 s | 0.3 s | 2.7 s | 40 s |
  | mzML (our chunked parser), 64 threads | 0.32 s | | | | 8.2 s |

  The decode stops scaling at 32 threads (12-14 cores busy): the row-group cache serialises decoding per archive.
  To reach the <= 0.5 s target: a per-partition (per-`Spectra`) decode path without the shared lock, and a faster
  chunked/numpress decode (0.18.0 is 4x slower than the point layout for 1.4x the points).
- Answer to §4 of your note: the Astral benchmark data are the ProteoBench DIA entrapment module
  (https://proteobench.readthedocs.io/en/stable/modules/dia/entrapment-dia-astral/), raw files and FASTA at
  https://proteobench.cubimed.rub.de/raws/DIA-astral-entrapment/ (`all_data_Entrapment_DIA_Astral.tar.gz`); no
  PRIDE/ProteomeXchange accession is listed.
