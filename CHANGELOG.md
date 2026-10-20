# Changelog

Behaviour changes a user must know about, in plain words. Releases before 0.3.0 are summarised in
`docs/10-release-report.md` (Appendix A and the repository paragraph).

## Unreleased

- **Run model (metadata only; the traces are unchanged):** the output footer (`odia.run_model`) and `-out_stats`
  (`"run_model"`) record what the calibration measured about the run, for downstream algorithms: along the gradient,
  the RT map's local bias, the RT error's robust scale and 99th percentile, and the peak FWHM; and the m/z errors of
  MS2 and MS1 (`"mz"`), each from the broad first pass (±50 ppm) and from the narrowed final pass, about the mass
  model -- their width along RT and by intensity tenth, with counts, the share beyond the tolerance and the share of
  random centroids. Logged with `-debug 1`. Descriptive, not a coverage guarantee (`docs/15-run-model-plan.md`).
- **`-mz_tol_model intensity` (advanced, experimental):** the fragment tolerance per centroid scales with the mass-error
  width at its intensity (weak centroids up to 2×, strong ones down to 0.5×), measured by the calibration. Better
  fragment co-elution and far fewer dropouts on the benchmark (timsTOF +10% co-elution at the default multiple, +14%
  at 3σ; Astral about equal at 9σ, +0.5% at 6σ), but 15–20% larger output. Default `constant` (unchanged).
- **`-rt_window_model running` (advanced, experimental):** the RT window from the run model's running estimates
  instead of the bins; the same coverage, no narrower. Default `bins` (unchanged).

## 0.4.0 (2026-10-19)

- **Faster beyond 64 threads:** every finished task woke all idle workers, which queued for the scheduler's lock
  and slept again (3.6 M context switches at 192 threads); now one sleeping worker is woken, and a worker that takes a
  task wakes the next. The units in flight grow with the threads (2 per thread beyond 32; were capped at 64). 7 M
  precursors on 2 × EPYC 9654: 192 threads 22.0 → 11.8 s, 384 threads 40.6 → 12.7 s, 16–96 threads unchanged; the
  output is identical at every thread count. The default (the physical cores) is no longer slower than 64 threads.
- **TSV output memory:** a unit's text is one buffer of the recycling Arrow pool, and the encoded bytes waiting for
  the writers are bounded (1 GiB): the human library to `-out_tsv` peaked at 11.2 GiB, now 6.3 GiB (and 85 → 66 s);
  the TSV is byte-identical.

- **The option names of 0.2.14 and earlier are removed** (accepted with a warning since 0.2.16): `-mz_tol`,
  `-mz_sigma_ms2`/`_ms1`, `-mass_calibration`, `-mz_offset`, `-calibration:mass_model`, `-out_shards`, `-rt_mode
  identity|linear|anchors|trafo`, `-ms1_isotopes -1` and the 30 options removed in 0.2.15 are errors now; the error is
  preceded by one line naming the replacement (`-mz_tol was renamed to -mz_tol_ms2 in 0.2.15 ...`), and an `-ini` file
  of 0.2.14 by a line saying so. Convert an old INI file with 0.3 first (`-write_ini new.ini -ini old.ini`).
  `--helphelp` no longer lists them.

## 0.3.1 (not released; part of 0.4.0)

- **Instrument files of your own for every run:** `$DIATRACEEXTRACTOR_INSTRUMENT_DIR`, then the user directory
  (`~/.config/DIATraceExtractor/instruments`, or `$XDG_CONFIG_HOME/...`; macOS `~/Library/Application Support/...`;
  Windows `%APPDATA%\...`), then the shipped files: a file replaces the shipped one of the same name or adds an
  instrument. The log names where a file came from; its values are recorded as `user-file:<file>` with its path and
  hash. A `$DIATRACEEXTRACTOR_INSTRUMENT_DIR` that is not a directory is an error.

- **Command-line review of 0.3.0** (`docs/cli-review-0.3/REVIEW.md`; Codex, Kimi and the binary):
  - options without effect in the given combination are named in one `WARNING: without effect here:` line (e.g.
    `-ms1_isotopes` with `-no_ms1`, `-mz_accuracy_ms2` with a fixed `-mz_tol_ms2`, `-rt_mode` with a whole-run window);
  - errors before any work: two outputs on one path or an output over an input; `-rt_trafo_invert` without
    `-rt_trafo` (it was accepted with `-rt_anchors`); `-mz_calibration off` without fixed tolerances (as its help said;
    before, measured tolerances were centred on the uncorrected m/z when the RT calibration ran);
  - `-out_mzml` alone no longer opens `-lib`;
  - `-ids` counts the ids not in the library; none matching is an error that says so (was "Library is empty.");
  - `--helphelp` names the deprecated options once at the end instead of listing them (118 → 75 lines); help texts
    self-contained and complete (the id format, the RT window's fallback, the instrument files' keys, mzPeak's
    centroided MS1, the measured instruments).

## 0.3.0 (2026-10-18)

The command line and instrument defaults cleanup (phase 2 of `docs/cli-review/CLI-REVIEW.md`). The traces are the
same as with 0.2.16 for every input tried (Astral run 01 and timsTOF A01 at the defaults; only new footer keys differ).

- **Fixed: traces written past their end (V0.2.15-V0.2.16).** With the measured, RT-dependent window, where the
  window shrinks faster than the RT grows (timsTOF A01), a precursor's window could end before an earlier one's; the
  extraction then wrote beyond that trace, into the next fragment's trace and sometimes into another precursor's, so
  1-3 traces also differed between identical runs. On A01 with DIA-NN's IDs 59,948 of 404,764 traces change
  (agreement with DIA-NN unchanged to the 4th decimal); B01 and Astral are unchanged. Output is now identical across
  repeats and thread counts.
- **Instrument defaults are data files.** `share/DIATraceExtractor/instruments/` (source: `data/instruments/`) holds
  `default.yaml` (fragment multiple 6, MS1 multiple 4, RT window 7 sigma + 2 FWHM) and one file per instrument or
  class with only what differs: `orbitrap.yaml` and `astral.yaml` (fragment multiple 9), `tof.yaml` and `timstof.yaml`
  (4). Each value cites its source. They reproduce the class rule of 0.2.16 exactly.
- **New `-instrument <name>`** (basic): `auto` (default) takes the instrument from the run's metadata as before; a name
  (`Astral`, `timsTOF SCP`, or any instrument name, classified like the metadata: `Exploris 480` → Orbitrap) sets it
  for runs whose metadata lack it, e.g. converted mzML files. Naming an instrument that contradicts the metadata gives a
  `WARNING:`. An unknown name is an error that lists the known ones.
- **New `-instrument_file <yaml>`** (advanced): your own instrument file for one run, over the shipped ones. Unknown
  or repeated keys, wrong types and out-of-range values are errors that name the file and the key. When several files
  match the run's instrument name, the highest `priority` wins (timsTOF 2, Orbitrap and Astral 1, TOF 0: the order of
  the 0.2.16 rule), then the most specific file, then the longest keyword.
- **`-mz_accuracy_ms1` now defaults to 0 = the instrument's multiple** (4 for every shipped file, as before). An INI
  file that carries 4.0 behaves the same.
- **Provenance:** the Parquet footer gains `odia.instrument`, `odia.instrument_run` (the run's own instrument string),
  `odia.instrument_files` (the files used, with SHA-1 hashes) and `odia.value_sources` (each value used and where it
  came from: `given`, `instrument-default:<name>`, `default`, `measured`); `-out_stats` gains `instrument` and `values`.
- **Shorter log.** One short line per stage under the same prefixes (`Library:`, `Run grid:`, `Instrument:`,
  `Calibration:`, `Extraction:`, `Written:`, `Total:`); memory, page faults, the calibration search, the scheduler and
  the writer detail only with `-debug 1` (the `Calibration panel library:` and `Scheduler:` lines too). Every warning
  starts with `WARNING:`. Before/after: `docs/messages-review.md`. Scripts that read the scheduler or calibration
  detail need `-debug 1`.
- **A mistyped option** prints one line with the closest current name and exits with code 6, instead of the whole
  usage text: `Unknown option '-mz_tolx' (did you mean -mz_tol_ms2?).`
- **`-write_ini` writes only the current names.** The names of 0.2.14 (accepted until 0.4) are left out;
  `-write_ini new.ini -ini old.ini` moves an old INI's values onto the current names (`-mz_tol` → `-mz_tol_ms2`,
  `-rt_mode identity` → `seconds`, `-aggregation sum -ms1_isotope_spacing unit` → `-scheme B`, ...) and names what it
  cannot carry over.
- **An old name in an INI file yields to the new name on the command line** (before: an error when the values
  differed); an old name on the command line still overrides the INI, and both names on the same level with different
  values remain an error. An old name in an INI counts as given even at its old default (e.g. `mz_sigma_ms1 4` of a
  0.2.14 INI fixes the MS1 multiple at 4, over an instrument file).
- **Building** needs yaml-cpp (in OpenMS's vcpkg tree, added by `ci/patch_openms_manifest.py`) and the OpenMS patch
  `patches/openms-toppbase-deprecated-options.patch` (applied by the release workflow).
