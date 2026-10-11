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

## Open: data-driven criteria (review of fixed thresholds, 2026-10-11)
- **Mass tolerance estimator.** Now: Student-t + flat-background mixture of the apex mass errors, tolerance = 99%
  coverage of the peak (`calibration:accuracy_coverage`). Explore: kernel-density estimate of the error
  distribution (as FeatureFinderMetabo's mass-trace detection in OpenMS) and choosing the coverage by a quality
  objective (fragment co-elution of re-extracted anchors vs tolerance). Also: remove the strict-anchor restriction
  of the mass model once the background component is validated on a full predicted library.
- **RT window.** Now fixed `-rt_window 60`. Explore: peak width and RT-map uncertainty as a function of RT from the
  calibration anchors (iterative refinement, iterative re-extraction with a narrowed window).
- **Co-elution window ±4 cycles** (`calibration:half_window_cycles`): open question whether to derive it from the
  measured peak width in cycles of the strict anchors (cycle time and chromatography dependent).
- **Anchor acceptance thresholds** (correlation 0.90/0.80, cosine 0.85/0.75, fragments 5/4): open; a decoy-calibrated
  score cut-off (same scoring on decoys or mass-shifted fragments, cut at ~1% false anchors) would replace them.
- **Apex intensity cutoff** (`calibration:min_relative_intensity`, now 0): evaluate 0 vs 0.1 with the mixture fit.
- **RT model internals** (mode bins 20, mode bandwidth 15 s, min stage-2 window 30 s, min reject 3 s, LOWESS span
  0.15): configurable now; could be expressed in measured peak widths / chosen by cross-validation.
- **Nearest-window assignment** assumes non-overlapping isolation windows; overlapping or staggered schemes untested.
- **Generality:** validate on a second instrument (a ProteoBench timsTOF DIA module).

## Open
- Lower-priority: scheduler beyond 64 threads (lock + look-ahead window); compact-v2 library layout.
- Lower the library-load transient memory peak (decoded row groups + parts + joined copy coexist).
- 50K shared library (Justin).
- Optional compact-v2 library layout (uint32 m/z, uint16 intensity; docs/03).
- Share results with the group.

## Dropped
- Per-peptide RT refinement: to be handled by library refinement (DIALibGen), not in the extractor.
- DIA-NN exact-parity mode.
