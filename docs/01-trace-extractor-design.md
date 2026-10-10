# DIATraceExtractor — design and status (2026-10-09)

Goal: extract DIA traces (fragment XICs + MS1 isotope XICs) efficiently from a predicted, recalibrated
spectral library and export them so they can be compared point by point with DIA-NN's and OpenSWATH's
traces on the same runs (ProteoBench Astral entrapment, 3 runs). **Extraction only — no scoring, no
classifier, no FDR.** A clean C++23 rebuild on OpenMS develop (3.7) as a TOPP tool (`TOPPBase`), not a
continuation of the ODIA code base; it reuses the ODIA/KG *ideas* listed in §2.

## 1. Architecture

```
library (Parquet/TSV/PQP/TraML) ─► struct-of-arrays Library (CSR fragments, top-N by intensity)
run (mzML streamed | Thermo .raw) ─► RunGrid: ONE global RT axis (DIA cycles) × lanes
                                          ├─ 300 window lanes: CSR cell per global cycle, m/z uint32 (1e-5 Th) + float32 = 8 B/peak
                                          └─ MS1 lane: own scans, interpolated onto the cycle axis
Extractor: units = (window, ≤256 precursors) ──► parallel: gather F×T tile + MS1 isotopes, encode Arrow batch
                                              └► ordered commit (unit order) ─► writer thread ─► Parquet / TSV
```

- **Global RT axis.** A DIA cycle visits each isolation window once; a cycle ends when a window
  repeats. `cycle_rt[c]` = mean acquisition time of the cycle's MS2 spectra. Every window lane is
  indexed by the same cycle `c` (empty cell if a window was not acquired), so all fragment traces of
  all precursors share one time axis, and a trace is just `(cycle_begin, values)`. MS1 (acquired 3×
  per cycle on this Astral method) is extracted at its own scans and linearly interpolated at
  `cycle_rt` (as DIA-NN does). Exact per-spectrum acquisition times are kept in the axes output.
- **Grid ingest.** Each spectrum is decoded once into its lane (8 B/peak). The serial XML parse runs
  on its own thread and overlaps with routing (producer/consumer, bounded queue); OpenMS's internal
  OpenMP decoding in the parser thread is bounded explicitly (`-ingest_threads`, default 1 — at the
  inherited default it burned 29× CPU for no gain).
- **Gather.** Per unit (one lane → cache locality, one owner, no locks): for each cycle in the
  precursor's RT range, the fragments (sorted by m/z) are looked up in the cell's sorted m/z array
  with a resumed binary search; `sum` (OpenSWATH) or `max` (DIA-NN) over centroids within ±ppm.
- **Ordered parallel pipeline** (`Pipeline.h`): units are extracted *and encoded to Arrow* on N
  threads; results are committed strictly in unit order with bounded look-ahead, so the output is
  byte-identical for any thread count (tested). The writer thread only appends batches.
- **Explicit per-stage threads and accounting**: every stage reports wall, CPU and CPU%; peak RSS.

## 2. What was reused from the handoff / KG (and why)

| idea | source | here |
|---|---|---|
| compact grid: per-window, RT-binned, m/z-sorted SoA, decode once, 8 B/peak, fixed-point m/z | KG 60-Architecture/Extraction data flow design v1; 80-Measurement/Memory efficiency work; HANDOFF §2.2 MzFixed | RunGrid |
| one RT axis instead of RT per transition (42.7 of 85 GB in V1 was redundant RT) | KG Memory efficiency work | global cycle axis; traces = (cycle_begin, values) |
| uint8 log-spaced intensity with per-trace scale, 0 = true zero | KG Memory efficiency work; BACKLOG | `-encoding log8` |
| positions, not float RTs, decide membership | memory odia-chunked-extraction-float-rt | cycle indices; double RT comparisons |
| route by window, one owner per cell, no shared locks | KG Streaming and threading measured | units never straddle a lane |
| serial XML parse is the ingest wall; overlap it; never inherit all-core OpenMP | same; data flow design v1 NFRs | producer/consumer; `-ingest_threads` |
| no single-threaded sink | doc/83 §2.3 | encoding moved to the workers |
| determinism across threads/chunks | HANDOFF §1.5, §2.5.3 | ordered commit; ctest gate |
| explicit tolerance semantics (half vs full width; sum vs max) | doc/27; KG DIA pipeline phases compared | ±ppm half widths; `-aggregation` |

Not reused: candidate prefilters, scoring, classifiers, FDR, PyProphet — out of scope by decision.

## 3. Outputs

- **Traces (Parquet)**, one row per trace: `run, precursor_id, modified_sequence, precursor_charge,
  decoy, precursor_mz, window, ms_level, annotation, fragment_type/ordinal/charge/loss, isotope,
  target_mz, library_intensity, rt_center, cycle_begin, intensity` (+ `rt` with `-expand_rt`).
  Point arrays are **binary blobs** (little-endian float32[n]; `intensity_log8` uint8[n] +
  `intensity_scale`), as in OpenSWATH's `.xic` — Parquet nested lists cost repetition/definition
  levels per point on the serial write path (145 s → 46 s on the full library). Decode with
  `numpy.frombuffer(b, '<f4')`. Footer metadata records tolerances, RT map, encodings.
- **Axes (Parquet)**: lane −1 = global cycle axis; lane ≥ 0 = exact acquisition RT and spectrum index
  of every window spectrum (with window bounds); lane −2 = MS1 scan times.
- **TSV** long format (one row per point; for subsets).

## 4. Validation

- OpenSWATH parity (ctest `parity_with_openswath`, OpenMS test data): with matched settings
  (±50 ppm = OpenSWATH 100 ppm full width, whole run, 2 isotopes) all 18 MS2 traces agree with
  OpenSwathWorkflow's `.xic` to its lossy numpress/slof precision, and all 18 MS1 traces agree after
  interpolating OpenSWATH's MS1 trace at our cycle times.
- **OpenSWATH isotope quirk**: `ChromatogramExtractor::prepare_coordinates` places isotope k at
  `mz + k·1.00336` *without dividing by the charge* (ChromatogramExtractor.cpp:161-170), i.e. its
  `Precursor_i1` of a 2+ precursor is the physical M+2. Default here is `/z` (physically correct, as
  DIA-NN); `-ms1_isotope_spacing unit` reproduces OpenSWATH.
- Determinism: byte-identical Parquet at 1 vs 16 threads (Astral, 20K precursors) and 1 vs 8 threads
  with 1-precursor units (ctest).

## 5. Measured (Astral run 01, 64 threads, spock-class 224-core node)

| | value |
|---|---|
| run | 295,811 spectra (2,928 MS1, 292,883 MS2), 141.0 M peaks, 977 cycles × 300 windows |
| grid | 1.08 GiB, ingest 34 s (serial mzML parse) |
| library | 3,488,457 precursors (DIALibGen 0.11.0 human, top-6 fragments), load 14 s |
| traces | 31.3 M traces, 2.29 G points (±60 s), extraction+write 46 s |
| **total** | **96 s wall, 3.19 GiB peak RSS**, 1.19 GB Parquet |

(The RT map in this run is a rough linear guess; the run measures throughput, not trace placement.)
Reference points from the handoff: DIA-NN 2.0 19.5 GB / 5:31 on a comparable Astral run; ODIA2
543 GiB / 16 h on timsTOF.

Next throughput levers if needed: sharded Parquet writers (the writer's single ZSTD stream is now the
bound), parallel mzPeak ingest (MS2 centroids are stored directly; MS1 is profile and needs
centroiding), parallel library load.

## 6. Status of the comparison

Done for all three ProteoBench Astral runs: see [02-benchmark-astral.md](02-benchmark-astral.md)
(RT recalibration from DIA-NN 1.7.12 identifications; DIA-NN traces via its `--xic` export with two
source fixes; OpenSWATH on identical coordinates).

Self-calibration (`-rt_mode auto -mass_calibration auto`, src/Calibration.cpp) is done: two-stage
co-elution anchors, run-RT mode curve, LOWESS with rejection, per-peak mass offsets; it matches a map
fitted on DIA-NN's identifications (02-benchmark-astral.md).

Open: an mzPeak reader (parallel ingest; MS1 there is profile data), sharded Parquet writers, library
refinement from the run anchors (per-peptide RT), the 50 K / top-6 shared library from the
project meeting once posted, DIA-NN 2.x comparison on request.

## 7. Parallel efficiency (2026-10-10)

Before, ~95% of a full-library run was serial: the mzML XML parse (35 s), the single Parquet writer
(caps extraction at ~46 s from 8 threads up) and the library load (14 s); 64 threads ran at 8% efficiency
and nothing improved beyond 8 threads. Four changes:

1. **Parallel mzML ingest** (RunGrid.cpp `loadChunked`, adapted from OpenMS ProSEAlgorithm's
   `loadMzMLChunked`): spectra are cut into ≤4 MiB chunks at their `<spectrum>` tags, each parsed with the
   file header by its own `MzMLFile::loadBuffer` (standard handler: all metadata and encodings), converted
   to grid spectra on the worker, and routed in file order through the ordered pipeline (bounded
   look-ahead). Falls back to the streaming parser for compressed or unusual files. `-ingest_threads`
   (default = `-threads`).
2. **Sharded Parquet output**: unit u → part u % 8, one writer thread per part; `-out` becomes a directory
   of `part-NNN.parquet` (one dataset for pyarrow/DuckDB). Fixed default (`-out_shards 8`), so the output
   is independent of `-threads`.
3. **Library load**: precursor strings in one buffer per column (`StringColumn`) instead of ~7 M
   `std::string`s; compact Parquet libraries parsed in parallel row chunks, joined in order;
   multi-threaded column decode.
4. **Passive OpenMP waits**: the tool sets `OMP_WAIT_POLICY=PASSIVE` (re-exec once) unless set by the user.

Full human library (3.49 M precursors, 31.3 M traces), Astral run 01:

| threads | library | ingest | extract+write | total wall | total CPU | efficiency | peak RSS |
|---|---|---|---|---|---|---|---|
| before, 64 | 13.6 s | 34.8 s | 46.7 s | 96.7 s | 513 s | 8% | 3.2 GiB |
| 1 | 10.5 s | 34.5 s | 158.8 s | 205.2 s | 308 s | — | 3.2 GiB |
| 8 | 5.4 s | 5.7 s | 20.9 s | 33.6 s | 328 s | 122%* | 4.3 GiB |
| 32 | 5.3 s | 2.8 s | 6.1 s | **15.6 s** | 336 s | 67% | 5.5 GiB |
| 64 | 5.2 s | 3.5 s | 6.1 s | 16.2 s | 460 s | 44% | 6.4 GiB |
| 128 | 5.4 s | 4.1 s | 6.5 s | 17.5 s | 800 s | 36% | 7.7 GiB |

(*the 8 writer threads and Parquet's column threads run in addition to the 8 workers.)
Speed-up 1→32 threads 13×; the full-library run is 6.2× faster than before. Outputs are identical:
parallel vs serial ingest (31,348,861 rows), 1 vs 64 threads, and the refined-library benchmark traces
before vs after. Refined library (84 K precursors): 42 s → 4.7 s (64 threads), 5.7 s (32 threads).

Remaining: library load (~5 s; Parquet decode and dictionary casts), ingest CPU grows with threads (35 →
174 → 425 CPU-s at 1/64/128 threads; per-chunk parser set-up and allocator contention), peak RSS rises
with the look-ahead buffers (3.2 → 6.4 GiB at 64 threads). 32 threads is the sweet spot on this node.

### 7.1 Library load (Parquet decoding)

Profile of the full human library (3.49 M precursors, `ODIA_PROFILE=1`): Parquet read+decode was 73% of the
load (3.9 of 5.4 s at 32 threads), dominated by `Precursor.Id` and `Modified.Sequence`. Cause, measured:
DIALibGen writes these as Arrow dictionary arrays with one global dictionary (all ~3.5 M strings), and
Parquet stores that whole dictionary in every row group's dictionary page (~20 MB) — each row group decodes
all 3.5 M strings although it uses ~0.87 M; with more row groups it gets worse (32 → 4.9 s, 128 → 15.6 s).

Done in the tool: row groups read concurrently (own reader each, only the 12 needed columns, column-parallel
decoding only while row groups alone cannot fill the threads), casts per row group, rows parsed in chunks on
all threads, parts joined with concurrent copies. Load 5.4 s → 2.0 s; output identical (31,348,861 traces).
Full-library run (warm page cache, 32 threads) 15.6 s → 13.0 s.

Remaining, in DIALibGen's writer (not this tool): writing the string columns as plain strings (Parquet then
dictionary-encodes per row group as useful) gives 1.17 s with 4 row groups and 0.79 s with 32, and a 26%
smaller file (424 vs 570 MB) — measured on a rewritten copy of the same library.
Cold page cache: reading the 5.8 GB mzML from Ceph adds ~10 s to ingest (the 1.2 GB mzPeak would cut that).
