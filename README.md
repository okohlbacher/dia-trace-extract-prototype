# ODIA-V3 — DIATraceExtractor

A standalone OpenMS (TOPP-style, C++23) tool that extracts DIA traces — fragment XICs and MS1 isotope
XICs — for a predicted, recalibrated spectral library, fast and with little memory, and exports them
for point-by-point comparison with DIA-NN's and OpenSWATH's traces. Extraction only (no scoring/FDR).

How it works (grid, criteria, calibration, parallel layout): [docs/EXTRACTION.md](docs/EXTRACTION.md).
Results and history: [docs/00-dossier.md](docs/00-dossier.md); design notes [docs/01-trace-extractor-design.md](docs/01-trace-extractor-design.md);
benchmark (ProteoBench Astral entrapment): [docs/02-benchmark-astral.md](docs/02-benchmark-astral.md).

## Build

Needs an OpenMS develop (≥ 3.7) build tree with its vcpkg dependencies (Arrow/Parquet), CMake ≥ 3.24,
GCC ≥ 13. In this project OpenMS is built at `OpenMS/OpenMS-build` (`cmake --preset linux-x64-release`).

The mzPeak reader/writer is the mzpeak-openms library (git submodule `external/mzpeak`, tag
`partitioned-read-2026-10-11`), built here against the same Arrow. It needs Boost ≥ 1.89 with `json` and
`program_options`: the OpenMS vcpkg manifest here lists `boost-json` and `boost-program-options`, so the OpenMS
vcpkg tree provides them; another `BoostConfig.cmake` directory can be given with `MZPEAK_BOOST_DIR`.

```bash
git submodule update --init
O=$PWD/OpenMS/OpenMS-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$O;$O/vcpkg_installed/x64-linux-dynamic"
cmake --build build -j 16
ctest --test-dir build        # OpenSWATH parity + thread determinism
```

## Use

```bash
# defaults: mass offsets and tolerances measured from the run (self-calibration), +-60 s around the library RT
# (here in run minutes, as DIALibGen's tuned libraries carry), max over the centroids, MS1 on its own scans
build/DIATraceExtractor -in run.mzML -lib library.parquet -rt_mode minutes \
  -threads 64 -out traces.parquet -out_stats stats.json -out_calibration anchors.tsv
# predicted RT in library units: also fit the RT map from the run
build/DIATraceExtractor -in run.mzML -lib library.parquet -rt_mode auto -threads 64 -out traces.parquet

# or with external anchors / a fixed mass correction and fixed tolerances
build/DIATraceExtractor -in run.mzML -lib library.parquet -rt_mode anchors -rt_anchors anchors.tsv \
  -mass_calibration off -mz_offset -2.5 -mz_offset_ms1 0.5 -mz_tol 6 -mz_tol_ms1 2.5 -threads 64 -out traces.parquet
```

- Input: centroided mzML (parsed in parallel chunks), mzPeak `.mzpeak` (partitioned parallel read; MS1 must be
  centroided in the file, e.g. mzpeak-convert `--representation centroid --representation-levels 1`) or Thermo
  `.raw`; libraries: DIA-NN/DIALibGen `.parquet`/`.tsv`,
  OpenSWATH `.tsv`/`.pqp`/`.TraML`; `-ids` restricts to a precursor list; `-top_fragments` (default 6).
- RT: `-rt_mode identity|minutes|linear|trafo|anchors|auto` maps library RT to run seconds (`auto`:
  self-calibration from co-eluting anchors); `-rt_window` is
  a half width (≤ 0 = whole run).
- m/z: tolerances are multiples of the error widths measured from the run (`-mz_sigma_ms2`, by instrument class:
  Orbitrap/Astral 9, timsTOF 4; `-mz_sigma_ms1` 4), or fixed ± ppm with `-mz_tol`/`-mz_tol_ms1`;
  `-mass_calibration auto` (default) measures the offsets (with an m/z-dependent term when the data show one), `-mz_offset*` set them; `-aggregation max` (default) or `sum` (OpenSWATH); `-ms1_isotopes` (default 2);
  `-ms1_isotope_spacing unit` reproduces OpenSWATH's charge-independent isotope positions.
- Output: traces Parquet (one row per trace; MS2 rows on the global cycle axis, MS1 rows on the MS1 scan axis;
  intensity as float32 blob or `-encoding log8`), `<out>.axes.parquet` (both axes, exact acquisition times),
  optional long TSV.

```python
import numpy as np, pyarrow.parquet as pq
t = pq.read_table("traces.parquet").to_pandas()
axes = pq.read_table("traces.axes.parquet").to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
row = t[t.ms_level == 2].iloc[0]   # MS1 rows index axes[axes.lane == -2] instead
y = np.frombuffer(row.intensity, "<f4"); rt = cycle_rt[row.cycle_begin:row.cycle_begin + len(y)]
```

## Layout

| path | content |
|---|---|
| `src/` | the tool (`DIATraceExtractor`), library readers, run grid, calibration, extractor, writer; `OpenSwathXicToTraces`, `MzMLToMzPeak` |
| `external/mzpeak` | mzpeak-openms (submodule): mzPeak reader and writer |
| `tests/` | ctest: OpenSWATH parity on OpenMS test data, thread determinism |
| `scripts/` | benchmark driver, DIA-NN 1.7.12 `.xic` reader, library converters, trace comparison |
| `docs/` | design, benchmark reports |
| `library/` | DIALibGen 0.11.0 Astral libraries (human proteome, ProteoBench entrapment) + `generate.sh` |
| `benchmark-data/` | ProteoBench Astral entrapment raw / mzML / mzPeak |
| `tools/` | DIALibGen (local OpenMS-develop compat branch), DIA-NN 1.7.12 (+ `--xic` export fixes), envs |
| `OpenMS/` | OpenMS develop checkout and build |
| `handoff/` | ODIA handoff package and knowledge graph (read-only reference) |
