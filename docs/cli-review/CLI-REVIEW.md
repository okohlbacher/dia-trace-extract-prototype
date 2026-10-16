# DIATraceExtractor: command line and instrument defaults, review (phase 1)

Following `docs/HANDOFF-instrument-defaults-and-cli.md` (the DIALibGen 0.12.1 cleanup as a template). State reviewed:
main at cbae00b (V0.2.15 + physical-core threads, the 7-sigma RT window, library decoys as an experiment). No code is
changed in this phase. Files: `inventory.tsv` (every option, type, default, basic/advanced, help length, uses in docs,
scripts, tests and CI), `help-before.txt`, `helphelp-before.txt`, `defaults-before.ini`.

Counts: 51 options (38 of the tool, 13 of TOPP); `--help` shows 16 (13 of the tool, `-ini`, `-threads`, `-write_ini`),
under the guideline's budget of about 15 per mode and 25 overall. One mode (extraction), plus an export-only use
(`-out_mzml` without a library).

## Findings, checked against the binary

1. **Compatibility broken (critical, guideline section 3.4).** V0.2.15 renamed or removed about 30 options without
   aliases. An old command line fails with exit 6 and prints the whole help (`-mz_tol 5`: "Unknown option(s)
   '[-mz_tol]' given. Aborting!" followed by 33 lines of help); an INI written by V0.2.14 fails with exit 6 ("Unknown
   (or deprecated) Parameter 'mz_tol' given in outdated parameter file!"). Renamed: `-mz_tol` -> `-mz_tol_ms2`,
   `-mz_sigma_ms2/_ms1` -> `-mz_accuracy_ms2/_ms1`, `-mass_calibration` -> `-mz_calibration`, `-mz_offset` ->
   `-mz_offset_ms2`, `-calibration:mass_model` -> `-calibration:mz_model`, `-out_shards` -> `-write_parts`,
   `-ms1_isotopes -1` -> `-no_ms1`, `-rt_mode identity` -> `seconds`, `-rt_mode anchors|trafo` -> implied by the file.
   Removed: `-aggregation`, `-ms1_isotope_spacing`, `-window_assignment` (part of `-scheme B`), `-rt_mode linear` with
   `-rt_intercept/-rt_slope`, `-rt_anchor_model`, `-ingest_threads`, `-library_part_rows`, `-log8_decades`, and 21
   `calibration:*` thresholds.
2. **Instrument defaults are code, not data (section 2).** `instrumentClass()` in `src/DIATraceExtractor.cpp` maps the
   run's instrument string to a class and a fragment multiple (Orbitrap/Astral 9, timsTOF 4, other TOF 4, unknown 6);
   the MS1 multiple (4) and the RT-window rule (7 sigma + 2 FWHM) are constants. Nothing records which source supplied
   a value, and a user cannot add an instrument without a rebuild.
3. **The instrument cannot be named (section 0).** The class comes only from the run's metadata. A converted mzML or
   mzPeak without an instrument entry gets the unknown multiple (6) and a warning; the only remedy is
   `-mz_accuracy_ms2 9`, which the user has to know.
4. **Unknown options print the whole help (section 3.3).** TOPPBase prints the full usage after "Unknown option(s)";
   the cause scrolls away and there is no "did you mean".
5. **Values without their source (section 2.3).** The Parquet footer records the resolved tolerances, offsets and RT
   window, but not whether each came from the user, the instrument class or the generic default.
6. **Runtime messages (section 4).** Warnings are printed by OpenMS without a `WARNING:` prefix of our own; several
   info lines are development diagnostics (the `Calibration:` line is about 900 characters with stage counts, search
   passes and a co-elution curve; `Scheduler:` lists task classes and buffer blocks; `Calibration panel library:`).
7. **Implied values are not visible in the help (section 2.3).** `-mz_tol_ms2` says "measured from the run
   (-mz_accuracy_ms2 x width)" but not what the multiple is for the user's instrument.
8. Fine as is: the threads default (physical cores, an INI value respected, stated in the tool description), the
   basic/advanced split through TOPP's advanced flag, the per-option descriptions with units and defaults, mode
   inference (export when only `-out_mzml` is given), validation of contradictory combinations.

## Proposal

### Instrument defaults as data files

```text
share/DIATraceExtractor/instruments/      (source tree: data/instruments/)
  default.yaml    instrument: auto; mz_accuracy_ms2: 6; mz_accuracy_ms1: 4; rt_window_sigma: 7; rt_window_fwhm: 2
  astral.yaml     name: Astral; aliases: [Orbitrap Astral, Astral Zoom]; mz_accuracy_ms2: 9    # report section 5, screen optimum 8-10
  timstof.yaml    name: timsTOF; aliases: [timsTOF Pro, timsTOF SCP, timsTOF HT, Bruker Daltonics timsTOF series]
                  mz_accuracy_ms2: 4                                                            # report section 5, optimum 4-5
```

- Precedence per value: explicit option (command line or INI) > the instrument's file > `default.yaml`.
- `-instrument auto|<name>` (basic; `auto` = from the run's metadata, the current behaviour). `-instrument_file
  <yaml>` for one run (advanced); `$DIATRACEEXTRACTOR_INSTRUMENT_DIR`, then the user config directory, then the shipped
  directory.
- Strict validation (unknown or duplicate keys, wrong types, ranges) with yaml-cpp from vcpkg (added to the patched
  OpenMS manifest, like boost-json); `.gitattributes` forces LF for the files (their hashes go into the provenance).
- Log: `instrument defaults: astral.yaml (Astral, from the run) over default.yaml` and `Astral defaults: fragment
  tolerance 9 x the measured width, MS1 4 x (-mz_accuracy_ms2 / -mz_accuracy_ms1 change them)`.
- Provenance: `odia.instrument`, `odia.instrument_files` (names and hashes) and, per resolved value, its source
  (`given`, `instrument-default:Astral`, `default`, `measured`) in the Parquet footer and `-out_stats`.
- Other Orbitraps (Exploris, Lumos, QE): no file, so the generic 6 applies, unless the owner wants the Astral value
  extended to them (decision 3).

### Target `--help` (17 lines of the tool's own options + TOPP's 3)

```text
  -in <file>*          DIA run: centroided mzML, mzPeak (.mzpeak) or Thermo .raw.
  -lib <file>          Spectral library: DIA-NN/DIALibGen .parquet or .tsv, OpenSWATH .tsv/.pqp/.TraML.
  -out <file>          Traces (Parquet), one row per trace; the RT axes go to <out stem>.axes.parquet.
  -out_tsv <file>      Traces as a long TSV, one row per point (large).
  -out_stats <file>    Run statistics (JSON): calibration, the values used and their sources, stage times.
  -instrument <name>   auto (default: from the run's metadata) or a name from the instrument files (Astral, timsTOF);
                       sets the tolerance multiples (Astral 9, timsTOF 4, others 6).
  -ids <file>          Extract only these precursors (ids, one per line).
  -top_fragments <n>   Most intense library fragments per precursor (0 = all). (default: 6)
  -rt_mode <mode>      auto | minutes | seconds (default: auto)
  -rt_window <s>       0 (default): measured along the gradient; seconds: fixed; negative: the whole run.
  -mz_tol_ms2 <ppm>    0 (default): measured (the instrument's multiple x the width); ppm: fixed.
  -mz_tol_ms1 <ppm>    likewise for MS1.
  -scheme <name>       A (default, DIA-NN's rules) or B (OpenSWATH's).
  -out_mzml <file>     Also write the run as centroided mzML; alone, only this.
```

### Compatibility (for one minor release, until 0.3)

- Every renamed option stays registered as a hidden advanced alias with "Deprecated (0.2.15, removed in 0.3): use
  -new_name"; old name and new name with different values is an error, on the command line and in an INI.
- Removed options with a fixed equivalent map onto it (`-rt_mode identity` -> `seconds`; `-aggregation sum
  -ms1_isotope_spacing unit -mz_calibration off` -> `-scheme B`); removed options at their old default are accepted
  with one `WARNING:` line; at another value they are an error that names the replacement or says why there is none.
- `-write_ini` writes only the new names; an old INI's values move onto them.

### Messages

- Unknown option: one line, exit 6: `Unknown option '-mz_tol' (did you mean -mz_tol_ms2?)` -- checked in `main()`
  against the registered names before TOPPBase parses, so the help does not bury it.
- `WARNING:` prefix on the tool's warnings, once per run.
- The development detail of `Calibration:`, `Scheduler:` and `Calibration panel library:` behind `-debug 1`; at the
  default level one line each: calibration (anchors, RT map, offsets, widths), tolerances and window, writing.

### Before / after

```text
V0.2.14 -in run.mzML -lib lib.parquet -rt_mode minutes -mz_tol 6 -mz_tol_ms1 0 -mass_calibration auto -threads 64 -out t.parquet
now     -in run.mzML -lib lib.parquet -rt_mode minutes -mz_tol_ms2 6 -out t.parquet        (the old line keeps working until 0.3)
mzML without instrument metadata, Astral:  -in run.mzML -lib lib.parquet -instrument Astral -out t.parquet
```

### Tests (guideline section 6)

Identity of the old full command and the new short one; each instrument file against V0.2.15; file loading, user
directory, `-instrument_file`, per-value precedence and every validation error; each deprecated alias on the command
line and in an INI, the conflict error, a V0.2.14 INI, the `-write_ini` round trip; the `--help` line count (pinned),
the unknown-option message and exit code; CI on all five platforms.

## Decisions for the owner

1. **Compatibility aliases for the V0.2.14 names** until 0.3: recommended yes (old commands and INIs fail today).
2. **Instrument defaults as YAML data files with `-instrument` and yaml-cpp:** recommended yes.
3. **Which instruments get a file:** recommended Astral and timsTOF only (the two with measured optima); other
   Orbitraps and TOFs fall back to the generic 6 until measured. Alternative: keep today's class rule (all Orbitraps 9,
   all TOFs 4).
4. **RT-window rule (7 sigma + 2 FWHM) in `default.yaml`,** not per instrument (its multiple depends on the library
   more than on the instrument): recommended yes.
5. **One-line unknown-option message** (pre-parse in `main()`): recommended yes.
6. **Development detail behind `-debug 1`:** recommended yes; the benchmark scripts that grep `Calibration:` and
   `Scheduler:` lines move to `-debug 1` in the same change.
7. **Release:** the work since V0.2.15 (physical cores, the 7-sigma window, the `.pkg`) as V0.2.16 now, and this
   cleanup as 0.3.0 after phase 2: recommended.

---

## Cross-model review of this proposal (2026-10-16)

Codex, Kimi and Vibe reviewed the proposal independently (`review-codex.md`, `review-kimi.md`, `review-vibe.md`, brief:
`review-brief.md`); each claim below was checked against the code.

**Agreed by all three, confirmed in code, and adopted:**

1. **Decision 3 as recommended would silently change results.** `instrumentClass()` gives 9 to every Orbitrap
   (Exploris, Exactive, Lumos, Fusion, Eclipse, ...) and 4 to every TOF; a generic 6 for instruments without a file
   narrows Exploris windows by a third and widens other TOFs by half, with no warning. Revised: the class rule becomes
   data (`orbitrap.yaml` 9, `tof.yaml` 4, matched by keyword as today; `timstof.yaml` 4 and `astral.yaml` 9 with their
   measured sources on top), so every instrument resolves exactly as in V0.2.15.
2. **Value migrations are not aliases.** `-rt_mode identity|linear|anchors|trafo` fails TOPP's valid-string check, and
   `-ms1_isotopes -1` its minimum, before the tool's code runs. Revised: the old values stay registered (hidden) and are
   translated in the tool; `-rt_mode linear` with `-rt_intercept/-rt_slope` becomes an error naming `-rt_anchors`.
3. **Removed options must be registered,** or every old INI dies (TOPP rejects unknown INI keys). Revised: all ~30 are
   hidden, with an "unset" sentinel default so that presence is detectable on the command line and in an INI; a
   written table (old name, type, default, replacement, preserved or rejected) goes into the implementation.
4. **`-write_ini` writes every registered parameter,** so the hidden aliases would appear in new INIs (and CTD/CWL).
   Revised: either skip them by name (a patch to TOPPBase) or accept that they appear at their unset sentinel -- an
   open point (decision 8).
5. **The unknown-option example contradicts the aliases** (`-mz_tol` will be accepted); the message is for typos of
   current names. A pre-parse must consume the values of value-taking options (`-rt_window -1`, `-mz_offset_ms2 -1.5`),
   know TOPP's own flags and `-opt=value` forms -- or the message is produced inside TOPPBase (decision 5).
6. **Keep the greppable line prefixes** (`Run grid:`, `Calibration:`, `Scheduler:`, `Extraction:`, `Written:`,
   `Total:`): about ten analysis and benchmark scripts and the CI read them. Revised: the prefixes stay at the default
   level with short content; the long detail moves behind `-debug 1`; `docs/messages-review.md` lists before/after.
7. **The alias timeline was zero releases long** ("removed in 0.3" while shipping in 0.3.0). Revised in decision 1.
8. Also adopted: the instrument file hashes are recorded but kept out of any output fingerprint; the footer records
   the run's raw instrument string and the resolved instrument; a `WARNING:` when `-instrument` contradicts the run's
   metadata; `-mz_accuracy_ms1` gets an "unset" default so that an instrument file can override it; the help line of
   `-instrument` names the resolved values instead of hard-coding them; tests for `--helphelp`, a V0.2.14 INI, the
   `-write_ini` round trip; CHANGELOG and messages table.
9. Found by Codex alone, confirmed: any `-ini` counts as "threads given", so a hand-written INI without a `threads`
   entry runs on one thread. Revised: only an INI that contains `threads` counts.

**Rejected:** Vibe's claim that the decoy change (cbae00b) altered default results -- it did not; the library decoys
are used only with the experimental `ODIA_ANCHOR_FDR`.

**Where the reviewers disagree (left to the owner):**

- *Where the one-line unknown-option message is produced:* Codex and Vibe: in TOPPBase (one parser; it also covers
  unknown INI keys) -- a patch to OpenMS that the release CI and the local build must carry until upstreamed. Kimi: a
  careful pre-parse in the free `main()`. Recommendation: the TOPPBase patch, offered upstream.
- *yaml-cpp or OpenMS's own parameter files:* Kimi: yaml-cpp as the guideline says, phased. Vibe: OpenMS's XML/Param
  parser, already in the tree, for files whose instrument-specific payload is one number. Codex: either, but justify
  yaml-cpp's cost. The first yaml-cpp build invalidates the CI's vcpkg cache (1-3 h on every platform, once).
- *Release sequencing:* Vibe: aliases in 0.2.16 now (so the V0.2.15 break is repaired at once). Kimi: cleanup and
  aliases in 0.3.0, removal no earlier than 0.4.0. Codex: either, as long as aliases ship before their removal.
- *Discovery machinery:* Vibe and Kimi: ship the directory and `-instrument`/`-instrument_file` first; the environment
  variable and the user config directory later (additive).

## Decisions for the owner (revised)

1. **Compatibility aliases** for every V0.2.14 name and value, registered hidden with "unset" defaults, translated in
   the tool, conflict = error: **yes** (all three reviewers).
2. **Instrument defaults as data files with `-instrument` (basic) and `-instrument_file`:** **yes**; the environment
   variable and user directory later.
3. **Instrument coverage:** today's classes as data (Orbitrap 9, TOF 4, unknown 6) plus Astral and timsTOF files with
   their measured sources, so results stay identical to V0.2.15: **recommended** (all three reviewers).
4. **RT-window rule in `default.yaml`** (7 sigma + 2 FWHM, with its source): **yes**.
5. **Unknown-option message:** in TOPPBase (patched, offered upstream) or a pre-parse in `main()`: recommended
   TOPPBase.
6. **Diagnostics:** short content under the stable prefixes, detail behind `-debug 1`: **yes**.
7. **Release:** (a) 0.2.16 now with the aliases (repairs the V0.2.15 break at once), cleanup as 0.3.0, aliases removed
   in 0.4.0 -- recommended; or (b) everything as 0.3.0.
8. **Deprecated names in `-write_ini`/CTD output:** skip them by name (TOPPBase patch, together with decision 5) --
   recommended; or let them appear at their unset default.
9. **Parser for the instrument files:** yaml-cpp (the guideline; one-time CI cache rebuild) -- recommended; or
   OpenMS's Param XML.
