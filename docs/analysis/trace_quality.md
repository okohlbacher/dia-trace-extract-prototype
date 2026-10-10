# Trace quality by extraction rule: interference, shape, MS2/MS1 tolerance (2026-10-10)

Data: entrapment tuned library, run 01, with run 02 as confirmation. There is no ground truth for traces, so this judges the
extraction **rules**: all engines read the same centroided spectra.

Scripts:

| script | role |
|---|---|
| `scripts/quality_reextract.py` | Reads the mzML directly (index offsets, 16 processes). For a random sample of **3000 IDs (seed 20261011)** it records every centroid within ±25 ppm of each fragment and MS1 isotope target, at every point of ours-A's trace. |
| `scripts/quality_lib.py` | Rule engine and shape metrics. |
| `scripts/quality_sample.py` | Calibration, validation, interference/capture/shape per rule, tolerance sweep. |
| `scripts/quality_ms1_examples.py` | MS1 rules and the interference panels. |
| `scripts/quality_engines.py`, `scripts/quality_engines_fig.py` | Shape metrics on the actual exports of all IDs. |
| `scripts/quality_tradeoff_fig.py` | Trade-off figures. |

Outputs are in `results/analysis/quality/`.

Figures are in `docs/figures/`:
- `quality_runNN_calibration`: mass error vs m/z and vs intensity
- `quality_tolerance_tradeoff_runNN`
- `quality_engine_shapes_runNN`
- `quality_interference_examples_runNN`

## 0. The rule emulation reproduces every engine

On the 3000-precursor sample, the per-trace median Pearson r is **1.0** for every engine. Fraction of traces with r > 0.999:

| engine | MS2, run 01 / 02 | MS1, run 01 / 02 |
|---|---|---|
| OpenSWATH | 99.95% / 99.97% | 100% / 99.8% |
| DIA-NN | 98.2% / 98.8% | 88.8% / 92.7% |
| ours A | 99.5% / 99.6% | 98.7% / 98.9% |
| ours B | 99.1% / 99.6% | 99.1% / 98.7% |

- The DIA-NN MS2 rule uses the quadratic transform from its `--verbose 3` log; its MS1 rule uses ±2.13 ppm at the uncorrected m/z.
- DIA-NN's lower MS1 figure comes from window-edge centroids, because DIA-NN keeps m/z in float32 (see `ms1_divergence.md`).

## 1. The true mass-error distribution (MS2)

Calibration data: apex centroids of the top-6 fragments of q < 0.001 IDs (13,018 points from 2,829 precursors in run 01). The fit is
robust, with an intensity-weighted quadratic in m/z and 3σ(I) trimming (`sample_runNN.txt`, `_calibration_dependence.tsv`).

- **Offset.** A constant explains almost everything: **−3.29 ppm** (run 01) and **−2.47 ppm** (run 02).
  - The m/z dependence is weak: −3.16 / −3.44 / −3.45 ppm at 300 / 800 / 1500 Th.
  - DIA-NN's quadratic is noticeably wrong at the edges: −3.70 / −3.52 / −2.37 ppm on run 01, and −2.23 / −3.08 / **−4.20** on run 02.
  - Ours A's constant (−2.74 / −1.92 ppm) is about 0.55 ppm too high on both runs, so our self-calibration MS2 offset is biased.
- **RT.** No real dependence (bin medians within ±0.2 ppm).
- **Fragment charge.** 2+ fragments sit 0.5 ppm lower than 1+ fragments.
- **Intensity shift (new finding).** Strong centroids drift up by **+1.45 ppm per decade above I ≈ 6×10⁴**.
  - Medians: −3.1 ppm at ≤10⁵, −2.5 at 3×10⁵, −1.7 at 10⁶, ≈0 at 3×10⁶.
  - Run 02 shows the same, at +1.53 ppm per decade.
- **Width.** σ depends on intensity: σ(I) = sqrt(0.83² + 70²/I), i.e. **2.4 / 1.1 / 0.86 ppm at I = 10³ / 10⁴ / 10⁵**. The global robust σ is 1.43 ppm.
- **Heavy tails.** Even for the strongest half of the apex points, ±5.9 ppm around the offset holds only 96% of them; the 99% quantile is ±10.9 ppm.
  Part of this tail is true signal: peptide-specific shifts, the high-intensity drift (e.g. ASLQLPMEK2 y4 in the examples figure). Part is
  interference.

"Interference" below means extracted intensity coming from centroids outside ±3σ(I) of μ(m/z, I). It is an **upper bound**, because
the heavy tails contain real signal.

**MS1 (M+0)** has a different offset: **+1.33 / +1.18 ppm**, with robust σ **0.85 / 0.79 ppm** (0.5–1.0 ppm, smaller at high intensity).

## 2. Interference, multiplicity, capture (sample; peak region = apex ±4 cycles)

**MS2** (run 01 tolerance ±5.91 ppm; run 02 ±8.28 ppm):

| rule | interference in peak (01 / 02) | captured true signal in peak (01 / 02) |
|---|---|---|
| OpenSWATH / ours B (no correction, sum) | 3.1% / 4.7% | **97.3%** / 99.7% |
| DIA-NN (quadratic correction, max) | 2.8% / 4.7% | 99.5% / 99.8% |
| ours A (constant, max) | 2.9% / 4.9% | 99.5% / 99.8% |

- **More than one centroid in the window: ≤ 4×10⁻⁵ of MS2 points.** On Astral, **max vs sum is irrelevant for MS2.**
- OpenSWATH's loss on run 01 comes from its centre: with no correction, the ±5.9 ppm window ends only 2.6 ppm below the true −3.3 ppm offset.
  On run 02 (±8.3 ppm) that edge is far enough away and the three rules are equivalent.

**MS1 M+0** (`sample_runNN_ms1_rules.tsv`; truth = ±3σ around +1.3 ppm):

| rule | interference in peak (01 / 02) | capture (01 / 02) |
|---|---|---|
| ours A / ours B / OpenSWATH (±5.9 or ±8.3 ppm) | **22% / 25%** | 100% / 100% |
| DIA-NN export (uncorrected, ±2.13 / 2.36 ppm) | 5.3% / 10.9% | 91.7% / 95.3% |
| **v_tol (our +1.2 ppm offset, ±2.13 / 2.36 ppm)** | **1.1% / 6.3%** | **97.8% / 99.2%** |
| median offset, ±2.5 ppm | 2.7% / 7.2% | 99.2% / 99.4% |
| median offset, ±3 ppm | 7.7% / 11.6% | ≈100% / 100% |

At MS1 the wide window is clearly bad: 1–4% of points have more than one centroid at ±5.9–8 ppm, and about a quarter of the
peak-region intensity comes from interference.

## 3. Shape quality of the actual exports (all IDs; 79,953 in run 01, 76,656 in run 02)

Common grid: DIA-NN's 60-scan span on the window's spectra. Peak region: ±4 points around each engine's own apex.
Medians are for run 01; the paired comparison is a Wilcoxon signed-rank test, with p < 1e-10 unless stated
(`engines_runNN_paired.tsv`, `_strata.tsv`).

| criterion | best → worst (run 01) | numbers |
|---|---|---|
| fragment co-elution (mean pairwise r) | ours A ≈ DIA-NN > OSW ≈ ours B | 0.541 / 0.540 / 0.484 / 0.485; DIA-NN better than OSW in 61% of precursors |
| library cosine at apex | ours A ≈ DIA-NN > OSW ≈ B | 0.926 / 0.926 / 0.911 / 0.911 |
| dropouts inside peak (mean) | ours A ≈ DIA-NN < OSW ≈ B | 13.0% vs 15.4% |
| isolated spikes (mean) | ours A ≈ DIA-NN < OSW ≈ B | 8.8% vs 11.5% |
| jaggedness | ours A ≈ DIA-NN < OSW ≈ B | 1.98 vs 2.12 |
| MS1 vs fragment co-elution | **v_tol > DIA-NN > v_dnn > ours A > ours B ≈ OSW** | 0.887 / 0.880 / 0.870 / 0.840 / 0.829 / 0.826 |

- **MS1 differences are largest at low intensity.** In the lowest intensity quintile, MS1 co-elution is 0.59 (v_tol) vs 0.43 (ours A) vs 0.37 (OSW).
- **The MS2 differences sit in the low and mid intensity quintiles.** At the top quintile all engines converge.
- **Run 02 (±8.28 ppm) has no MS2 difference between engines.**
  - MS2 co-elution: DIA-NN 0.554, OSW 0.555, ours A 0.556, ours B 0.555. All |median differences| ≈ 0.
  - The run 01 gap is therefore the window-centre effect, not max vs sum.
  - MS1 again favours the narrow window: v_tol 0.888 vs ours A 0.813 and OSW 0.802 (v_tol better in 37% of precursors, worse in 15%).
- **Ours vs the engine whose rules we copy.** Ours A vs DIA-NN and ours B vs OSW have median differences of 0 on every MS2 metric. The tiny
  mean differences in A's favour come from the m/z-dependent correction of DIA-NN.

## 4. How far the MS2 tolerance can be narrowed

Trade-off on the sample (`sample_runNN_sweep.tsv`, figure `quality_tolerance_tradeoff_runNN`). The centre is the constant offset, the
quadratic in m/z, or quadratic plus intensity shift; the **three centre models give practically identical curves**.

| half width (ppm) | capture in peak | interference in peak | co-elution | cosine at apex | dropouts | traces r > 0.99 vs DIA-NN |
|---|---|---|---|---|---|---|
| ±3 | 96.2% | 0.2% | 0.461 | 0.897 | 17.0% | 38% |
| ±4 | 98.3% | 0.8% | 0.506 | 0.913 | 15.7% | 59% |
| ±5 | **99.2%** | 1.9% | 0.519 | 0.916 | 15.0% | 80% |
| **±6** | **99.6%** | 3.0% | **0.523** | **0.919** | **14.8%** | 91% |
| ±8 | 99.9% | 5.0% | 0.521 | 0.920 | 14.8% | 68% |

(Run 01, quadratic plus intensity-shift centre.)

- **To keep 99% / 99.5% of true signal you need ±5 / ±6 ppm**, with any centre model. The raw apex-error quantiles are much wider
  (99% at ±10.9 ppm) because of the heavy tails.
- **Co-elution, cosine and dropouts are best at ±6 ppm** on both runs. On run 02, ±6 gives co-elution 0.540 vs 0.534 at the current ±8.28
  and cuts interference from 4.6% to 2.8%. Narrowing below ±5 ppm costs shape quality quickly; it is signal loss, not interference removal.
- **Intensity-dependent windows (±kσ(I)) look perfect on capture and interference only by construction**, since the truth is ±3σ(I).
  On the independent shape metrics they are **worse** than a fixed ±6: co-elution 0.497 at 3σ(I) vs 0.523. The narrow high-intensity
  windows cut the real tails. Not recommended.

## Recommendation

- **MS2: constant offset plus a fixed ±6 ppm window.**
  - Use the robust median apex error of high-confidence anchors as the offset; −3.3 ppm on run 01, not the current −2.7 ppm estimate.
  - Fix the width at ±6 ppm. DIA-NN's optimised accuracy happens to land there on run 01 (5.9). Where it is wider, as on run 02 (8.3),
    clamp to about ±6. A data-driven equivalent is ≈ 4.2 × the global robust σ.
  - Max vs sum does not matter on Astral MS2. A quadratic or intensity-dependent centre gains nothing measurable.
  - **Effect on DIA-NN agreement:** with our own better offset, exact equality with DIA-NN drops. Traces with r > 0.99 vs DIA-NN go from
    82% (ours A) to 91% at ±6 with our centre. For bit parity, emulate DIA-NN's quadratic transform instead (99%).
  - Use two modes: parity (DIA-NN transform) and quality (own offset, ±6).
- **MS1: our measured offset (+1.2 to +1.3 ppm) plus a narrow window of ±2.1–2.5 ppm** (≈ 3σ of MS1, or DIA-NN's 5 × 70th percentile of
  the residual).
  - This is the single largest quality gain found: MS1-fragment co-elution goes from 0.840 to 0.887, and interference from 22% to 1–3%.
  - Applying the offset is better than DIA-NN's uncorrected export: 97.8% vs 91.7% capture.
- **Keep ours B as is** for OpenSWATH parity. Its run-01 deficit is inherent to OpenSWATH's uncorrected window.

## Caveats

- **The truth is a model.** "Interference" is an upper bound: per-peptide shifts and the high-intensity drift put real signal outside ±3σ(I).
  The shape metrics are the independent judge, and they agree with the capture analysis on where the optimum width lies.
- **Shapes are judged within DIA-NN's 60-scan span** around its own apex. That favours DIA-NN-like apex choices only marginally, since all
  engines use the same grid.
- **MS1 comparisons involve interpolation.** Ours is interpolated twice (MS1 scans → cycle axis → MS2 times); DIA-NN and OSW once.
  This slightly smooths ours, so the MS1 advantage of v_tol is if anything understated.
- **Scope.** Two runs, one instrument (Astral, 2 Th windows), one library.
