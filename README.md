# ODIA-V3 — DIATraceExtractor

A standalone OpenMS (TOPP-style, C++23) tool that extracts DIA traces — fragment XICs and MS1 isotope
XICs — for a predicted, recalibrated spectral library, fast and with little memory, and exports them
for point-by-point comparison with DIA-NN's and OpenSWATH's traces. Extraction only (no scoring/FDR).

Design and validation: [docs/01-trace-extractor-design.md](docs/01-trace-extractor-design.md).
Benchmark (ProteoBench Astral entrapment): [docs/02-benchmark-astral.md](docs/02-benchmark-astral.md).

## Build

Needs an OpenMS develop (≥ 3.7) build tree with its vcpkg dependencies (Arrow/Parquet), CMake ≥ 3.24,
GCC ≥ 13. In this project OpenMS is built at `OpenMS/OpenMS-build` (`cmake --preset linux-x64-release`).

```bash
O=$PWD/OpenMS/OpenMS-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$O;$O/vcpkg_installed/x64-linux-dynamic"
cmake --build build -j 16
ctest --test-dir build        # OpenSWATH parity + thread determinism
```

## Use

```bash
# self-calibrated (RT map + mass offsets from the run itself)
build/DIATraceExtractor -in run.mzML -lib library.parquet \
  -rt_mode auto -mass_calibration auto -rt_window 60 -mz_tol 6 -mz_tol_ms1 6 \
  -threads 64 -out traces.parquet -out_stats stats.json -out_calibration anchors.tsv

# or with external anchors / a fixed mass correction
build/DIATraceExtractor -in run.mzML -lib library.parquet -rt_mode anchors -rt_anchors anchors.tsv \
  -mz_offset -2.5 -mz_offset_ms1 0.5 -threads 64 -out traces.parquet
```

- Input: centroided mzML (streamed) or Thermo `.raw`; libraries: DIA-NN/DIALibGen `.parquet`/`.tsv`,
  OpenSWATH `.tsv`/`.pqp`/`.TraML`; `-ids` restricts to a precursor list; `-top_fragments` (default 6).
- RT: `-rt_mode identity|minutes|linear|trafo|anchors|auto` maps library RT to run seconds (`auto`:
  self-calibration from co-eluting anchors); `-rt_window` is
  a half width (≤ 0 = whole run).
- m/z: `-mz_tol`/`-mz_tol_ms1` are ± ppm half widths; `-mz_offset*` apply a mass correction;
  `-aggregation sum` (OpenSWATH) or `max` (DIA-NN); `-ms1_isotopes` (default 2);
  `-ms1_isotope_spacing unit` reproduces OpenSWATH's charge-independent isotope positions.
- Output: traces Parquet (one row per trace on the global cycle axis, intensity as float32 blob or
  `-encoding log8`), `<out>.axes.parquet` (global axis, exact acquisition times), optional long TSV.

```python
import numpy as np, pyarrow.parquet as pq
t = pq.read_table("traces.parquet").to_pandas()
axes = pq.read_table("traces.axes.parquet").to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
row = t.iloc[0]; y = np.frombuffer(row.intensity, "<f4"); rt = cycle_rt[row.cycle_begin:row.cycle_begin + len(y)]
```

## Layout

| path | content |
|---|---|
| `src/` | the tool (`DIATraceExtractor`), library readers, run grid, extractor, writer; `OpenSwathXicToTraces` |
| `tests/` | ctest: OpenSWATH parity on OpenMS test data, thread determinism |
| `scripts/` | benchmark driver, DIA-NN 1.7.12 `.xic` reader, library converters, trace comparison |
| `docs/` | design, benchmark reports |
| `library/` | DIALibGen 0.11.0 Astral libraries (human proteome, ProteoBench entrapment) + `generate.sh` |
| `benchmark-data/` | ProteoBench Astral entrapment raw / mzML / mzPeak |
| `tools/` | DIALibGen (local OpenMS-develop compat branch), DIA-NN 1.7.12 (+ `--xic` export fixes), envs |
| `OpenMS/` | OpenMS develop checkout and build |
| `handoff/` | ODIA handoff package and knowledge graph (read-only reference) |
