# ODIA-V3 — DIATraceExtractor

A standalone OpenMS (TOPP-style, C++23) tool that extracts DIA traces — fragment XICs and MS1 isotope
XICs — for a predicted, recalibrated spectral library, fast and with little memory, and exports them
for point-by-point comparison with DIA-NN's and OpenSWATH's traces. Extraction only (no scoring/FDR).

How it works (grid, criteria, calibration, parallel layout): [docs/EXTRACTION.md](docs/EXTRACTION.md).
Results and history: [docs/00-dossier.md](docs/00-dossier.md); design notes [docs/01-trace-extractor-design.md](docs/01-trace-extractor-design.md);
benchmark (ProteoBench Astral entrapment): [docs/02-benchmark-astral.md](docs/02-benchmark-astral.md).

Release report (benchmarks against DIA-NN 1.7.12 and OpenSWATH on Astral and timsTOF diaPASEF, with the version of
the tool each number came from): [docs/10-release-report.md](docs/10-release-report.md).

## Install a binary release

Every tag `V<version>` is built by `.github/workflows/release.yml` for Linux (x86_64, aarch64), macOS (Apple
silicon) and Windows (x86_64, arm64) and published on the GitHub release: a self-contained folder with the tool,
the OpenMS and vcpkg libraries it links and OpenMS's shared data. Unpack and run `bin/DIATraceExtractor`
(Linux, macOS: a launcher that sets the library and data paths; Windows: `bin\DIATraceExtractor.exe`).

- **Linux**: `DIATraceExtractor-<version>-linux-x86_64.tar.gz` (built on Ubuntu 24.04; needs glibc >= 2.39), `-linux-aarch64`.
- **macOS**: `DIATraceExtractor-<version>-macos-arm64.tar.gz` or the same folder as a `.dmg` (relocatable: every
  library is bundled and found relative to the tool; signed with the project's Developer ID and notarized from
  V0.2.11 on, the `.dmg` stapled); Homebrew:
  `brew install --formula diatraceextractor.rb` with the formula attached to the release, or
  `brew tap okohlbacher/odia && brew install diatraceextractor` (the tap tracks the latest release).
- **Windows**: `DIATraceExtractor-<version>-windows-x86_64.zip`, `-windows-arm64.zip` (self-contained: the Visual C++
  and OpenMP runtimes are bundled; not code-signed yet, so SmartScreen warns on first start).
- **Verify** (from V0.2.12): `sha256sum -c SHA256SUMS --ignore-missing` against the release's `SHA256SUMS`, and the
  build provenance with `gh attestation verify <file> -R okohlbacher/dia-trace-extract-prototype`.

Pushes to main and pull requests run the same workflow (build, tests, install, smoke tests, packaging) on Linux
x86_64; nothing is published from them.

The OpenMS commit the releases are built against is pinned in `ci/openms.lock`; `ci/package.sh` and the install
rules at the end of `CMakeLists.txt` make the folder (CMake's runtime-dependency resolution collects the libraries).

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
# the defaults: RT map, mass offsets and tolerances measured from the run, all cores (up to 64)
DIATraceExtractor -in run.mzML -lib library.parquet -out traces.parquet
# a library tuned to this run (RT in run minutes, as DIALibGen -mode tune writes): skip fitting the RT map
DIATraceExtractor -in run.mzML -lib library.parquet -rt_mode minutes -out traces.parquet -out_stats stats.json
# OpenSWATH's extraction rules, fixed tolerances (+- ppm)
DIATraceExtractor -in run.mzML -lib library.parquet -scheme B -mz_tol 6 -mz_tol_ms1 6 -out traces.parquet
# only convert a run (e.g. diaPASEF mzPeak) to mzML for tools that do not read mzPeak
DIATraceExtractor -in run.mzpeak -out_mzml run.mzML
```

`DIATraceExtractor --help` lists the 13 everyday options, `--helphelp` all of them. In short:

- **Input:** `-in` is a centroided mzML, an mzPeak `.mzpeak` (MS1 must be centroided in the file, e.g. mzpeak-convert
  `--representation centroid --representation-levels 1`) or a Thermo `.raw`. `-lib` is a DIA-NN/DIALibGen
  `.parquet`/`.tsv` or an OpenSWATH `.tsv`/`.pqp`/`.TraML` library. `-ids` restricts the extraction to a precursor
  list; `-top_fragments` (default 6, 0 = all).
- **RT:** `-rt_mode auto` (default) fits the library-to-run RT map from the run itself and works with any library
  RT scale (predicted, iRT, minutes); `minutes` and `seconds` take the library RT as run time. `-rt_anchors` or
  `-rt_trafo` (advanced) give a map. `-rt_window` is the half width around each precursor: by default 60 s,
  widened to the measured RT scatter where that is larger (about 105 s on timsTOF with predicted RT); a negative
  value extracts the whole run. The tool warns when the library's RTs map onto only a small part of the run, the
  sign of a wrong `-rt_mode`.
- **m/z:** tolerances are measured from the run: multiples of the mass-error width by instrument class (Orbitrap and
  Astral 9, timsTOF 4 for fragments; 4 for MS1), with the offsets (and an m/z-dependent term when the data show one)
  corrected. `-mz_tol`/`-mz_tol_ms1` fix them in ± ppm. `-scheme B` applies OpenSWATH's rules as one preset (sum of
  the centroids, MS1 isotopes at unit spacing, every window containing the precursor, no mass correction).
- **Output:** `-out` is a Parquet dataset (a directory of part files, read as one table by pyarrow, polars or
  DuckDB) with one row per trace: MS2 rows on the global cycle axis, MS1 rows on the MS1 scans. The axes go to
  `<out stem>.axes.parquet`. `-out_tsv` writes a long TSV, `-out_stats` a JSON summary.

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
