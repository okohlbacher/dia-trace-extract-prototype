# Handoff: parallel mzPeak reading for DIATraceExtractor (2026-10-10)

For the agent working on okohlbacher/OpenMS-mzPeakRW. From the ODIA-V3 trace extractor
(okohlbacher/dia-trace-extract-prototype). Purpose: agree what the reader must provide so that ingesting a
DIA run from mzPeak is parallel and faster than our mzML path.

## 1. Context

- **Consumer:** `DIATraceExtractor`, an OpenMS TOPP tool built against OpenMS **develop** (3.7, `String` removed).
  It vendors `MzPeakFile` from OpenMS-mzPeakRW 0525e34, ported to `std::string` (`src/mzpeak/`). Tag
  `mzpeakrw-2026-10-10` has the same reader (only DIAPseudoSpectra removed); its message says "not yet
  ported to develop".
- **Input files:** produced only by the **latest mzpeak-convert from okohlbacher/mzPeakConverter**, now
  v0.17.2, with default options. Do not target 0.7.0 or files from the fork's own writer.
  Test file: `benchmark-data/DIA-astral-entrapment/mzpeak-0.17.2/LFQ_Astral_DIA_15min_50ng_Human_01.mzpeak`
  (Thermo Astral, 15 min DIA; runs 02 and 03 next to it).
- **Current state:** the vendored reader reads **0 spectra** from that file, so the tool stops with
  "no MS2 isolation windows".
- **What we do with the data:** route each spectrum by level and isolation window into a grid with one
  global RT axis. Per peak we keep m/z as uint32 fixed point (1e-5 Th) and the intensity as float.
  - We do not need `MSSpectrum` objects, CV parameters, the vendor facets or chromatograms.
  - Today's mzML path: chunks of the XML are parsed in parallel and handed over in file order through
    an ordered pipeline. Run 01 ingests in **2.8 s at 32 threads** (warm cache).
  - mzPeak should beat that and remove the 5.8 GB mzML read from Ceph (1.29 GB mzPeak).

## 2. What a 0.17.2 file contains (run 01, measured)

| ZIP entry (all stored, uncompressed) | content | rows | row groups |
|---|---|---|---|
| `spectra_metadata.parquet` (12.8 MB) | one row per spectrum: index, id, ms_level, time, representation, point/peak counts | 295,811 | 1 |
| `spectra_metadata_precursors.parquet` | isolation window (target, lower/upper offset) per MS2, keyed by `source_index` | 292,883 | 1 |
| `spectra_metadata_selected_ions.parquet`, `_scans.parquet` | selected ion, scan start time, filter string | 292,883 / 295,811 | 1 |
| `spectra_data.parquet` (255 MB) | **MS1 profile**: 2,928 spectra, 65.7 M points | 35,131 chunk rows | 16 |
| `spectra_peaks.parquet` (1.0 GB) | **MS2 centroids**: 292,883 spectra, 133.5 M peaks | 5.7 M chunk rows | 28 |
| `chromatograms_*`, `vendor_*` | not needed by us | | |

- **Index file:** `mzpeak_index.json` declares `data_kind` `"data_arrays"` / `"peaks"` / `"metadata"` with
  a column mapping.
- **Signal layout (both signal tables):** chunked. Each row is a `chunk` struct with:
  - `spectrum_index`;
  - the m/z range of the chunk (`mz_chunk_start`, `mz_chunk_end`), 50 Th wide;
  - `chunk_encoding`: in this file every chunk is `MS:1002312`, numpress-linear;
  - the m/z values in `mz_numpress_linear_bytes`, or in `mz_chunk_values` for delta chunks;
  - `intensity` as a list of float.
- **Other encodings the converter can write:** delta chunks, `zero-run-mask` for profile data (all-zero
  stretches are dropped), and an m/z lattice grid (`grid-fit`).
- **Row-group boundaries:** in this file, **no spectrum spans two row groups** (spectrum_index ranges
  are disjoint, sorted, and stored as row-group statistics). The reader must still handle a spectrum
  that spans a boundary when the writer does not guarantee it.
- **Empty spectra:** 23 MS2 spectra have 0 peaks. They exist in the metadata but have no rows in the
  peaks table.

**Parallel decode (pyarrow, warm cache):** reading row groups in parallel, each thread with its own
reader over one memory-mapped buffer, gives these times:

| table | 1 thread | 8 threads | 32 threads |
|---|---|---|---|
| `spectra_peaks` | 3.2 s | 0.70 s | 0.31 s |
| `spectra_data` | 0.8 s | 0.20 s | 0.12 s |

These cover only the Parquet decode, not numpress decoding or building spectra. For comparison, the
fork's point-layout file (135 row groups) decoded in 3.6 s on 1 thread and 0.16 s on 32. The vendored
reader loaded that file in 8.7 s serially.

## 3. Requirements

### R1 Read 0.17.2 files correctly
- **Layout and tables:**
  - `data_kind` `"data_arrays"`;
  - the split metadata, precursors and selected_ions tables, joined on `source_index`;
  - the chunked layout in both `spectra_data` and `spectra_peaks`.
- **Encodings:**
  - numpress-linear;
  - delta;
  - `zero-run-mask` (it is fine to return only the stored points);
  - the `grid-fit` lattice, as long as the converter can still write it.
- **Empty spectra:** keep them. Return the 23 MS2 spectra that have 0 peaks with their metadata and
  isolation window. Dropping them shifts our cycle detection.
- **Metadata we need for every spectrum:**
  - index and native id;
  - ms_level;
  - time in seconds (use the unit in the column mapping);
  - for MS2: the isolation window target and lower/upper offsets;
  - representation (profile or centroid).

### R2 A partitioned, thread-safe read API
We need independent work units that our threads can decode concurrently. Proposed shape (names are up to
you; we will adapt):

```cpp
class MzPeakReader            // or extend MzPeakFile; const methods thread-safe after open()
{
public:
  void open(const std::string& path);                  // reads the ZIP directory, index JSON and metadata tables
  const SpectrumTable& spectra() const;                // per spectrum: index, id, ms_level, time_s,
                                                       // isolation target/lower/upper, representation, n_points
  std::vector<Partition> partitions() const;           // a partition is a facet (data|peaks) plus a row-group range;
                                                       // it covers complete spectra [first_index, last_index]
  void decode(const Partition& p, SpectrumSink& out) const;   // thread-safe, no shared mutable state
};

struct SpectrumSink   // called per spectrum, in spectrum_index order within the partition
{
  virtual void spectrum(std::uint64_t index, std::span<const double> mz, std::span<const float> intensity) = 0;
};
```

- **Complete spectra per partition:** each partition holds only complete spectra. If a spectrum spans a
  row-group boundary, the partition boundary moves so it falls into one partition (use the
  `spectrum_index` row-group statistics). Do not hand spectrum fragments to the caller.
- **No locks or shared mutable state:**
  - each `decode` uses its own `parquet::arrow::FileReader`, or a thread-safe random-access file over
    the same stored entry;
  - no global locks; no `File::TempDir` extraction per call (stored entries can be read in place at
    their ZIP offset, memory-mapped).
- **Raw arrays, no per-peak objects:** the sink receives m/z (double) and intensity (float) spans that
  are valid for the duration of the call. No `MSSpectrum` or `Peak1D` allocation per peak.
- **Partition size:** at least one row group per partition. If a file has too few row groups for the
  thread count (e.g. 16 for MS1), allow splitting a row group by row ranges at spectrum boundaries.
- **Order:** we impose order ourselves, by partition and spectrum index. The reader does not need its own
  thread pool.
- **Keep the existing API:** the serial `transform()` / `load()` should keep working; they can be built on
  top of this.

### R3 Merged order across the two facets
MS1 sits in `spectra_data` and MS2 in `spectra_peaks`. We need the global spectrum order: the
`spectrum_index` from the metadata table is enough. Options:
- `partitions()` covers both facets and we interleave them by index ourselves; or
- the reader returns partitions sorted by `first_index` across facets.

The metadata (`spectra()`) must be available before any signal is decoded, so we can detect the DIA
cycles first.

### R4 MS1 profile data
Our mzML is vendor-centroided at both levels; in the 0.17.2 file MS1 is profile only. We need centroided
MS1 for comparisons with DIA-NN and OpenSWATH. Please state which of these you will do:
- **(a)** the converter writes Thermo vendor centroids for MS1 to `spectra_peaks`. This is preferred, as
  it is identical to the mzML input. This is a request to mzPeakConverter: today `--representation` is
  honoured only for Shimadzu and BAF.
- **(b)** the reader offers an optional centroiding pass on profile spectra inside `decode`, so it runs
  in parallel.
- **(c)** neither: we centroid on our side.

### R5 Numbers we will check
- **Speed:** full signal read of run 01, warm cache, a partition decoder feeding a no-op sink.
  - Target: **≤ 1 s at 32 threads**, and better than 3 s at 8 threads.
  - Report 1/8/32/64 threads.
- **Memory:**
  - peak RSS bounded by roughly (threads × the largest row group) plus the metadata;
  - the whole signal is never resident at once.
- **Determinism:** the decoded arrays are identical for any thread count.
- **Accuracy against the vendor-centroided mzML of the same run** (`benchmark-data/.../mzML/*_01.mzML`):
  - the same number of MS2 spectra (292,883, including the empty ones);
  - the same times and isolation windows;
  - per-peak intensities equal;
  - m/z within the numpress-linear error. Report the maximum ppm error. We need ≤ 0.5 ppm, because we
    quantize to 1e-5 Th.

### R6 Porting
- Target OpenMS develop: `std::string`, and TOPPBase in OpenMS_CLI.
- Ideally `MzPeakFile` builds standalone, as we vendor it today: Arrow, Parquet and nlohmann_json only.
  Then we can re-vendor without hand porting.

## 4. What we do on our side once this exists

- Feed partitions into the same ordered pipeline as the parallel mzML ingest (`RunGrid.cpp`
  `loadChunked`): workers decode a partition and convert it to grid spectra, which are routed in index
  order.
- Then measure cold- and warm-cache ingest against mzML and check that the extracted traces are
  identical (or within the m/z tolerance) on the refined-library benchmark.
