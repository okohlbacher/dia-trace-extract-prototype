# MS2 disagreement tail: ours P (DIA-NN parity) vs DIA-NN 1.7.12 (2026-10-10)

Script: `scripts/ms2_tail.py`. It runs on the full trace set (`cmpP.per_trace.tsv`) and on the 3000-precursor centroid sample from
`scripts/quality_reextract.py`. Outputs are in `results/analysis/ms2_tail/runNN/`: `ms2_tail.txt`, `tail_strata.tsv`,
`sample_traces.tsv`, `diann_spectra_check.tsv`. Example figures: `docs/figures/ms2_tail_examples_runNN.png/.pdf`.

## Size of the tail (all MS2 traces, P vs DIA-NN)

| run | traces | r < 0.9 | r < 0.99 | l1 > 0.01 |
|---|---|---|---|---|
| 01 | 931,850 | 3.5% | 9.3% | 17.7% |
| 02 | 894,187 | 2.3% | 6.0% | 11.2% |

The earlier ~8% / ~30% figures were for the old A configuration. Its offset was −2.74 ppm and therefore further from DIA-NN's centre.

## Cause, ranked

**1. Window centre: DIA-NN's m/z-dependent quadratic MS2 correction vs our constant offset. This is 100% of the tail.**

- On the sample I emulated DIA-NN's window exactly:
  - window centre `query = mz + t0·mz² + t1 + t2·mz`, with the coefficients from its `--verbose 3` log;
  - centre, margin (`query·acc`) and peak m/z all in float32;
  - strict bounds, maximum centroid.
- **This reproduces every DIA-NN trace.** Run 01: 34,923 fragment traces, 0 with r < 0.99, 0 with l1 > 0.01. Run 02: 34,996 traces, also 0.
- **The tail is the centre difference and nothing else.** The emulated P rule reproduces P's export (l1 > 0.01 in 0.7% / 0.4% of traces). P-emulation vs DIA-NN-emulation reproduces the observed tail: 17.8% vs 17.8% of traces with l1 > 0.01 on run 01.
- **All 6,225 (run 01) and 3,927 (run 02) deviating sample traces are explained.**
- **The tail grows with the centre difference** Δc = DIA-NN centre − ours. Fraction with r < 0.9, all traces:

  | \|Δc\| | run 01 | run 02 |
  |---|---|---|
  | < 0.1 ppm | 0.5% | 0.4% |
  | 0.25–0.5 ppm | 2.9% | 1.5% |
  | > 0.5 ppm | 5.3% | 3.8–6.5% |

- **The m/z pattern follows Δc, which differs per run** because DIA-NN's quadratic differs per run.
  - Run 01: the tail is worst at 300–800 Th, where DIA-NN's centre is 0.4–0.6 ppm lower than ours.
  - Run 02: it is worst above 800 Th, where DIA-NN's centre is 0.8–1.8 ppm lower.
- **Mechanism (examples figure).** Both windows are ±5.9 / ±8.3 ppm wide but shifted by about 0.5 ppm, so only centroids in that sliver
  at either edge differ.
  - Often these are the **apex centroids of strong peaks**, which drift upward by about +1.5 ppm per intensity decade (see `trace_quality.md`).
    DIA-NN's upper edge sits lower and cuts them (VDFNVPMK2 y6, LTPDPYR2 y6).
  - In weak traces, single noise centroids switch in or out.
  - This is why r < 0.9 concentrates at low intensity (6.8% in the second-lowest DIA-NN intensity decile vs 0.6% in the highest). l1 > 0.01
    peaks at mid-to-high intensity, where an apex point moves.

**2. Float32 window arithmetic (a sub-part of 1).** Emulating DIA-NN's transform in double precision instead of float32 leaves 2.2% / 1.5% of
traces with l1 > 0.01 (0.4% / 0.26% with r < 0.9). DIA-NN's window bounds jitter by up to ~0.1 ppm (float32 ulp at these m/z), and parity
needs float32. Strict vs inclusive bounds make no difference.

**Ruled out (contribution 0):**

| hypothesis | check | result |
|---|---|---|
| Isolation window (b) | DIA-NN XIC times vs our lanes, all precursors | 100% of DIA-NN XIC times are acquired MS2 spectra of **the same window lane as ours**. 0 precursors in another window. Windows do not overlap (0.0009 Th gaps). Only 38 precursors lie within 0.01 Th of a window edge. |
| Fragment m/z (c) | DIA-NN's exported extraction m/z vs our target m/z | Differences are only float32 rounding of the library value: median 0.021 ppm, max 0.059 ppm. |
| Fragment charge (c) | tail rate by fragment charge | 2+ fragments are not special; their rate follows Δc (run 01 4.4% vs 3.4%; run 02 1.6% vs 2.5%). |
| Neutral losses (c) | library contents | The library has none. |
| XIC span (d) | tail rate by common-point count | The rate is flat (2.6–3.6% for 20–60 common points). |
| Time misalignment (d) | DIA-NN times vs acquired spectra | None. |
| Skipped spectra, nearest vs max centroid, intensity precision (e) | full emulation | Excluded by the exact emulation. |

## Verdict

- **(i) A DIA-NN rule choice, emulable exactly.** It is DIA-NN's quadratic MS2 mass correction (here a single RT bin), applied in float32.
  For exact parity, add a mode that takes DIA-NN's transform `t0,t1,t2` (plus the RT-bin centres if DIA-NN uses more than one bin) and computes
  `query = f32(f32(mz) + f32(t0·mz² + t1 + t2·mz))` and `margin = f32(query·acc)`. Then accept centroids with `f32(peak_mz)` strictly inside
  (query − margin, query + margin), and take the maximum. The coefficients are only in DIA-NN's log at `--verbose 3`
  ("Mass correction transform"); refitting the model ourselves would not reproduce DIA-NN's own fit.
- **(ii) Not a DIA-NN bug.** It is a modelling choice plus float32 precision. However, its quadratic is a worse description of the true error than
  a constant (`trace_quality.md` §1: off by up to 1.8 ppm at the m/z extremes on run 02).
- **(iii) Nothing on our side.** Our extraction is exact for our rule.
