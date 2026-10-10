# Backlog

## In progress elsewhere
- **mzPeak reader for files from the latest mzpeak-convert** (okohlbacher/mzPeakConverter, now v0.17.2;
  0.7.0 is outdated, not to be used). Separate agent, PR to OpenMS-mzPeakRW. Our requirements for a
  parallel, partitioned read API: docs/04-mzpeak-reader-handoff.md. The vendored reader reads 0 spectra from
  0.17.2 files. MS1 is profile-only in 0.17.2 Thermo output; vendor-centroided MS1 requested from the converter (docs/05-mzpeak-converter-handoff.md). When it lands:
  re-vendor, feed partitions into the ordered ingest pipeline, benchmark vs mzML (cold/warm cache).

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
