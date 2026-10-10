# Backlog

## In progress elsewhere
- **mzPeak reader for mzpeak-convert 0.7.0 files** (separate agent, PR to OpenMS-mzPeakRW). The vendored
  `src/mzpeak/MzPeakFile` reads OpenMS-mzPeakRW-written files (identical traces) but not converter 0.7.0
  output: `data_kind "data_arrays"` vs `"data arrays"`, split spectrum metadata / precursor tables, chunked
  numpress-linear MS1 profile layout. Also wanted: parallel read (now 8.7 s vs 3.2 s mzML), keep empty
  spectra (23 dropped in run 01, shifts 18 cycle RTs by ≤4.9 ms). When it lands: re-vendor, benchmark
  ingest from `.mzpeak` vs mzML (cold cache).

## Waiting
- **DIALibGen PR #4** (plain string columns): after merge, regenerate the libraries and re-measure load
  time and the library-load memory peak (~4.3 GiB transient).

## Open
- Lower the library-load transient memory peak (decoded row groups + parts + joined copy coexist).
- Run the entrapment library through the benchmark.
- 50K shared library (Justin).
- Per-peptide RT refinement (self-calibration is at the ceiling of predicted RTs: ~91% within ±60 s).
- Optional compact-v2 library layout (uint32 m/z, uint16 intensity; docs/03).
- Share results with the group.
