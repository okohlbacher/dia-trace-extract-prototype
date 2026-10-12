# Backlog

## mzPeak (2026-10-11: mzpeak-openms `partitioned-read-2026-10-11` integrated, submodule external/mzpeak)
- Done: 0.17.2/0.18.0 files read fully (empty spectra kept); partitioned parallel read straight into the peak pool;
  `MzMLToMzPeak` rewritten on the library's streaming writer (RT seconds in, minutes stored);
  `results/mzpeak/run01_openms.mzpeak` regenerated. Point-layout run 01: traces byte-identical to the mzML path;
  ingest 0.85 s at 64 threads (mzML 0.32 s). 0.18.0 (numpress, vendor-centroided MS1): MS2 99.99% identical,
  MS1 76% identical (vendor vs ProteoWizard centroiding), ingest 3.6 s.
- Open (reader): diaPASEF goes through the per-spectrum path (no mobility in the partitioned sink): 20 s ingest;
  ions attached to the first precursor when precursor_index is NULL (handoff 08 §7).
- Open (reader): decode scales to ~10-14 cores only (the library serialises row-group decoding per archive:
  `docs/read-path-performance.md` of the submodule); open() 0.17 s and the metadata pass 0.2 s are serial.
  Target was <= 0.5 s. The chunked/numpress decode (0.18.0) is 4x slower than the point layout.
- Build: `boost-json` and `boost-program-options` added to the OpenMS vcpkg manifest (2026-10-11); no conda
  dependency any more.
- Open (converter): the MS1 centroiding difference (24% of MS1 traces differ from the mzML) is a centroider
  difference, not a defect; decide which centroids are the reference.

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
- **Co-elution window**: now measured (stage 2 uses twice the median FWHM of the stage-1 anchors, ≥ 3 cycles; stage
  1 keeps a seed of ±4). On Astral the FWHM is 2 cycles (3.3 s), giving ±4 as before.
- **Anchor acceptance thresholds** (correlation 0.90/0.80, cosine 0.85/0.75, fragments 5/4): a decoy-calibrated score
  cut is implemented (`calibration:anchor_fdr`, decoys = fragment windows shifted by `calibration:decoy_shift` Th)
  but **off by default**: on Astral runs 01/02 such decoys almost never show 4 fragments at one apex, the cut
  collapsed to a negative score, noisy anchors entered (error width 0.93 → 1.18 ppm, tolerance ±5.5 → ±7.1 ppm) and
  DIA-NN agreement fell from 91.2% to 86.3%. False anchors are real peptides' fragments; peptide-like decoys from
  the library generator (`-generation:decoys`) are the right calibration set. Open.
- **RT window**: `-rt_window 0` = measured (99th percentile of the anchor residuals + peak width; 36–53 s on Astral);
  the default stays 60 s. Iterative re-extraction with a narrowed window: open.
- **Apex intensity cutoff** (`calibration:min_relative_intensity`, now 0): evaluate 0 vs 0.1 with the mixture fit.
- **RT model internals** (mode bins 20, mode bandwidth 15 s, min stage-2 window 30 s, min reject 3 s, LOWESS span
  0.15): configurable now; could be expressed in measured peak widths / chosen by cross-validation.
- **Nearest-window assignment** assumes non-overlapping isolation windows; overlapping or staggered schemes untested.
- **Generality:** ProteoBench diaPASEF (timsTOF SCP) runs end to end and agrees with DIA-NN 1.7.12 (on our mzML
  export) at equal tolerance as well as on Astral (docs/00-dossier.md §6). Open: the all-ID tolerance trade-off on
  this data (our ±16 ppm vs DIA-NN's ±11.8); the MS1 agreement (r > 0.9 for 60% at equal tolerance); run B01 through
  DIA-NN.
- **Tolerance rule:** the trace-quality screen (dossier §7) gives MS2 optima of 8–10σ (Astral) and 4–5σ (timsTOF),
  MS1 4σ on both; defaults 6σ / 4σ. An anchor-based criterion that reproduces the timsTOF optimum is open: the
  anchors' co-elution curve (`calibration:accuracy_sigma 0`) rises to 8σ on both instruments because anchors are
  present, strong precursors; weighting it with the off-peak share, or scoring the anchors' weakest fragments, may
  recover the balance. Peptide decoys from the library generator would give both a cleaner anchor set and a proper
  FDR for the acceptance thresholds.

## Open: from the V0.1 adversarial review (docs/09-adversarial-review-v0.1.md)
- Panel sampling (`keep_every`) only for compact Parquet libraries; TSV/TraML/PQP load the whole library for the panel.
- A precursor whose MS2 cycle range is empty gets no MS1 trace either (MS1-only extraction at the run's edges).
- `TraceWriter` removes every `part-*` file in the output directory before writing.
- `RecyclingPool::ReleaseUnused` leaves the per-thread caches (threads x 2 MiB) mapped; `putLarge_` can overshoot
  the cache cap by one block per thread.
- Error exits leak the in-flight unit's trace blocks (process ends anyway); `tryCommit_` erases a `loaded_` node
  the caller's range-for is bound to (safe only because it returns immediately).
- Tie order of centroids equal at 1e-5 Th differs between the fast and the OpenMS mzML parser (matters for `sum`
  in the last float bit only).
- Second data set from another instrument for the data-driven criteria (ProteoBench timsTOF DIA).

## Open
- Lower-priority: scheduler beyond 64 threads (lock + look-ahead window); compact-v2 library layout.
- Lower the library-load transient memory peak (decoded row groups + parts + joined copy coexist).
- 50K shared library (Justin).
- Optional compact-v2 library layout (uint32 m/z, uint16 intensity; docs/03).
- Share results with the group.

## Dropped
- Per-peptide RT refinement: to be handled by library refinement (DIALibGen), not in the extractor.
- DIA-NN exact-parity mode.
