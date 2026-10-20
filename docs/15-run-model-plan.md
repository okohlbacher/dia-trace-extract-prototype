# Run model: fast estimation, auto-set extraction parameters, m/z interplay — design and plan (2026-10-20)

Status: the run model is integrated (commit 807208d): running estimators over the calibration anchors give, along the
gradient, the RT map's local bias (`drt_center`), the RT error's robust scale (`drt_scale`) and 99th percentile
(`drt_tail99`), and the peak FWHM (`fwhm`). `ODIA::RunModel` is kept in the tool after calibration, written to the
footer (`odia.run_model`) and `-out_stats`, logged with `-debug 1`; it changes nothing in the extraction (traces
identical on Astral run 01 with the 7 M library and timsTOF A01). Analyses: `docs/14-rt-window-estimation.md`.

## 1. What the data say (inputs to the design)

| quantity | Astral (7 M library) | timsTOF A01 | convergence (docs/14) |
|---|---|---|---|
| anchors | 2,024 | 775 | |
| RT error scale along RT | 14–26 s | 18–54 s | 10% at 300 anchors |
| 99th percentile / scale | 1.9–3.3 | 1.9–4.7 | 5% at 300 anchors |
| local RT bias | ±2.5 s | ±7 s | |
| peak FWHM along RT | 2.2–2.4 s | 3.5 s, falling to 1.0 s at the gradient's end | 2.6% at 300, 5.6% at 100 anchors |
| m/z width after offset + slope × m/z, MS2 / MS1 | 1.00 / 0.40 ppm | 2.75 / 2.30 ppm | |
| m/z offset drift along RT, MS2 / MS1 | 0.24 / 0.29 ppm | 1.47 / 2.71 ppm | |
| narrowing from an RT-drift correction, MS2 / MS1 | 0.7% / 5% | 3% / 1% | |
| m/z width, lowest vs highest intensity decile, MS2 | 2.30 vs 0.73 ppm | 5.6 vs 1.4 ppm | |
| same, MS1 | 0.71 vs 0.25 ppm | 8.8 vs 1.8 ppm | |
| MS1 width along RT (timsTOF) | 0.30–0.44 ppm | 1.45–3.49 ppm | |

- The RT window is set by the RT error, not the peak width (FWHM is 1–13% of the window).
- The anchors' RT-error tail is lighter than the identifications' (the window needs 7 × scale for 99.4% of DIA-NN's
  peaks while the anchors' 99th percentile is 2–3 × scale), so any multiple must be calibrated against IDs.
- The m/z error depends strongly on intensity (3–5× between deciles) and only weakly on RT (drift corrections narrow
  ≤ 5%); the MS1 width on timsTOF varies along RT (×2.4).
- Calibration costs 7–8% of a run; most of it is anchor finding (stage 1 scans the whole run; stage 2 the panel
  around the first map), not estimation.

## 2. Fast estimators

The estimation itself (sorting ~2,000 anchors, 40 running windows) takes milliseconds; speed has to come from finding
fewer anchors and finding them faster.

1. **Sequential panel with a stopping rule.** Process the panel in randomised, RT-stratified batches (e.g. 10% each);
   after each batch update the run model and stop when every quantity it feeds has converged: the change of
   `drt_scale` and `drt_tail99` along RT between successive batches < 5% at every grid point (and ≥ 300 used anchors, ≥
   150 per RT segment that the window rule distinguishes), and the m/z model's offset and width stable to 0.1 ppm.
   Bounded by the full panel (today's behaviour as the worst case).
2. **FWHM from fewer anchors.** The FWHM converges with ~100 anchors: take it from the stage-1 anchors (already
   collected) and do not let it drive the panel size.
3. **Stage 1 smaller and better parallelised.** Stage 1 only has to produce a rough map and the stage-2 search width:
   run it on a stratified subset (e.g. 25% of the panel, more if fewer than ~200 stage-1 anchors), and split each
   isolation window's whole-run tile into RT segments so diaPASEF (24 windows) uses all threads.
4. **Reuse.** Stage 2's per-anchor traces (apex, FWHM, mass errors) are all the model needs: no extra extraction.

## 3. Auto-set extraction parameters from the run model

| parameter | today | from the run model | validation |
|---|---|---|---|
| RT half window | 7 × scale + 2 × FWHM per equal-count bin (≤ 8), linear | `k_id × drt_scale(t) + c × fwhm(t)`, continuous; optionally centred on `rt_map + drt_center(t)` | coverage of DIA-NN's whole peaks on the ten configurations ≥ 99.4%, and window width; `k_id` stays 7 unless re-calibrated |
| window centre | the RT map | + `drt_center(t)` | gain is small (bias ≤ 0.3 × scale) — optional |
| co-elution window (calibration, downstream scoring) | 2 × median FWHM, global | 2 × `fwhm(t)` in cycles | anchor yield and mass-width stability |
| trace pool / memory budget | `-buffer_mb` fixed | expected points per trace from the window (`2 × half(t) / cycle time`) | peak RSS |
| MS1 sampling check | none | warn when `fwhm(t)` < 3 MS1 scan intervals | — |
| m/z tolerance | k × width (instrument multiple), constant | intensity-dependent: accept a centroid if `|error| ≤ k × σ(I)`, σ(I) from the anchors' errors by intensity decile; optional `σ(t)` for timsTOF MS1 | trace-quality screen (docs/10 §5) and agreement with DIA-NN; changes traces, so opt-in first |
| downstream smoothing / peak model | — | kernel width `fwhm(t)`; expected RT spread `drt_scale(t)` as a prior | scoring (not in this tool yet) |

## 4. m/z calibration and the RT model: interplay

- **Anchor selection couples them.** Stage 1 searches at ±50 ppm; the anchors fix the first RT map and the mass
  model; stage 2 searches at the narrowed mass window around that map. A wider mass window admits interference and
  biases the RT residuals; a narrower one loses weak anchors and thins the RT model. The stopping rule (§2.1) must
  watch both.
- **RT-dependent mass offset:** not worth it (≤ 5% narrowing on both instruments).
- **Intensity-dependent mass width:** the large effect. It argues for a tolerance that is a function of the centroid's
  intensity (or, equivalently, for down-weighting weak centroids at the window's edge), and it explains why the
  trace-quality screen preferred wide multiples on Astral (weak fragments need them) and narrow ones on timsTOF.
- **Joint diagnostics in the run model:** add `mz_width_ms2(t)`, `mz_width_ms1(t)` and `mz_width_by_intensity` (deciles)
  to the model, so that downstream algorithms see the same picture.

## 5. Review (Codex, 2026-10-20; `docs/review/run-model-plan-codex.md`) and what changes

| finding | verdict | consequence |
|---|---|---|
| fewer anchors contradict the coverage data (worst 5th-percentile draw 97.0% at 300, 97.9% at 1,000 vs 99.4%) | agreed | no anchor reduction; the panel stays at 5% |
| the stopping rule measures agreement between overlapping batches, not accuracy; optional stopping can omit tails | agreed | dropped as proposed; only a procedure validated end to end (many panel orders, held-out runs, adverse draws) could return |
| the neighbourhood was rank-balanced, not the k nearest in RT | agreed, fixed | true k-nearest (contiguous run grown towards the nearer neighbour); unit test with a density gap |
| endpoints smoothed inward and extrapolated as constants | agreed | the model records each point's neighbourhood extent (`support_lo/hi`); endpoints stay descriptive, no window change from them |
| a 99th percentile from ~60 neighbours is about their maximum | agreed | `drt_tail99` is descriptive only |
| anchor residuals are in-sample and a selected population; ten configurations are not ten independent tests | agreed | window multiples stay calibrated against identifications; future validation per run, stratified, on held-out runs, with uncertainty |
| intensity-dependent tolerance has a selection loop | agreed | research item: estimate widths from a broad fixed diagnostic search, check truncation and false matches, validate signal recovery and bias by intensity — not just agreement with DIA-NN |
| thread determinism needs more than a seed | agreed | applies to any batched or RT-tiled calibration: canonical order, tie-breaks, deterministic reductions, overlap and deduplication at tile edges; test anchor identities across thread counts |
| input contracts (span 0, non-finite values, `at()` returning 0, trimmed mean with ties) | agreed, fixed | arguments validated, non-finite anchors dropped, `at()` returns NaN for a missing curve, central half by rank |
| conclusions too strong (Gaussian tails, RT mass drift irrelevant in general, a 0.1 ppm threshold) | agreed | wording softened; thresholds relative to the width |

## 6. Implementation plan (revised)

| step | change | output effect | validation |
|---|---|---|---|
| 1 | done: run model as metadata, estimator fixes above | none (traces identical) | unit tests (gradient, density gap, invalid input) |
| 2 | m/z terms in the run model: width along RT and by intensity decile (MS1, MS2), from a broad fixed diagnostic window as well as the narrowed one, with counts per cell | metadata only | unit test; traces identical |
| 3 | stage 1 split into RT tiles per isolation window (overlapping, deduplicated, canonical order) | none intended: identical anchors | anchor identities and output identical across thread counts; timsTOF calibration time (target −1–2 s) |
| 4 | continuous RT window from the run model, opt-in (`-rt_window_model running`) | window only when chosen | per-run, stratified coverage with uncertainty on the ten configurations and held-out runs, endpoint behaviour, fallback when the model is empty; default only after that |
| 5 | intensity-aware m/z tolerance, opt-in, after the research item above | traces | signal recovery, interference and quantitative bias by intensity; trace-quality screen; DIA-NN agreement |
| — | dropped: anchor reduction, sequential stopping | | |

Defaults stay as they are; the run model is diagnostics for now and the input for steps 4–5 once validated.
