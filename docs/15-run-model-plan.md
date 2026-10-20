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
| 2 | done: m/z terms in the run model (`mz`): width along RT and by intensity tenth (MS1, MS2), from the broad first pass (±50 ppm) and the narrowed final pass, with counts, the share beyond the tolerance and a background share (§7) | metadata only | unit test; traces identical on both instruments |
| 3 | tried and rejected: stage 1 split into RT segments (§8) | none: identical anchors | identical anchors and traces at 8/32/all threads, but 0.3–0.6 s slower |
| 4 | done, opt-in: continuous RT window from the run model (`-rt_window_model running`; §8) | window only when chosen | per-run, stratified coverage with uncertainty on the ten configurations and held-out runs, endpoint behaviour, fallback when the model is empty; default only after that |
| 5 | done, opt-in: intensity-dependent fragment tolerance (`-mz_tol_model intensity`; §8) | traces | signal recovery, interference and quantitative bias by intensity; trace-quality screen; DIA-NN agreement |
| — | dropped: anchor reduction, sequential stopping | | |

Defaults stay as they are; the run model is diagnostics for now and the input for steps 4–5 once validated.

## 7. Step 2: the m/z terms on the benchmark runs (2026-10-20)

Astral run 01 with the 7 M library, timsTOF A01; core robust scale (ppm) about the final mass model; "broad" = the
strict stage-1 anchors at ±50 ppm, "final" = the last stage-2 pass; tenths by centroid intensity. Traces identical to
0.4.0 on both (only `odia.run_model` differs).

| | centroids | scale | along RT | weakest / strongest tenth | beyond tolerance | background |
|---|---|---|---|---|---|---|
| Astral MS2 broad / final | 3,153 / 3,691 | 0.92 / 0.98 | 0.79–1.00 / 0.90–1.06 | 1.89 / 0.70 · 2.33 / 0.72 | 2.0% / 1.9% (±8.6) | 1.8% / 2.8% |
| Astral MS1 broad / final | 530 / 606 | 0.36 / 0.39 | 0.30–0.39 / 0.32–0.46 | 0.69 / 0.23 · 0.71 / 0.25 | 9.6% / 3.6% (±1.5) | 7.6% / 4.3% |
| timsTOF MS2 broad / final | 1,752 / 2,235 | 2.59 / 2.71 | 1.9–2.9 / 2.3–3.0 | 5.39 / 1.36 · 5.68 / 1.44 | 2.7% / 2.0% (±10.8) | 1.8% / 1.3% |
| timsTOF MS1 broad / final | 293 / 373 | 2.44 / 2.29 | 1.3–3.9 / 1.4–3.5 | 3.40 / 3.19 · 4.40 / 2.67 (5 / 7 bins) | 9.9% / 6.7% (±8.9) | 9.6% / 7.5% |

- **No sign of the selection loop at today's windows:** the broad widths are not larger than the final ones (they are
  4–16% smaller on MS2, from the stricter, stronger stage-1 anchors -- a different population, so the comparison is
  not a clean test). The final windows are 25× (Astral MS2) to 12× (timsTOF MS1) the scale, so they cannot truncate
  the core; the loop matters once a tolerance is narrowed towards the scale (step 5).
- **The intensity dependence holds in the broad search:** 2.7× (Astral MS2), 4× (timsTOF MS2), 3× (Astral MS1)
  between the weakest and strongest tenth; timsTOF MS1 is too sparse (293–373 centroids) for a firm intensity trend.
- **MS1 apex centroids beyond the tolerance (4–10%) are mostly background** (the background share is about as large):
  a precursor without its own centroid in the window takes a random one.
- The m/z widths along RT agree with the offline analysis (`results/runmodel/mass.py`; timsTOF MS1 varies ×2.6 along
  the gradient).

## 8. Steps 3–5 (2026-10-20)

Data: `results/steps35/` (scripts and tables). Timings on data (2 × EPYC 9654, 112 cores online), shared with other
users (load 23–55), interleaved A/B; extraction at 32 threads as in the benchmark.

**Step 3, stage 1 in RT segments: rejected.** Stage 1 was restructured into waves of ≤ 32 jobs: fragment bounds per
job, the trace gathering split into RT segments of every job (disjoint cells, each computed exactly as before), then
the members scored in parallel; the trace buffers stayed at ≤ 32 × 256 MiB. Anchors (all nine columns 0.4.0 writes)
and traces were identical to 0.4.0 at 8, 32 and all threads on both instruments. But calibration got slower:

| calibration wall (2 repeats) | 0.4.0 | segmented | per window, no 32-thread cap |
|---|---|---|---|
| timsTOF A01, 32 threads | 4.78 / 4.64 s | 5.38 / 5.26 s | 4.84 / 4.78 s |
| timsTOF A01, all threads | 3.68 / 3.77 s | 4.24 / 4.07 s (2× CPU) | 4.17 / 4.17 s |
| Astral 01, 32 threads | 1.17 / 1.21 s | 1.32 / 1.31 s | 1.23 / 1.18 s |
| Astral 01, all threads | 0.95 / 0.92 s | 1.22 / 1.20 s (2.8× CPU) | 1.03 / 1.01 s |

The whole-run gathering is memory-bound: a tile written by many threads (across sockets) and the barriers between
the phases cost more than the better balance gains, and more threads only add CPU time. 0.4.0's 32-thread cap is
right; the code is unchanged.

**Step 4, the RT window from the run model: opt-in, no gain.** `-rt_window_model running`: 7 × `drt_scale(t)` + 2 ×
`fwhm(t)` on the run model's grid; beyond the grid (the outer 5% of the anchors on each side) the curve's maximum.
Offline on the ten configurations (`rtcov.py`; DIA-NN's whole peaks, q ≤ 1%; bootstrap 95% intervals over the IDs;
leave-one-run-out multiple):

| rule | lowest coverage (95% interval) | worst RT tenth | mean half window | multiple, run held out | lowest held-out coverage | mean half window there |
|---|---|---|---|---|---|---|
| bins (default) | 99.41% (99.35–99.46) | 94.1% | 145.8 s | 6.75–7 | 99.30% | 145.4 s |
| running, end value continued | 98.97% (98.90–99.05) | 92.0% | 142.3 s | 7.5 | 99.41% | 152.1 s |
| running, maximum beyond the grid (implemented) | 99.53% (99.49–99.59) | 95.4% | 145.3 s | 6.25–6.75 | 99.24% | 139.5 s |

With the end value continued the window misses the gradient's ends (95.1% of the IDs beyond the grid on Astral 01
with the predicted human library), hence the maximum there. At the same multiple the window is as wide as the bins'
(−0.3%), at the held-out multiple 4% narrower with the same lowest coverage. Full library (timsTOF A01): output −4.1%,
time within noise; Astral 01: same output size. The run model does not narrow the window: the width is set by the RT
scatter, which the bins already follow. Both rules miss in the same RT tenth of Astral run 03 (human library, 94–95%):
a local error the anchors do not see.

**Step 5, intensity-dependent fragment tolerance: opt-in, better traces.** `-mz_tol_model intensity`: the fragment
tolerance per centroid = the base tolerance × the width at its intensity / the overall width, from the broad first
pass's ten intensity bands (factors 0.5–3; Astral 2.0 for the weakest band to 0.63, timsTOF 2.1 to 0.52). Scores of
the tolerance screen (§5 of the release report) on DIA-NN's identified precursors (`step5.py`; Astral 01 tuned library,
75,867 precursors; timsTOF A01, 44,990), at the same base tolerances k × σ:

| | k | co-elution const / intensity | dropouts | library cosine | off-peak share |
|---|---|---|---|---|---|
| Astral 01 | 4 | 0.320 / 0.367 | 35.2 / 27.6% | 0.894 / 0.922 | |
| | 6 | 0.357 / **0.372** | 29.7 / 26.0% | 0.916 / 0.926 | |
| | 9 (default) | 0.369 / 0.370 | 27.2 / 24.9% | 0.924 / 0.928 | |
| | 12 | **0.370** / 0.366 | 26.3 / 23.9% | 0.926 / 0.929 | |
| timsTOF A01 | 3 | 0.175 / **0.231** | 35.1 / 13.2% | 0.521 / 0.633 | 86.2 / 89.5% |
| | 4 (default) | 0.201 / 0.221 | 24.9 / 9.5% | 0.547 / 0.638 | 87.1 / 89.9% |
| | 5 | **0.202** / 0.211 | 18.8 / 7.4% | 0.563 / 0.640 | 87.7 / 90.2% |
| | 6 | 0.195 / 0.203 | 15.1 / 6.1% | 0.572 / 0.644 | 88.0 / 90.4% |

(Astral's off-peak share is 0 at the median: its tuned-library windows are about as long as ±3 W around the apex.)
The gain holds in each tertile of DIA-NN's precursor quantity (timsTOF at k = 3: low 0.075 → 0.099, mid 0.113 → 0.159,
high 0.329 → 0.422; Astral at k = 6: low 0.157 → 0.171, high 0.580 → 0.592). The best co-elution with intensity is
at a smaller multiple (Astral 6, timsTOF 3) and above the best constant one: +0.5% on Astral, +14% on timsTOF.
The cost: more noise admitted where weak centroids get the wider window (timsTOF off-peak +3 points), and larger
output (fewer zeros; full library at the default multiple: timsTOF +14.5%, 22.4 → 25.6 GB; Astral +20%). Time within
noise (timsTOF extraction 30.9 → 32.3 s, Astral 15.9 → 15.8 s; load 26–54).

Not yet shown: that the better co-elution means more or better identifications (needs a scoring step), and that it
holds on the other runs (Astral 02/03, timsTOF B01), with the multiple chosen on runs other than the one tested.
