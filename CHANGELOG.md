# Changelog

Behaviour changes a user must know about, in plain words. Releases before 0.3.0 are summarised in
`docs/10-release-report.md` (Appendix A and the repository paragraph).

## 0.3.1 (unreleased)

- **Instrument files of your own for every run:** `$DIATRACEEXTRACTOR_INSTRUMENT_DIR`, then the user directory
  (`~/.config/DIATraceExtractor/instruments`, or `$XDG_CONFIG_HOME/...`; macOS `~/Library/Application Support/...`;
  Windows `%APPDATA%\...`), then the shipped files: a file replaces the shipped one of the same name or adds an
  instrument. The log names where a file came from; its values are recorded as `user-file:<file>` with its path and
  hash. A `$DIATRACEEXTRACTOR_INSTRUMENT_DIR` that is not a directory is an error.

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
