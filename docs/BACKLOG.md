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
  peak RSS of an A01 run 15.4 → 13.8 GB). DIALibGen was rebuilt against OpenMS develop: TOPPBase now lives in
  `OpenMS_CLI`, `OpenMS::String` is gone; both fixed in `tools/DIALibGen`, branch compact-parquet-plain-strings,
  not yet pushed.
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
- **RT window**: measured since V0.2.15 (4 × robust RT scatter of the anchors about the map + 2 × FWHM, per RT bin;
  99.6-99.8% of DIA-NN's whole peaks inside; `results/rtwin/`). Open: iterative re-extraction with a narrowed window;
  the 4 / 2 multiples were chosen on three runs (Astral 01, timsTOF A01/B01) and should be checked on more data.
- **Apex intensity cutoff** (`CalibrationOptions::min_relative_intensity`, now 0): evaluate 0 vs 0.1 with the mixture fit.
- **RT model internals** (mode bins 20, mode bandwidth 15 s, min stage-2 window 30 s, min reject 3 s, LOWESS span
  0.15): constants in `CalibrationOptions` since V0.2.15 (only the LOWESS span stays an option); could be expressed
  in measured peak widths / chosen by cross-validation.
- **Nearest-window assignment** assumes non-overlapping isolation windows; overlapping or staggered schemes untested.
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
- Second data set from another instrument for the data-driven criteria (ProteoBench timsTOF DIA).

## Open
- **Command line (V0.2.15, 2026-10-15; adversarial review with Codex and Kimi, `results/clicheck/`, `results/rtwin/`):**
  done: `-rt_mode auto` default (the old default took library RT as run seconds and silently extracted the wrong
  place for predicted, iRT or minutes libraries); all cores up to 64 unless `-threads` is given (384 threads were
  4.5x slower than 64 on the 7 M library: the scheduler beyond 64 threads, below); the RT window measured along the
  gradient (4 x robust RT scatter + 2 x FWHM per RT bin; 99.6-99.8% of DIA-NN's whole peaks inside, ±60 s: 80% on
  timsTOF); a warning when the mapped library RTs cover under a quarter of the run; `-out` one Parquet file (32
  writers append their row groups to it while extracting; one footer; content-identical across thread counts, bytes
  not); consistent names (`-mz_tol_ms2`, `-mz_accuracy_ms2/_ms1`, `-mz_calibration`, `-mz_offset_ms2`,
  `-calibration:mz_model`, `-no_ms1`, `-write_parts`); 13 basic options in `--help`, the rest under `--helphelp`;
  removed `-aggregation`, `-ms1_isotope_spacing`, `-window_assignment` (part of `-scheme B`, which also assigns every
  containing window), the linear RT map, `-rt_anchor_model`, `-ingest_threads`, `-library_part_rows`, `-log8_decades`
  and 21 calibration thresholds; `-rt_anchors`/`-rt_trafo` select their map; per-anchor FWHM in `-out_calibration`.
  Fixed: `-ids` shrank the calibration panel on TSV/TraML libraries; MS1 off still required an MS1 tolerance; scheme B
  accepted `-mz_offset`; `-mz_offset` was silently ignored with mass calibration on (now an error); a negative
  `-buffer_mb`; calibration failures exited as "corrupt input". Not taken up: one `-calibration` switch for RT and
  mass (Kimi), descriptive scheme names (Codex).
  Open: DIALibGen's command line deserves the same review (`-threads 1` and Q Exactive defaults; long option list).
- **Release CI (V0.2.6–V0.2.12, 2026-10-13/14):** `.github/workflows/release.yml` builds OpenMS (pinned, `ci/openms.lock`) and
  the tool on Linux x86_64/aarch64, macOS arm64 and Windows x86_64/arm64; all five are required, run ctest and an
  extraction from the installed folder in a clean environment, and package self-contained folders (tar.gz, dmg, zip)
  plus a Homebrew formula. Done: repository and tap `okohlbacher/homebrew-odia` public (`ci/publish_formula.sh TAG`
  after each release); Windows packages self-contained (VC++ runtime, vcomp140), arm64 7/7 tests; macOS package
  relocatable (@rpath) and, from V0.2.11, signed with the Developer ID, notarized and the `.dmg` stapled. V0.2.12: the workflow also runs on pushes to
  main and pull requests (Linux x86_64; docs/scripts/results changes skipped), `SHA256SUMS` and a build provenance
  attestation per release file, actions on Node 24 (checkout v7, cache v6, setup-python v7, artifacts v7/v8,
  action-gh-release v3). V0.2.14: the signed tool runs after a Homebrew install (V0.2.11–0.2.13 aborted: Homebrew
  re-signs the rewritten dylibs ad hoc, the hardened runtime refused them; entitlement disable-library-validation;
  and OpenMS's libraries kept the build's rpath to Homebrew's libomp, loading a second OpenMP runtime; removed),
  and CI installs the tarball through a local tap and extracts traces. Open:
  - **Windows code signing**: needs a SignPath project for this repository and `SIGNPATH_API_TOKEN`/`SIGNPATH_ORG_ID`
    (wire like BALL's: zip artifact configuration, test-signing policy first).
  - Publish the tap formula from the workflow (needs a tap-scoped token as a secret) instead of by hand.
  - Linux packages need glibc ≥ 2.39 (Ubuntu 24.04 runners); an older-glibc build (manylinux container) if users need it.
  - Signed tarball/Homebrew installs start slowly the first time (online check of each bundled dylib); a stapled `.pkg`
    would avoid it.
- **Laptop benchmark (2026-10-13, MacBook Pro M4 Max, on battery; `docs/12-macos-benchmark-note.md`, report §6):** done and
  merged (V0.2.4). Three defects fixed: `--help` abort (unregistered subsection), exit-time abort under libc++ (pool never
  destroyed), `-threads 1` on the serial parser (70 → 14 s on the node). Open: re-run the laptop's 1-thread point. **Resolved 2026-10-13 (laptop analysis, commit df71463 on branch `ingest-perf` (rebased on V0.2.6; FileReader in the platform layer),
  note §"Parallel-efficiency analysis"):** the 19–23 s of system time were libc++ `ifstream::read` issuing one 4 KiB `read()`
  per 4 KiB (~2.9 M syscalls on one descriptor, serialised by the macOS kernel; libstdc++ unaffected) → `pread`; and
  `orderedParallel`'s look-ahead of 32 kept the 16 workers waiting 85 % of the ingest → unbounded for the fast path (parked
  chunks hold metadata only). M4 Max 16 threads 3.3 → 0.70 s, 1 thread 41 → 6.5 s, byte-identical output. Cluster re-measured with V0.2.7 on dax (within the Linux run-to-run spread, report §6); laptop re-measured on mains (0.70 s at 16 threads, 6.2 s at 1). Open: full-library extraction at 11.6/16 cores and ~3.9 s outside ingest/extraction.
- **Full trace export for the group (2026-10-12):** extract all mass traces for every arm (ours A-V0.2, ours B, P; DIA-NN 1.7.12
  XICs; OpenSWATH) for every benchmark file (Astral runs 01–03, diaPASEF A01/B01) in both formats (Parquet and TSV), and
  put them on Ceph next to the benchmark data. Rule B is the `-scheme B` preset (V0.2.1). Done 2026-10-13: `scripts/export_all_traces.sh benchmark-data/traces-v0.2.1` (179 GB, layout in its README.txt). Open: share with the group.
- Push the DIALibGen build fix (tools/DIALibGen, branch compact-parquet-plain-strings, 6f26790) and open its PR.
- Scheduler beyond 64 threads (lock + look-ahead window): 384 threads are 4.5x slower than 64 on the 7 M library (2 x EPYC 9654, 2026-10-15); the default stops at 64 meanwhile. Compact-v2 library layout.
- Lower the library-load transient memory peak (decoded row groups + parts + joined copy coexist).
- 50K shared library (Justin).
- Optional compact-v2 library layout (uint32 m/z, uint16 intensity; docs/03).
- Share results with the group.

## Dropped
- Per-peptide RT refinement: to be handled by library refinement (DIALibGen), not in the extractor.
- DIA-NN exact-parity mode.
