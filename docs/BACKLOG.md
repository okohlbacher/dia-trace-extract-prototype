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

## Libraries
- DIALibGen's plain-string Parquet writer (okohlbacher/DIALibGen#4, merged 2026-10-10): the v0.11.1 Astral
  libraries were generated with it; `library/hye_timstof.parquet` had been written by a stale build and was
  regenerated on 2026-10-12 (content identical in every column; 650 → 511 MB; calibration panel load 3.6 → 1.3 s,
  peak RSS of an A01 run 15.4 → 13.8 GB). DIALibGen itself is maintained elsewhere (not tracked here).
- Lower the library-load transient memory peak (decoded row groups + parts + joined copy coexist; ~4.3 GiB).

## Open: data-driven criteria (review of fixed thresholds, 2026-10-11)
- **Mass tolerance estimator.** Now: width = 1.4826 × MAD of the apex errors, tolerance = instrument-specific multiple
  (dossier §7). Tried and rejected as defaults: 99%-coverage of a background-subtracted histogram, a Student-t +
  background mixture (both too narrow / unstable), the anchors' co-elution curve (right on Astral, not on TOF).
  Explore: a kernel-density estimate of the error distribution (as FeatureFinderMetabo's mass-trace detection in
  OpenMS); an anchor-based criterion that reproduces the TOF optimum.
- **Co-elution window**: now measured (stage 2 uses twice the median FWHM of the stage-1 anchors, ≥ 3 cycles; stage
  1 keeps a seed of ±4). On Astral the FWHM is 2 cycles (3.3 s), giving ±4 as before.
- **Anchor acceptance thresholds** (correlation 0.90/0.80, cosine 0.85/0.75, fragments 5/4): a decoy-calibrated score
  cut is implemented (`CalibrationOptions::anchor_fdr`, decoys = fragment windows shifted by `decoy_shift` Th; no longer
  command-line options since V0.2.15)
  but **off by default**: on Astral runs 01/02 such decoys almost never show 4 fragments at one apex, the cut
  collapsed to a negative score, noisy anchors entered (error width 0.93 → 1.18 ppm, tolerance ±5.5 → ±7.1 ppm) and
  DIA-NN agreement fell from 91.2% to 86.3%. False anchors are real peptides' fragments; peptide-like decoys from
  the library generator (`-generation:decoys`) are the right calibration set. Open.
- **RT window**: measured since V0.2.15; since V0.2.16 7 × the robust RT scatter of the anchors about the map + 2 ×
  FWHM per RT bin (`default.yaml`), chosen on all ten benchmark configurations (99.37-100% of DIA-NN's whole peaks
  inside; `results/rtwin/all/`). Open: iterative re-extraction with a narrowed window.
- **Apex intensity cutoff** (`CalibrationOptions::min_relative_intensity`, now 0): evaluate 0 vs 0.1 with the mixture fit.
- **RT model internals** (mode bins 20, mode bandwidth 15 s, min stage-2 window 30 s, min reject 3 s, LOWESS span
  0.15): constants in `CalibrationOptions` since V0.2.15 (only the LOWESS span stays an option); could be expressed
  in measured peak widths / chosen by cross-validation.
- **Nearest-window assignment** assumes non-overlapping isolation windows; overlapping or staggered schemes untested
  (neither of the two target instruments' methods uses them).
- **Generality:** ProteoBench diaPASEF (timsTOF SCP) runs end to end and agrees with DIA-NN 1.7.12 (on our mzML
  export) at equal tolerance as well as on Astral (docs/00-dossier.md §6). The MS1 disagreement (60%) is DIA-NN's
  export rule (uncorrected m/z, MS2 accuracy): 79% when emulated; closed. Mass model: offset + slope × m/z when
  material (`calibration:mz_model`, 2026-10-12), which timsTOF MS1 needs (+15 ppm/kTh). Open: the
  intensity dependence of the errors (weak centroids low, strong high, on both instruments) cannot be modelled
  before extraction; it argues for the ID-screen multiples rather than the anchor width.
- **Tolerance rule:** the trace-quality screen (dossier §7) gives MS2 optima of 8–10σ (Astral) and 4–5σ (timsTOF),
  MS1 4σ on both; the multiples are set per instrument class (`-mz_accuracy_ms2`). Tried and rejected as a measured
  criterion (2026-10-12, `ODIA_PROFILE=1` prints the curves): the anchors' all-fragment co-elution curve, the same on
  the weaker half of the anchors, on the weak fragments only, and on both -- all rise monotonically to 8σ on
  timsTOF too (anchors are present precursors whose fragments stay correlated when the window widens; the ID
  screen's fall comes from interference in weak and absent precursors); and the anchor yield at a 1%
  decoy-calibrated false rate vs the search tolerance, which the shifted-fragment decoys make meaningless (0–5
  decoys). A criterion needs a population with absent peptides: peptide decoys from the library generator.

## From the V0.1 adversarial review (docs/09-adversarial-review-v0.1.md), closed 2026-10-12
- Panel sampling (`keep_every`) now applies to TSV/TraML/PQP too (the file is parsed whole, only the sample stays).
- `TraceWriter` removes only its own shard names (`part-NNN.parquet`) from the output directory.
- `putLarge_` reserves the cache space atomically (no overshoot); `tryCommit_`'s erase is documented as an
  invariant at its one call site inside the `loaded_` loop.
- The OpenMS mzML loader orders centroids like the fast parser and the mzPeak path (stable sort of the quantized
  m/z in file order): with `-aggregation sum` both parsers now give byte-identical traces (run 01, 682,793 traces).
- Overlapping isolation windows are counted at ingest and reported (`overlapping_windows` in the footer and the
  log); still untested on such a scheme.
- Left as is: a precursor whose MS2 cycle range is empty gets no MS1 trace (both axes span the same run; the
  window can miss MS2 cycles but hit MS1 scans only within one cycle of the run's edges); `ReleaseUnused` cannot
  touch other threads' caches (threads × 2 MiB stay mapped); error exits leak the in-flight blocks (process ends).
- Second data set from another instrument for the data-driven criteria: done (ProteoBench timsTOF diaPASEF). The
  Orbitrap Astral and the timsTOF are the only target instruments; no further ones are planned.

## Open
- **Laptop benchmark with V0.3.0** (MacBook Pro M4 Max, `scripts/bench_macos.sh` with the Homebrew release binary):
  requested 2026-10-19; report §6 still shows V0.2.7's laptop series.
- **TOPPBase patch upstream:** `patches/openms-toppbase-deprecated-options.patch` (deprecated options, one-line
  unknown-option message, `--helphelp` listing) to be offered to OpenMS; not yet.
- **Release signing** (waiting on secrets): the macOS `.pkg` is built and tested on every macOS build and published
  once MACOS_INSTALLER_CERTIFICATE_BASE64/_PASSWORD (Developer ID Installer) are set; the tap is updated from the
  workflow once HOMEBREW_TAP_DEPLOY_KEY is set (until then `ci/publish_formula.sh TAG`); Windows code signing needs a
  SignPath project and SIGNPATH_API_TOKEN/SIGNPATH_ORG_ID.
- **Scheduler beyond 64 threads** (lock + look-ahead window): 384 threads were 4.5x slower than 64 on the 7 M
  library (2 × EPYC 9654, 2026-10-15). The default is the physical cores (no cap), so `-threads 64` is advisable on
  such nodes.
- **Library loading:** lower the transient memory peak (decoded row groups + parts + joined copy coexist; ~4.3 GiB);
  optional compact-v2 layout (uint32 m/z, uint16 intensity; docs/03).
- **0.4:** remove the 0.2.14 option names (accepted with a warning since 0.2.16).
- Linux packages need glibc ≥ 2.39 (Ubuntu 24.04 runners); an older-glibc build (manylinux) if users need it.
- 50K shared library (Justin).
- Share the trace export (`benchmark-data/traces-v0.2.1`, 179 GB) and the results with the group.

## Done (recent)
- **Command-line review of 0.3.0** (2026-10-19, `docs/cli-review-0.3/REVIEW.md`; own pass, Codex, Kimi; Vibe gave
  no output): options without effect named in one warning line; errors for colliding outputs, `-rt_trafo_invert`
  without `-rt_trafo`, `-mz_calibration off` without tolerances; `-out_mzml` alone does not open the library; `-ids`
  counts unknown ids; `--helphelp` 118 → 75 lines; help texts completed.
- **User instrument files** (2026-10-19): `$DIATRACEEXTRACTOR_INSTRUMENT_DIR` and the user directory over the
  shipped files.
- **0.3.0** (2026-10-18): instrument defaults as data files, `-instrument`, value sources in the output, the short
  log, the TOPPBase patch (`docs/cli-review/CLI-REVIEW.md`, phase 2); the merge-join fix (traces written past their
  end with an RT window shrinking faster than the RT, V0.2.15-V0.2.16; timsTOF A01).
- **Command line (V0.2.15, 2026-10-15; adversarial review with Codex and Kimi, `results/clicheck/`, `results/rtwin/`):**
  `-rt_mode auto` default; the RT window measured along the gradient; a warning for a wrong RT scale; `-out` one
  Parquet file; consistent names; 13 basic options; parity switches folded into `-scheme B`; several validation
  fixes. V0.2.16: the physical cores by default, the 0.2.14 names accepted again.
- **Release CI (V0.2.6–V0.2.14):** five platforms, required; ctest and an extraction from the installed folder;
  self-contained packages, Homebrew formula, checksums and provenance attestations; macOS signed, notarized, `.dmg`
  stapled, runs after a Homebrew install.
- **Laptop benchmark (V0.2.4-V0.2.7, 2026-10-13):** three defects fixed (`--help` abort, exit-time abort under libc++,
  `-threads 1` on the serial parser); the macOS ingest (libc++ 4 KiB reads → `pread`; the ordered commit's look-ahead):
  M4 Max 16 threads 3.3 → 0.70 s, 1 thread 41 → 6.5 s, byte-identical output.
- **Full trace export for the group (2026-10-13):** `scripts/export_all_traces.sh benchmark-data/traces-v0.2.1`.
- **Peptide decoys for the calibration (2026-10-16, `results/decoys/`):** tried, not adopted (the anchor cut is not
  robust across instruments); kept as the experiment `ODIA_ANCHOR_FDR`.

## Dropped
- Per-peptide RT refinement: to be handled by library refinement (DIALibGen), not in the extractor.
- DIA-NN exact-parity mode.
