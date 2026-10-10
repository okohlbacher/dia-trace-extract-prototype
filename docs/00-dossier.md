# ODIA-V3 trace extraction: dossier (2026-10-10)

**Goal.** Fast, memory-efficient extraction of DIA fragment and precursor traces (XICs) from a *predicted, recalibrated*
library, exported as Parquet/TSV for comparison with DIA-NN and OpenSWATH. Scoring, FDR and PyProphet are out of scope.

**Tool.** `DIATraceExtractor`, an OpenMS TOPP tool (C++23, OpenMS develop), in the repo okohlbacher/dia-trace-extract-prototype.
- **Run grid.** One global RT axis made of DIA cycles, with a compact peak store of 8 bytes per peak.
- **Library.** Read as a stream, in parts.
- **Work.** A central scheduler runs reading, extraction, encoding and writing on one worker pool.
- **Output.** Traces on the global axis plus their exact acquisition times; deterministic for any thread count.
- **Rule sets.** *A* follows DIA-NN (max of the centroids in the window, mass-corrected); *B* follows OpenSWATH (sum, isotopes at
  1.00336 Th).

**Data.** ProteoBench DIA entrapment, Orbitrap Astral, 15 min, 3 runs (5.8 GB mzML each). DIA-NN is the last open-source
version, 1.7.12, with its `--xic` export patched.

## 1. Libraries: tuned vs untuned (DIALibGen 0.11.1)

Libraries are predicted by DIALibGen 0.11.1. The *tuned* versions come from `-mode tune`: the RT model is fine-tuned on DIA-NN
1.7.12 IDs from run 01, and predicts in run minutes. DIA-NN takes each library as a spectral library (`--lib` TSV, which now has
a `UniprotID` column; without it DIA-NN reported "0 protein groups").

| library | DIA-NN IDs at 1% FDR, run 01 (untuned → tuned) | tuned, runs 02 / 03 | RT error, robust SD (untuned¹ → tuned, runs 01/02/03) | IDs within ±60 s |
|---|---|---|---|---|
| entrapment (7.0 M precursors) | 78,405 → 82,333 (+5%) | 79,545 / 80,058 | 24.2 s → 12.5 / 12.9 / 13.3 s | 94.5% → 97.3–97.9% |
| human (3.5 M precursors) | 84,076 → 93,300 (+11%) | 92,035 / 92,211 | 21.9 s → 7.4 / 7.2 / 7.7 s | 95.4% → 98.3–99.7% |

¹ Untuned: the raw prediction after DIA-NN's own RT calibration (its best case). Tuned: the RT stored in the library, with no
further calibration. Runs 02 and 03 were not used for tuning.

## 2. How similar are the traces? (shared tuned library, all IDs, runs 01–03)

All engines extract from the same library, restricted to DIA-NN's 1% FDR IDs, with the same ±60 s window and the same m/z
tolerance.

![Entrapment](figures/correlation_pairs_entrapment.png)

| pair (entrapment; human similar) | MS2 median r | MS2 r ≥ 0.995 | MS1 median r |
|---|---|---|---|
| ours B (OpenSWATH rules) vs OpenSWATH | 0.9999 | ≈ 99% | 0.9998 |
| ours A (DIA-NN rules) vs DIA-NN | 1.0000 | 81% | 0.988 |
| DIA-NN vs OpenSWATH | 0.994 | 48% | 0.964 |
| ours A vs OpenSWATH | 0.996 | 52% | 0.56² |

² At MS1, OpenSWATH places isotope k at mz + k·1.00336, without dividing by the charge. For charge ≥ 2 its "M+1/M+2" traces are
therefore a different ion. M+0 agrees (r 0.998).

- **We reproduce each engine's rules.** Ours B matches OpenSWATH nearly exactly. Ours A matches DIA-NN at MS2. The remaining
  differences to DIA-NN are explained in §3.
- **The two reference engines disagree with each other** more than either disagrees with its matching configuration of ours.
- **Galleries.** `figures/trace_mirror_random.png` holds 50 random precursors with mirrored pairs (ours A vs DIA-NN | ours B vs
  OpenSWATH | OpenSWATH vs DIA-NN). Precursors are grouped by how the engines diverge:

  | divergence mode | share |
  |---|---|
  | all agree | 17% |
  | minor differences | 25% |
  | MS1 only | 30% |
  | MS2 shape differs | 15% |
  | DIA-NN XIC truncated | 6% |
  | low signal | 6% |
  | scale only | 1% |

  No mode is caused by an RT shift: all engines read the same spectra. `figures/trace_gallery_by_mode.png` shows the same
  precursors overlaid rather than mirrored.

## 3. Which traces are more correct?

There is no ground truth, so we judged the extraction **rules**. We re-extracted 3,000 random IDs directly from the mzML, recording
every centroid near each target. Our emulation of each engine reproduces its exported traces (median r 1.0). Details are in
[analysis/trace_quality.md](analysis/trace_quality.md) and [analysis/ms1_divergence.md](analysis/ms1_divergence.md).

**True MS2 mass error.** Apex centroids of confident IDs give:
- **Offset.** Essentially constant: −3.3 ppm (run 01), −2.5 ppm (run 02).
  - DIA-NN's quadratic correction is *worse* than a constant at the m/z extremes.
  - Our self-calibrated offset is about 0.55 ppm too high.
- **Intensity drift.** Strong centroids drift upward by +1.45 ppm per decade above 6×10⁴.
- **Spread.** σ ≈ 2.4 / 1.1 / 0.86 ppm at intensities 10³ / 10⁴ / 10⁵.

**Interference and signal capture**, inside the peak region:

| | MS2 | MS1 |
|---|---|---|
| DIA-NN | 99.5% capture, ~2.9% interference | export uses the narrow window but at the **uncorrected** m/z: 92% capture, 5% interference |
| ours A | 99.5% capture, ~2.9% interference | wide window: 100% capture, 22% interference |
| OpenSWATH / ours B | 97.3% capture (no mass correction, so the window edge sits close to the true peak position) | wide window: 100% capture, 22% interference |
| **ours, narrow MS1 window (±2.1 ppm, our offset)** | — | **98% capture, 1.1% interference** |

Max vs sum is irrelevant on Astral data: two centroids share a window in at most 4×10⁻⁵ of points.

**Shape quality** on the actual exports, run 01, all IDs:

| | ours A / DIA-NN | OpenSWATH / ours B |
|---|---|---|
| fragment co-elution | 0.54 | 0.48 |
| library cosine at apex | 0.926 | 0.911 |
| dropouts inside the peak | 13.0% | 15.4% |
| isolated spikes | 8.8% | 11.5% |

- On run 02 (±8.3 ppm window) there is no MS2 difference between the engines.
- MS1-vs-fragment co-elution: our narrow MS1 window 0.887 > DIA-NN 0.880 > ours A 0.840 > OpenSWATH 0.826.

**Verdict.**
- **MS2:** DIA-NN-style extraction (ours A) gives the cleanest traces.
- **MS1:** ours with a narrow, mass-corrected window gives the best traces of all, better than DIA-NN's export.
- **OpenSWATH:** its uncorrected window and its isotope spacing cost quality.

**How narrow can MS2 go?** Keeping 99% / 99.5% of the true signal needs ±5 / ±6 ppm. Shape quality peaks at ±6 ppm (co-elution
0.523 at ±6 vs 0.506 at ±4 and 0.461 at ±3). DIA-NN's ±5.9 ppm on run 01 is already right; on run 02 its ±8.3 ppm is too wide.
Windows that widen with intensity do not help.

**Recommended rule for ours A:**
- **MS2:** constant offset from the median apex error, fixed ±6 ppm.
- **MS1:** our offset with ±2.1–2.5 ppm (about 3σ).
- **Ours B:** unchanged, for OpenSWATH parity.

## 4. Performance

Full tuned libraries, run 01, 64 threads, all precursors, top 6 fragments; old binary (whole library in memory) → current
streaming binary:

| library | wall time | peak RSS | page faults |
|---|---|---|---|
| human, 3.49 M precursors, 31.3 M traces | 15.3 → 13.3 s | 4.90 → 3.59 GiB | 4.07 → 2.42 M |
| entrapment, 7.0 M precursors, 63.0 M traces | 25.7 → 21.0 s | 6.68 → 4.49 GiB | 6.41 → 2.68 M |

What the current binary does:
- **Library.** Streamed in parts; it is never resident as a whole.
- **Trace buffers.** A pre-faulted pool of recycled 256 KiB blocks, freed as soon as each unit is encoded.
- **Arrow and Parquet buffers.** A recycling allocator in size classes, which removed about 4 GiB of glibc arena retention.
- **Scheduling.** One worker pool with four task classes (read, extract, encode, write), rebalanced at every task by pipeline
  pressure. Typical split on the 7 M library: about 0.9 workers reading, 25 extracting, 7 encoding and 7.5 writing (writing is
  capped at 8 output files).
- **Determinism.** Output is byte-identical for 1 and 64 threads.

**Runtime vs threads** (run 01, shared tuned library of 82,333 precursors with all fragments, one run per point, nearly idle
node; `scripts/bench_threads.sh`, `results/threads/`):

![Thread scaling](figures/thread_scaling.png)

| threads | ours A | ours B | DIA-NN 1.7.12³ | OpenSwathWorkflow⁴ |
|---|---|---|---|---|
| 4 | 22.9 s / 2.1 GiB | 13.9 s / 2.1 GiB | 115 s / 1.7 GiB | 227 s / 10.5 GiB |
| 8 | 12.7 s | 7.7 s | 59 s | 189 s |
| 16 | 7.4 s | 5.1 s | 34 s | 183 s |
| 32 | 5.9 s | 4.9 s | 26 s | 183 s |
| 64 | 6.1 s / 2.9 GiB | 4.6 s / 2.7 GiB | 27 s / 3.3 GiB | 182 s / 14.1 GiB |

³ DIA-NN can export traces only while it searches, so its time includes the search. ⁴ OpenSWATH's time includes its scoring.

- **Ours A vs ours B.** Ours A pays for its self-calibration of the mass offsets (panel and calibration).
- **Speed.** Ours is 4–5× faster than DIA-NN at every thread count, and 30–40× faster than OpenSWATH at 16+ threads.
- **Where ours stops scaling.** Beyond 16–32 threads, ingesting the 5.8 GB mzML dominates our runtime; mzPeak input should help
  there.
- **OpenSWATH stops scaling at 8 threads,** while its memory grows to 14 GiB.

## 5. Open items

- **Adopt the recommended rule in ours A.** MS2: constant offset from the median apex error, ±6 ppm. MS1: our offset, ±2.1–2.5 ppm.
- **Fix our MS2 offset bias** of about 0.55 ppm.
- **Store MS1 on its own scan axis.** Today it is linearly interpolated onto the cycle axis.
- **mzPeak input.** Waiting on the converter (vendor-centroided MS1) and on a parallel reader (handoffs `04`, `05`).
- **Run ingest** still costs about 1.3 M page faults in the OpenMS mzML parser.
