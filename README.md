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
vcpkg tree provides them; another `BoostConfig.cmake` directory can be given with `MZPEAK_BOOST_DIR`. The instrument
files are read with yaml-cpp, also from that tree (`yaml-cpp` in the manifest, `ci/patch_openms_manifest.py`).

From 0.3.0 OpenMS needs one patch to `TOPPBase` (`patches/openms-toppbase-deprecated-options.patch`: deprecated options
left out of `-write_ini`, a one-line message for an unknown option); the release workflow applies it. Locally:
`git -C OpenMS apply ../patches/openms-toppbase-deprecated-options.patch`, then rebuild the `OpenMS_CLI` target.

```bash
git submodule update --init
O=$PWD/OpenMS/OpenMS-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$O;$O/vcpkg_installed/x64-linux-dynamic"
cmake --build build -j 16
ctest --test-dir build        # OpenSWATH parity, thread determinism, the command line
```

## Use

```bash
# the defaults: RT map, RT window, mass offsets and tolerances measured from the run; all physical cores
DIATraceExtractor -in run.mzML -lib library.parquet -out traces.parquet
# a library tuned to this run (RT in run minutes, as DIALibGen -mode tune writes): skip fitting the RT map
DIATraceExtractor -in run.mzML -lib library.parquet -rt_mode minutes -out traces.parquet -out_stats stats.json
# OpenSWATH's extraction rules, fixed tolerances (+- ppm)
DIATraceExtractor -in run.mzML -lib library.parquet -scheme B -mz_tol_ms2 6 -mz_tol_ms1 6 -out traces.parquet
# a converted mzML without the instrument in its metadata: name it (sets the tolerance multiples)
DIATraceExtractor -in run.mzML -lib library.parquet -instrument Astral -out traces.parquet
# only convert a run (e.g. diaPASEF mzPeak) to mzML for tools that do not read mzPeak
DIATraceExtractor -in run.mzpeak -out_mzml run.mzML
```

`DIATraceExtractor --help` lists the 14 everyday options, `--helphelp` all of them; a mistyped option gets one line
with the closest name (`Unknown option '-mz_tolx' (did you mean -mz_tol_ms2?).`). The option names of 0.2.14 and earlier still work
until 0.4 (one `WARNING:` line names the current ones); `-write_ini new.ini -ini old.ini` rewrites an old INI file on
the current names. Option names follow one
pattern: `rt_*` for retention time, `mz_*` for m/z with `_ms2`/`_ms1` for the level, `ms1_*` for MS1 traces, `out_*`
for output files.

- **Input:** `-in` is a centroided mzML, an mzPeak `.mzpeak` (MS1 must be centroided in the file, e.g. mzpeak-convert
  `--representation centroid --representation-levels 1`) or a Thermo `.raw`. `-lib` is a DIA-NN/DIALibGen
  `.parquet`/`.tsv` or an OpenSWATH `.tsv`/`.pqp`/`.TraML` library. `-ids` restricts the extraction to a precursor
  list; `-top_fragments` (default 6, 0 = all).
- **RT:** `-rt_mode auto` (default) fits the library-to-run RT map from the run itself and works with any library
  RT scale (predicted, iRT, minutes); `minutes` and `seconds` take the library RT as run time; `-rt_anchors` or
  `-rt_trafo` give a map. `-rt_window` (half width) is by default measured along the gradient: 7 × the robust RT scatter
  of the calibration anchors about the map + 2 × their peak width (FWHM), per RT bin (`rt_window_sigma`,
  `rt_window_fwhm` in `default.yaml`). On the ten benchmark
  configurations (Astral runs 01–03 with tuned and predicted libraries, timsTOF A01/B01) it contained 99.4–100% of
  DIA-NN's whole peaks; about ±40–90 s with tuned libraries, ±250–300 s with predicted RT. A value in
  seconds fixes it, a negative value extracts the whole run. The tool warns when the library's RTs map onto only a
  small part of the run, the sign of a wrong `-rt_mode`.
- **Instrument:** the instrument-dependent defaults are data files, `share/DIATraceExtractor/instruments/*.yaml`
  (`data/instruments/` in the source tree): `default.yaml` holds every default, one file per instrument or class only
  what differs (`orbitrap.yaml` 9, `astral.yaml` 9, `tof.yaml` 4, `timstof.yaml` 4: the fragment tolerance as a
  multiple of the measured width; each value with its source). `-instrument auto` (default) picks the file from the
  run's metadata, the most specific match (`timsTOF` over `TOF`); `-instrument <name>` names it (a file name or alias,
  e.g. `Astral`, `timsTOF SCP`, or any instrument name classified like the metadata, e.g. `Exploris 480` → Orbitrap);
  `-instrument_file my.yaml` uses a file of your own for one run (keys `name`, `extends`, `aliases`, `keywords`,
  `mz_accuracy_ms2`, `mz_accuracy_ms1`, `rt_window_sigma`, `rt_window_fwhm`; unknown or repeated keys, wrong types and
  values out of range are errors). Per value: the option given > the instrument's file > the files it extends >
  `default.yaml`. The log names the files used; the Parquet footer (`odia.instrument*`, `odia.value_sources`) and
  `-out_stats` record each value used with its source (`given`, `instrument-default:Astral`, `default`, `measured`).
- **m/z:** the tolerances `-mz_tol_ms2`/`-mz_tol_ms1` (± ppm) are by default measured: `-mz_accuracy_ms2` and
  `-mz_accuracy_ms1` × the robust width of the mass errors (the instrument's multiples: Orbitrap and Astral 9, timsTOF
  and other TOFs 4, others 6 for fragments; 4 for MS1), with the measured offsets (and an m/z-dependent term when the data show one) corrected
  (`-mz_calibration auto`; `off` uses `-mz_offset_ms2`/`-mz_offset_ms1`). `-scheme B` applies OpenSWATH's rules as one
  preset (sum of the centroids, MS1 isotopes at unit spacing, every window containing the precursor, no mass
  correction). `-no_ms1` extracts fragments only; `-ms1_isotopes` (default 2) after the monoisotopic peak.
- **Log:** one short line per stage (`Library:`, `Run grid:`, `Instrument:`, `Calibration:`, `Extraction:`,
  `Written:`, `Total:`); every warning starts with `WARNING:`; `-debug 1` adds the development detail (memory, the
  calibration search, the scheduler, the files searched, every value with its source).
- **Output:** `-out` is one Parquet file with one row per trace (MS2 rows on the global cycle axis, MS1 rows on the
  MS1 scans), written by 32 writers in parallel; the axes go to `<out stem>.axes.parquet`. `-out_tsv` writes a long
  TSV, `-out_stats` a JSON summary, `-out_calibration` the calibration anchors.

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
