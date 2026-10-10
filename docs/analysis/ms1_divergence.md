# MS1 divergence: ours A (DIA-NN rules) vs DIA-NN 1.7.12 (2026-10-10)

Data: entrapment library, DIALibGen 0.11.1 tuned, shared 1% FDR library, runs 01–03 (`results/refined/proteobench_entrapment_astral_nce25_v0.11.1_tuned_run01/runNN`).
Scripts: `scripts/ms1_variants.sh` (re-runs of our extractor, 16 threads), `scripts/ms1_divergence.py` (metrics as in
compare_traces.py, plus point-level agreement). Outputs: `results/analysis/ms1_runNN.{summary,per_trace,strata,v_dnn_by_mz}.tsv`,
`results/analysis/ms1var_runNN/`. DIA-NN was re-run with `--verbose 3` (16 threads) into `results/analysis/diann_v3_runNN/`. Its MS1 XICs are
byte-identical to the benchmark's (79,953/79,953 traces, run 01), and the log shows the MS1 tolerance it actually used.

## Root cause (ranked)

1. **MS1 tolerance, the dominant cause.** DIA-NN does not use the optimised MS2 accuracy (5.91 ppm) for MS1. It uses its own
   `MassAccuracyMs1` = 5 × the 70th percentile of |MS1 residual| after its MS1 correction (diann.cpp ~10007–10010, "Refining mass
   correction" pass): **2.13 / 2.36 / 2.37 ppm** (runs 01/02/03; log: "Recommended extraction MS1 mass accuracy"). The benchmark gives us
   `-mz_tol_ms1 = ` the MS2 accuracy (5.91 / 8.28 / 5.48 ppm). With max aggregation, our 2.5–3.5× wider window catches centroids
   that DIA-NN misses (low-intensity centroids with larger mass error, plus neighbouring interferences). So ours is higher (log2 +0.18),
   and DIA-NN has dropout points: in 11% of the points DIA-NN is 0 where ours is > 0. Run 02 has the widest gap
   (8.28 vs 2.36 ppm) and the worst MS1 agreement. Run 03 has the narrowest gap and the best.
2. **MS1 mass offset, matters only together with the narrow window.** `ms1_XIC()` computes the corrected `query_mz` but extracts at
   the **uncorrected** library m/z (`level(mz, …)`; a 1.7.12 quirk, see diann.cpp:6778–6781). We add +1.20 ppm. Inside ±5.9 ppm the offset
   is irrelevant (v_off ≈ A), but inside ±2.13 ppm it moves the window edge: tolerance alone gives r 0.997 and l1 0.078, and tolerance plus no
   offset gives an exact match.
3. **Residual (~0.5–0.7% of points), at the window edge.** DIA-NN keeps peak m/z, target and margin in float32 with strict bounds. We use
   1e-5 Th fixed point with inclusive bounds. Differing points are almost all single centroids at the ±2.13 ppm edge (DIA-NN 0, ours > 0, or
   the reverse). Above 512 Th, where the float32 ulp doubles, the differing fraction rises from 0.0072 to 0.0101. The rest is float32-minute
   RT rounding in DIA-NN's export (≤ ~0.1% intensity at interpolated points).

Ruled out:
- **Aggregation.** Max vs sum changes nothing (v_sumA ≈ A, v_dnn_sum ≈ v_dnn; Orbitrap MS1 centroids within ±6 ppm are almost always single).
- **RT alignment / axis.** DIA-NN exports raw MS1 scans (nearest scan to its apex ±30 scans). Interpolated onto our cycle axis it matches
  our interpolation exactly once the m/z rule matches (apex dt ≠ 0 drops from 19.7% to 1.4% of traces).
- **Isotopes.** DIA-NN exports **only M+0**, as the max centroid at the uncorrected precursor m/z. No isotopes and no summing.
- **Profile/centroid.** MS1 is centroided in the mzML ("FTMS + p", centroid spectrum). DIA-NN does no MS1 processing on load.

## Numbers (MS1 M+0, ours vs DIA-NN; median over traces; DIA-NN interpolated at our cycle times)

| run | variant (flags) | Pearson r | l1 | log2 ratio | traces all points within 1% | points differing > 1% |
|---|---|---|---|---|---|---|
| 01 | A (benchmark: ±5.91 ppm, +1.20 ppm, max) | 0.9845 | 0.208 | +0.176 | 24.6% | 15.0% |
| 01 | v_off (±5.91, no offset) | 0.9838 | 0.212 | +0.180 | – | 15.2% |
| 01 | v_tol (±2.13, +1.20) | 0.9972 | 0.078 | +0.003 | 35.6% | 8.4% |
| 01 | **v_dnn (±2.13, no offset, max)** | **1.000000** | **0.00002** | **0.000** | **88.0%** | **0.75%** |
| 02 | A (±8.28, +1.10) | 0.9780 | 0.257 | +0.212 | 24.5% | 17.7% |
| 02 | v_tol (±2.36, +1.10) | 0.9999 | 0.011 | 0.000 | 48.9% | 5.8% |
| 02 | **v_dnn (±2.36, no offset)** | **1.000000** | **0.00001** | **0.000** | **92.6%** | **0.43%** |
| 03 | A (±5.48, +1.26) | 0.9954 | 0.106 | +0.085 | 33.1% | 11.8% |
| 03 | **v_dnn (±2.37, no offset)** | **1.000000** | **0.00001** | **0.000** | **90.5%** | **0.57%** |

Stratification of A (run 01, `ms1_run01.strata.tsv`):
- **Intensity is the main modulator.** By DIA-NN MS1 intensity decile, r goes from 0.60 (log2 +3.1) in the lowest decile to 0.9997
  (log2 +0.02) in the highest. Low-abundance centroids scatter more in mass and fall outside ±2.13 ppm more often.
- **m/z and charge.** The divergence is worst at 600–700 Th (r 0.957) and smallest for z = 1 (r 1.000, l1 0.001).
- **RT.** Agreement degrades late in the gradient (r 0.88 in the last RT bin).
- **q-value.** Agreement is worse at higher q-value.

All of these follow from "the wider window adds signal where DIA-NN's narrow window is empty". None of them points to an RT or axis effect.

## Ours A vs OpenSWATH at MS1 (median r 0.55, apex dt 3.3 s)

This is an artefact of mixing isotopes. The 0.55 pools M+0, M+1 and M+2:
- **M+0:** r 0.998, l1 0.06, apex dt 0, for every charge.
- **M+1/M+2, z = 1:** r 0.9998, because the spacing is the same.
- **M+1/M+2, z ≥ 2:** r 0.38 / 0.06 (z = 2), with apex shifts of 13–33 s. OpenSWATH places isotope k at mz + k·1.00336 regardless of
  charge, so it traces a different ion species.

With OpenSWATH's rules (ours B) all MS1 traces agree to 0.9998. The remaining M+0 difference between A and OpenSWATH (r 0.998) is
max vs sum plus our +1.2 ppm offset. Nothing here needs fixing.

## Recommendation (not implemented)

To reproduce DIA-NN 1.7.12's exported MS1 exactly, set extractor A's MS1 rule to **no MS1 offset and an MS1 tolerance equal to DIA-NN's
MS1 accuracy**. Concretely, either:
- pass DIA-NN's value (`-mz_offset_ms1 0 -mz_tol_ms1 <Recommended extraction MS1 mass accuracy>`; it needs `--verbose 2`), or
- add a `diann` MS1 tolerance mode that derives it from our own calibration anchors: tolerance = 5 × the 70th percentile of |MS1 residual
  after the offset). Our run 01 MS1 σ is 0.407 ppm, so for a normal distribution 5 × 1.036σ ≈ 2.11 ppm, against DIA-NN's 2.13.

Remaining edge mismatches (< 1% of points) would need float32 m/z and strict bounds and are not worth emulating.

Caveat: DIA-NN's *scoring* path does not use what the export shows. Scoring uses the corrected m/z (`predicted_mz_ms1`) with the same
narrow window, interpolated linearly at each MS2 scan's time (diann.cpp ~7190–7200). So "corrected + DIA-NN tolerance" (v_tol, r 0.997–0.9999)
is the better model of what DIA-NN scores. The uncorrected export is a 1.7.12 quirk. I would keep A's physically sensible offset and only
adopt DIA-NN's MS1 tolerance (v_tol), adding an `-mz_offset_ms1 0` compatibility flag for exact export parity.

## Side finding: MS2 is not exactly identical either

The median MS2 r is 1.000000, but **31.8% of MS2 traces (A vs DIA-NN, run 01) differ by l1 > 0.01**, and 6.9% have r < 0.9.

Cause: DIA-NN's MS2 correction depends on m/z. It is quadratic in m/z (log transform `1.98e-9·mz² + 3.88e-4 − 5.59e-6·mz` Th), which is
−3.7 to −3.8 ppm at 300–800 Th and −2.4 ppm at 1500 Th. Ours is a constant −2.74 ppm. The deviating fraction tracks this: 36–38% at
300–600 Th and 3.5% at 1200–1500 Th, where the two corrections coincide.

To match it, the MS2 offset would need the same m/z-dependent form (t0·mz² + t1 + t2·mz, RT-binned when DIA-NN uses more than one bin).
