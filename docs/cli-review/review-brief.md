Cross-model review of a PROPOSAL (no code changed yet). Review only -- do not modify any file. You have no shell: everything is inlined.

Attack the proposal: what is wrong, missing, risky or over-engineered for a 0.2.x prototype CLI tool? Check it against the guideline. Specifically: (1) the compatibility plan (aliases, removed options at old defaults, INI handling) -- what breaks, what silently changes results; (2) the instrument data files (matching run metadata strings to files, Orbitraps without a file falling to 6 sigma -- silent change for Exploris users?, provenance, yaml-cpp cost); (3) whether -instrument belongs in basic help; (4) the unknown-option pre-parse in main() (TOPP flags, -ini, values that start with '-', e.g. negative numbers like -rt_window -1); (5) moving diagnostics behind -debug 1; (6) anything in the guideline the proposal ignores. Give file/section references, a severity, and concrete failing cases. If something is fine, say so briefly. End with your recommendation for each of the 7 owner decisions.

=== GUIDELINE (docs/HANDOFF-instrument-defaults-and-cli.md) ===
# Handoff: instrument-specific defaults and command-line options (basic vs. advanced)

For coding agents working on a command-line tool (OpenMS/TOPP-based or not) that has instrument-dependent settings and too
many options. It distils what was done, and what went wrong, when DIALibGen 0.12.1 was cleaned up
(repo `okohlbacher/DIALibGen`, PR #5; review `reports/cli-review/CLI-REVIEW.md`, decisions `reports/cli-review/DECISIONS.md`).
DIALibGen is the worked example throughout. Adapt the names; keep the rules.

## 0. Goal

- A user names the instrument and the inputs, and gets the right result. Everything else is inferred, defaulted, or in the
  advanced help.
- No silent behaviour: every default the tool picks for the user is logged in one line and recorded with the output.
- Old command lines, INI files and recipes keep working for at least one minor release.

DIALibGen before and after:

```text
before: -generation:instrument Astral -generation:nce 25 -generation:precursor_mz 380 980 -generation:fragment_mz 150 2000
        -generation:precursor_charges 1 2 3 4 ... (13 -generation:* options) -threads 96
after:  -generation:instrument Astral -generation:variable_modifications 'Oxidation (M)'
```

The library is byte-identical. `--help` went from 126 options to 23, and generate's own help shows 14.

## 1. Work order

1. **Inventory first, change nothing.** Dump the real help from a built binary (`--help`, `--helphelp`, `-write_ini`), and
   list every option in a TSV with these columns: name, mode(s), type, default, help length, advanced?, and where it is
   used (docs, report, benchmarks, CI, GUI). Count the options in total and the ones shown in `--help`.
2. **Adversarial findings, checked in code.** Wrong or unsafe defaults; defaults that force users to spell out what the
   tool could infer; options that are redundant, dev-only, inert or conflicting; help text that is wrong, empty or full of
   jargon. Reproduce each finding with the binary where possible.
3. **Proposal.** The target `--help` written out in full, what moves to advanced, what is removed, renamed or turned into
   an env var (with a reason for each), the new defaults with their source, the compatibility plan, and before/after
   commands for the main use cases.
4. **Cross-model review of the proposal** (Codex, plus Kimi/Vibe if available). The reviewers caught real problems: a fatal
   unknown-INI parameter, silent ID loss from implied windows, and string comparisons of enum options.
5. **The owner decides the open points.** Present them as numbered decisions with a recommendation each. Do not implement
   before that.
6. **Implement on one branch,** one commit per logical change, with tests and identity checks (section 6), then a Codex
   review of the diff and CI on every platform.

## 2. Instrument-specific defaults: data files, not code

### 2.1 Layout

- A directory of default-parameter files shipped with the tool, found by the same data-directory search as models and
  other data. Release: `share/<Tool>/instruments/`; source tree: `data/instruments/`. Embed it in every package (macOS
  `.app` bundle, Windows installer, Linux tarball/AppImage/deb). See the `openms-external-tools` skill for embedding data in
  a macOS `.app`.
- `default.yaml` holds every default that applies when no instrument file overrides it, including the default instrument
  itself. One file per instrument (`astral.yaml`, `timstof.yaml`) holds only what differs.
- Ship files only for instruments with sourced values. The others fall back to `default.yaml`; don't invent method
  windows.

### 2.2 Schema (DIALibGen's, as a template)

```yaml
# Every value cites its source in a comment (report section, benchmark settings, vendor default, upstream code).
name: Astral                        # required; matched ignoring case, ' ', '-', '_', a leading vendor prefix, a trailing model number
aliases: [Orbitrap Astral, Astral Zoom]
model_instrument: Lumos             # required: the slot/group the ML model actually knows (here no Astral slot exists)
ion_mobility: false                 # drives automatic decisions (no CCS/1/K0 prediction for instruments without IM)
nce: 25                             # > 0, <= 100
precursor_mz: [380, 980]            # [min, max]
fragment_mz: [150, 2000]
precursor_charges: [1, 2, 3, 4]     # 1-8, no repeats
```

`default.yaml` holds `instrument` (the default), the generic windows and charges, and a `models:` map giving
per-model-slot fallbacks (`nce`, `ion_mobility`) for instruments without their own file.

### 2.3 Rules

- **Precedence, per value:** explicit option (command line, INI, JSON/recipe) > the instrument's file > `default.yaml`.
  Resolve each value separately, so one explicit option overrides only itself.
- **Derived decisions are pure functions of the resolved instrument** (e.g. ion mobility on or off). A saved recipe, the
  GUI and a re-run then all decide the same way. Expose an override (`-generation:predict_ion_mobility auto|true|false`).
- **User extension:** the search order is an env var directory (`$<TOOL>_INSTRUMENT_DIR`), then a user config directory,
  then the shipped directory. The first file with a given name wins, so a user's `astral.yaml` replaces the shipped one.
  Add an explicit `-instrument_file path.yaml` for a single run.
- **Strict validation:** unknown keys, duplicate keys, wrong types and out-of-range values are errors that name the file
  and the key. Use an established parser (DIALibGen: yaml-cpp 0.8 on every CI platform, notice added to the licence
  bundle). Never hand-roll YAML.
- **Logging:** one line naming the files used, and one line giving the values an instrument file implied plus how to
  change them:
  ```text
  instrument defaults: astral.yaml (Astral) over default.yaml
  Astral method defaults: precursor m/z 380-980, fragment m/z 150-2000, charges 1 2 3 4 (... e.g. -generation:precursor_mz 350 1200 for a wider window)
  ```
  The full paths and the directories searched go behind `-debug 1`.
- **Provenance:** the output's recipe records each resolved value with its source (`instrument-default:Astral`,
  `default`, `config`, `TOPP`), plus the file names and content hashes. Keep the files themselves out of the output
  fingerprint: the same values give the same output, whichever file supplied them.
- **Make implied values visible** in the help, e.g. "Default: the instrument's (Astral 380-980), else 350-1200".
- **Without an instrument,** use the default instrument and say so in one log line, including what it implies.

### 2.4 Decide with the owner, don't assume

- **Implied vs opt-in method windows.** Windows implied by the instrument silently lose identifications when the actual
  method is wider (all three reviewers flagged this). DIALibGen's owner chose "implied" and accepted it with a log line and
  easy override. Ask; don't pick.
- **Charges and similar values** that change the output size (timsTOF 2-4 vs 1-4: 433 k → 381 k precursors). The owner
  chose 1-4.
- **Old recipes without a new key:** reproduce the old output, or apply the new rule? The DIALibGen owner chose the new
  rule.

## 3. Command-line options: basic vs. advanced

### 3.1 Budget and split

- `--help` shows only what a typical user needs. Target about 15 options per mode and at most about 25 overall.
  DIALibGen: generate 14, refine 14, tune 8, convert 4, prepare 2.
- Per-mode help: `--help -mode <mode>` prints only that mode's options.
- **Use the framework's native split.** In TOPP, register with `advanced = true`: hidden from `--help`, shown in
  `--helphelp`, still in `-write_ini`. Don't invent a second mechanism.
- Every option needs a real description: what it does, the unit, the default (and what "auto" resolves to). DIALibGen had
  23 of 24 `generation:*` options whose help was just the key name.
- Dev, benchmark and test hooks don't belong on the user surface. Make them advanced, or env vars (`ODIA_*`) when they
  only exist for ablations. Keep options needed to replay a result as advanced CLI options, not env vars, because recipes
  record options.

### 3.2 Defaults that should be automatic

- **Threads:** all physical cores, respecting cgroup/affinity, unless the CLI or INI sets a value; an INI value of 1 is
  respected. TOPP's default of 1 made every default run single-threaded.
- **Choices the data can decide** (e.g. which model heads to tune): `auto` chooses from what the input supports. Never
  default to a choice that fails on common input. DIALibGen's `-tune_heads both` failed on every run without ion
  mobility.
- **Infer the mode** from the inputs when `-mode` is absent: a library input without IDs means convert.
- **Infer the output format** from the extension.
- **Compute device:** `auto` picks the fastest working route and logs one line saying what and why. An explicit device
  forces it, and `require_gpu` makes any fallback an error.

### 3.3 Validation of combinations

- Inert combinations (an option that only matters with another one that isn't set) warn in the minor release and become
  errors in the next.
- An unknown option prints one line with the closest name and exits non-zero. Don't print the whole help, because the
  error scrolls away:
  `Unknown option '-instrument' (did you mean -generation:instrument?)`.

### 3.4 Compatibility (mandatory)

- **Removed or renamed options stay registered** as hidden advanced aliases: "Deprecated (0.12, removed in 0.13): use
  -new_name". An unknown INI parameter is fatal in TOPP (exit 6), so without aliases every old INI breaks.
- **Precedence by presence:** new name > old name > env var > default. An old and a new name with different values is an
  error, on the CLI and in INIs.
- **`-write_ini` writes only the new names** and moves an old INI's values onto them.
- **Old INIs that carry old default values** (e.g. `tune_heads both`): handle them through a changed-defaults table keyed
  on the INI's recorded version. Apply values that came from the INI as given, and log one line naming the old defaults
  they carry.
- **JSON/recipe keys:** never rename. Add keys, and record a schema version if semantics change.
- **Archived benchmark scripts** stay as they are and must keep working through the aliases.
- **Update the GUI in the same change:** defaults, saved-settings migration, `auto` values. Skip deprecated items by a
  name list, not by the advanced flag, which would empty the forms.

## 4. Messages and diagnostics

- **One or two lines:** what went wrong, the likely cause, the fix. No development history, measurements or URLs in
  runtime text; those go in the docs.
- **Every warning starts with `WARNING:`.** Repeat a warning once per run, not once per model or batch.
- **Diagnostics behind a switch:** reuse TOPP's advanced `-debug 1` (or an env var such as `ODIA_VERBOSE=1`). It shows
  device probes, loader errors and search paths, shape and placement summaries, and the files used.
- **Keep greppable lines stable** (`[MS2] provider CUDA device 0 sessions 4`, `WARNING:`), or update every consumer in
  the same commit: tests, benchmark harnesses, CI checks.
- **Ship a short before/after table** of the changed messages (`docs/messages-review.md`).

## 5. Pitfalls we actually hit

- An instrument file's content hash changed on Windows because git checked it out with CRLF. Force LF for the data files
  (`.gitattributes`: `data/instruments/*.yaml text eol=lf`).
- A Windows test wrote a BOM through the ANSI code page, so the fixture must be written as UTF-8 explicitly.
- Name matching that is too loose maps "Exploris" onto an "Exploris 480" file. Normalise case, spaces, `-`, `_`, the
  vendor prefix and the trailing model number, and nothing more.
- Enum-like options compared as raw strings in several places (`"both"`) broke when `auto` was added. Grep every
  comparison.
- The GUI sent the model slot (Lumos) instead of the instrument the user named (Astral). Record both
  (`instrument` and `model_instrument`).
- A recipe was incomplete: it missed one input path, tune settings and the convert fingerprint. Check that `-write_config`
  replays the run exactly.
- The docs claimed behaviour the code no longer had. Verify every doc statement you touch against the binary.
- TOPP owns the `-threads` help line and still prints "(default: '1')". State the real default in the tool description
  line.

## 6. Tests and checks (minimum)

- **Identity:** the old full command (all options explicit) and the new short command give the same output, byte-identical
  except for intended recipe/provenance keys. Also check each instrument file's output against the previous release.
- **Instrument files:** loading the shipped files, the user directory override, the single-file option, precedence per
  value, and every validation error (unknown key, duplicate key, wrong type, range).
- **Defaults:** the threads default with and without an INI value, the `auto` choices on inputs with and without the
  relevant data, mode inference, output-format inference.
- **Compatibility:** each deprecated name on the CLI and in an INI, the old+new conflict error, an INI from the previous
  release in every mode, `-write_ini` round trip.
- **Help:** the line count per mode (pin it), the unknown-option message and exit code, and that `--helphelp` lists the
  advanced and deprecated options and the instrument directory.
- **CI on every platform** (Linux x64/arm64, macOS arm64/x64, Windows), plus the GUI tests.

## 7. Checklist for the PR

- [ ] Inventory TSV and help dumps (before/after) committed or attached.
- [ ] `--help` per mode at or under budget; every option described; dev hooks advanced or env vars.
- [ ] Instrument YAML files with sourced values; `default.yaml` holds all generic defaults; user directory and
      single-file option work; strict validation.
- [ ] One log line each for the files used, the implied values and device/auto choices; details behind `-debug 1`.
- [ ] Recipe/provenance records each value's source and the file hashes; the fingerprint is unchanged for unchanged values.
- [ ] Deprecated aliases, conflict errors, old-INI handling, GUI migration.
- [ ] Messages: short, `WARNING:` prefix, greppable lines stable; before/after table.
- [ ] Identity checks and the tests in section 6; CI green everywhere.
- [ ] CHANGELOG: every behaviour change a user must know about, in plain words; docs updated (usage, parameters, adding
      an instrument).
- [ ] Open decisions listed for the owner; no merge, tag or release without an explicit yes.

## 8. Prompt template for an agent

```text
Review and then (after my decisions) implement the command-line and instrument-defaults cleanup of <TOOL> following
<abs path>/HANDOFF-instrument-defaults-and-cli.md. Phase 1 (no code changes): inventory + adversarial findings checked
in code + proposal (target --help per mode written out, basic/advanced split, new defaults with sources, compatibility
plan, before/after commands) + cross-model review (Codex; Kimi/Vibe if available). Write it to <path>/CLI-REVIEW.md and
list my open decisions with a recommendation each. Phase 2, only after I answer: implement on branch <branch>, one
commit per change, tests and identity checks per section 6, Codex review of the diff, CI on all platforms. Never merge,
tag or release without my explicit yes.
```

=== PROPOSAL (docs/cli-review/CLI-REVIEW.md) ===
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

=== INVENTORY ===
option	type	default	shown	help_chars	uses_docs	uses_scripts	uses_tests	uses_CI
in	file		basic	58	26	37	5	5
lib	file		basic	80	24	45	3	4
out	file		basic	90	31	50	4	4
out_tsv	file		basic	48	2	1	1	3
out_stats	file		basic	56	7	22	0	0
ids	file		basic	50	12	19	0	0
top_fragments	n	6	basic	81	12	26	3	4
rt_mode	mode	auto	basic	291	32	35	0	0
rt_window	s	0.0	basic	255	12	26	3	4
mz_tol_ms2	ppm	0.0	basic	144	14	25	3	4
mz_tol_ms1	ppm	0.0	basic	134	18	28	3	4
scheme	name	A	basic	322	14	9	1	4
out_mzml	file		basic	253	7	0	0	0
out_axes	file		advanced	75	1	0	0	0
out_calibration	file		advanced	49	5	3	0	0
run_name	name		advanced	72	0	0	0	0
targets_only	flag	false	advanced	20	0	0	0	0
no_ms1	flag	false	advanced	31	3	0	0	0
ms1_isotopes	n	2	advanced	105	6	4	1	0
ms1_axis	axis	scans	advanced	159	2	1	0	0
rt_anchors	file		advanced	84	5	2	0	0
rt_trafo	file		advanced	88	2	0	0	0
rt_trafo_invert	flag	false	advanced	76	0	0	0	0
mz_accuracy_ms2	k	0.0	advanced	205	6	0	0	0
mz_accuracy_ms1	k	4.0	advanced	99	3	0	0	0
mz_calibration	mode	auto	advanced	188	6	12	2	0
mz_offset_ms2	ppm	0.0	advanced	92	4	4	0	0
mz_offset_ms1	ppm	0.0	advanced	87	2	7	0	0
encoding	enc	float32	advanced	157	2	0	0	0
expand_rt	flag	false	advanced	59	1	0	0	0
skip_empty	flag	false	advanced	38	0	0	0	0
write_parts	n	32	advanced	188	2	0	0	0
unit_precursors	n	256	advanced	62	0	1	2	0
buffer_mb	MiB	512	advanced	79	1	0	0	0
calibration:mz_model	model	linear	advanced	172	4	2	0	0
calibration:panel	n	0	advanced	84	0	0	0	0
calibration:search_ppm	ppm	50.0	advanced	85	0	0	0	0
calibration:lowess_span	f	0.15	advanced	76	0	0	0	0
ini	file		basic	27	0	0	0	0
log	file		advanced	46	0	0	0	0
debug	n	0	advanced	35	3	0	0	1
threads	n	1	basic	103	30	51	4	4
write_ini	file		basic	37	4	0	0	0
write_ctd	out_dir		advanced	73	0	0	0	0
write_nested_cwl	out_dir		advanced	74	0	0	0	0
write_cwl	out_dir		advanced	114	0	0	0	0
write_nested_json	out_dir		advanced	37	0	0	0	0
write_json	out_dir		advanced	75	0	0	0	0
no_progress	flag	false	advanced	41	0	0	0	0
force	flag	false	advanced	30	1	1	0	1
test	flag	false	advanced	52	3	1	0	1

=== current --helphelp ===
Options (mandatory options marked with '*'):
  -in <file>*                    DIA run: centroided mzML, mzPeak (.mzpeak) or Thermo .raw.
  -lib <file>                    Spectral library: DIA-NN/DIALibGen .parquet or .tsv, OpenSWATH .tsv/.pqp/.TraML.
  -out <file>                    Traces (Parquet), one row per trace. The RT axes go to <out stem>.axes.parquet next to it.
  -out_tsv <file>                Traces as a long TSV, one row per point (large).
  -out_stats <file>              Run statistics (JSON): calibration, stage times, memory.
  -ids <file>                    Extract only these precursors (ids, one per line).
  -top_fragments <n>             Most intense library fragments per precursor (0 = all). (default: '6') (min: '0')
  -rt_mode <mode>                How library RT maps to the run: auto (fitted from the run itself; any library RT scale), minutes (library RT is run minutes, e.g. a library tuned to this run), seconds (run seconds). -rt_anchors or -rt_trafo select a given map instead. (default: 'auto') (valid: 'auto', 'minutes', 'seconds')
  -rt_window <s>                 Extract +- this many seconds around each precursor's mapped RT. 0 (default): measured from the run, along the gradient: 7 x the RT scatter of the calibration anchors about the RT map + 2 x their peak width (FWHM). Negative: the whole run. (default: '0.0')
  -mz_tol_ms2 <ppm>              Fragment tolerance, +- ppm. 0 (default): measured from the run (-mz_accuracy_ms2 x the fragment mass-error width). (default: '0.0') (min: '0.0')
  -mz_tol_ms1 <ppm>              MS1 tolerance, +- ppm. 0 (default): measured from the run (-mz_accuracy_ms1 x the MS1 mass-error width). (default: '0.0') (min: '0.0')
  -scheme <name>                 A (default): DIA-NN's extraction rules (max of the centroids in the window) with the measured mass correction. B: OpenSWATH's rules (sum of the centroids, MS1 isotopes at unit spacing, every isolation window containing the precursor, no mass correction); needs -mz_tol_ms2 and -mz_tol_ms1. (default: 'A') (valid: 'A', 'B')
  -out_mzml <file>               Also write the run as a centroided mzML, one spectrum per isolation window (diaPASEF frames split by window, scans collapsed), for tools that do not read mzPeak. Without -out and -out_tsv only this is done and -lib is not needed. (valid formats: 'mzML')
  -out_axes <file>               RT axes (Parquet) to join with the traces; default <out stem>.axes.parquet.
  -out_calibration <file>        Calibration anchors (TSV), usable as -rt_anchors.
  -run_name <name>               Run name in the output (default: the input file name without extension).
  -targets_only                  Skip library decoys.
  -no_ms1                        No MS1 traces (fragments only).
  -ms1_isotopes <n>              MS1 isotope peaks after the monoisotopic one (0: monoisotopic only). (default: '2') (min: '0' max: '200')
  -ms1_axis <axis>               MS1 traces on the MS1 scans (their own times, in the axes file) or interpolated onto the MS2 cycle axis (cycles). (default: 'scans') (valid: 'scans', 'cycles')
  -rt_anchors <file>             Use this RT map instead: two columns, library RT and run RT (s); fitted with LOWESS.
  -rt_trafo <file>               Use this RT map instead: trafoXML, library RT -> run RT (s). (valid formats: 'trafoXML')
  -rt_trafo_invert               The trafoXML maps run RT -> library RT (as OpenSwathRTNormalizer writes it).
  -mz_accuracy_ms2 <k>           Measured fragment tolerance = k x the robust width of the fragment mass errors. 0 (default): by instrument class from the run's metadata (Orbitrap/Astral 9, TOF 4, unknown 6). (default: '0.0') (min: '0.0')
  -mz_accuracy_ms1 <k>           Measured MS1 tolerance = k x the robust width of the MS1 mass errors. (default: '4.0') (min: '0.1')
  -mz_calibration <mode>         Auto: correct the m/z by the offsets measured from the run. off: use -mz_offset_ms2 and -mz_offset_ms1 instead (needs -mz_tol_ms2 and -mz_tol_ms1). (default: 'auto') (valid: 'auto', 'off')
  -mz_offset_ms2 <ppm>           Fragment m/z correction with -mz_calibration off (ppm added to the target). (default: '0.0')
  -mz_offset_ms1 <ppm>           MS1 m/z correction with -mz_calibration off (ppm added to the target). (default: '0.0')
  -encoding <enc>                Trace intensities: float32, or log8 (1 byte per point, log-spaced over 6 decades below each trace's maximum). (default: 'float32') (valid: 'float32', 'log8')
  -expand_rt                     Also write each trace's RT values in its row (larger file).
  -skip_empty                    Do not write traces that are all zero.
  -write_parts <n>               Parquet writers working in parallel on the one -out file (1: a single writer, slower for large libraries). Fixed, so that the output does not depend on -threads. (default: '32') (min: '1')
  -unit_precursors <n>           Precursors per parallel work unit. (default: '256') (min: '1')
  -buffer_mb <MiB>               Memory for extracted traces waiting to be written. (default: '512') (min: '16')

Calibration from the run itself:
  -calibration:mz_model <model>  Mass correction: linear (offset + slope x m/z, kept only where the m/z dependence is material, else constant) or constant. (default: 'linear') (valid: 'constant', 'linear')
  -calibration:panel <n>         Precursors sampled for calibration (0: 5% of the library). (default: '0') (min: '0')
  -calibration:search_ppm <ppm>  First-pass mass search half width, before calibration. (default: '50.0') (min: '0.1')
  -calibration:lowess_span <f>   LOWESS span of the fitted RT map. (default: '0.15') (min: '0.01' max: '1.0')

                                 
Common UTIL options:
  -ini <file>                    Use the given TOPP INI file
  -log <file>                    Name of log file (created only when specified)
  -debug <n>                     Sets the debug level (default: '0')
  -threads <n>                   Sets the number of threads allowed to be used by the TOPP tool (0 = all available cores) (default: '1')
  -write_ini <file>              Writes the default configuration file
  -write_ctd <out_dir>           Writes the common tool description file(s) (Toolname(s).ctd) to <out_dir>
  -write_nested_cwl <out_dir>    Writes the Common Workflow Language file(s) (Toolname(s).cwl) to <out_dir>
  -write_cwl <out_dir>           Writes the Common Workflow Language file(s) (Toolname(s).cwl) to <out_dir>, but enforce a flat parameter hierarchy
  -write_nested_json <out_dir>   Writes the default configuration file
  -write_json <out_dir>          Writes the default configuration file, but compatible to the flat hierarchy
  -no_progress                   Disables progress logging to command line
  -force                         Overrides tool-specific checks
  -test                          Enables the test mode (needed for internal use only)
  --help                         Shows options
  --helphelp                     Shows all options (including advanced)


=== registerOptionsAndFlags_, main_, threadCount_, instrumentClass (src/DIATraceExtractor.cpp) ===
108:std::pair<std::string, double> instrumentClass(std::string name)
109-{
110-  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
111-  const auto has = [&](const char* w) { return name.find(w) != std::string::npos; };
112-  if (has("tims")) { return {"timsTOF", 4.0}; }
113-  if (has("orbitrap") || has("astral") || has("exploris") || has("exactive") || has("lumos") || has("fusion") || has("eclipse") || has("velos") || has("elite") || has("tribrid") || has("ascend")) { return {"Orbitrap", 9.0}; }
114-  if (has("tof")) { return {"TOF", 4.0}; }
115-  return {"unknown", 6.0};
116-}
117-
118-/// The run grid as mzML: every MS1 scan and every window cell in acquisition-time order, centroided, each MS2
119-/// spectrum carrying its window's isolation target and offsets.
120-void writeRunAsMzML(const ODIA::RunGrid& grid, const std::string& source, const std::string& filename)
121-{
122-  struct Item { double rt; std::int32_t lane; std::size_t cell; };
123-  std::vector<Item> items;
124-  for (std::size_t j = 0; j < grid.ms1.rt.size(); ++j) { items.push_back({grid.ms1.rt[j], -1, j}); }
125-  for (std::size_t w = 0; w < grid.windows.size(); ++w)
126-  {
127-    const auto& lane = grid.windows[w];
128-    for (std::size_t c = 0; c < lane.acquired_rt.size(); ++c)
129-    {
130-      if (lane.acquired_rt[c] == lane.acquired_rt[c] && lane.peaks.cellEnd(c) > lane.peaks.cellBegin(c)) { items.push_back({lane.acquired_rt[c], static_cast<std::int32_t>(w), c}); }
131-    }
132-  }
133-  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rt < b.rt; });
--
437:      const auto cls = instrumentClass(grid.instrument);
438-      if (sigma_ms2 <= 0.0) { sigma_ms2 = cls.second; }
439-      OPENMS_LOG_INFO << "Instrument: " << (grid.instrument.empty() ? std::string("(none in the run's metadata)") : "'" + grid.instrument + "'")
440-                      << " -> " << cls.first << "; tolerances " << sigma_ms2 << " sigma (MS2), " << getDoubleOption_("mz_accuracy_ms1") << " sigma (MS1)"
441-                      << (getDoubleOption_("mz_accuracy_ms2") > 0 ? " [MS2 given]" : "") << std::endl;
442-      if (grid.instrument.empty() || cls.first == "unknown")
443-      {
444-        OPENMS_LOG_WARN << "Instrument class unknown: MS2 tolerance " << sigma_ms2 << " sigma; set -mz_accuracy_ms2 (Orbitrap 9, TOF 4)." << std::endl;
445-      }
446-    }
447-
448-    // --- calibration (from the run itself) and RT map
449-    std::optional<ODIA::CalibrationResult> cal;
     1	  void registerOptionsAndFlags_() override
     2	  {
     3	    // Basic options first (--help); everything marked advanced (last argument true) appears with --helphelp only.
     4	    // A plain string, not registerInputFile_: for unregistered extensions OpenMS (develop) sniffs the
     5	    // content, takes the mzPeak ZIP container for a single-file .zip and fails; -in is checked below.
     6	    registerStringOption_("in", "<file>", "", "DIA run: centroided mzML, mzPeak (.mzpeak) or Thermo .raw.");
     7	    registerInputFile_("lib", "<file>", "",
     8	                       "Spectral library: DIA-NN/DIALibGen .parquet or .tsv, OpenSWATH .tsv/.pqp/.TraML.", false);
     9	    registerOutputFile_("out", "<file>", "", "Traces (Parquet), one row per trace. The RT axes go to <out stem>.axes.parquet "
    10	                        "next to it.", false);
    11	    registerOutputFile_("out_tsv", "<file>", "", "Traces as a long TSV, one row per point (large).", false);
    12	    registerOutputFile_("out_stats", "<file>", "", "Run statistics (JSON): calibration, stage times, memory.", false);
    13	    registerInputFile_("ids", "<file>", "", "Extract only these precursors (ids, one per line).", false);
    14	    registerIntOption_("top_fragments", "<n>", 6, "Most intense library fragments per precursor (0 = all).", false);
    15	    setMinInt_("top_fragments", 0);
    16	
    17	    registerStringOption_("rt_mode", "<mode>", "auto",
    18	                          "How library RT maps to the run: auto (fitted from the run itself; any library RT scale), "
    19	                          "minutes (library RT is run minutes, e.g. a library tuned to this run), seconds (run seconds). "
    20	                          "-rt_anchors or -rt_trafo select a given map instead.", false);
    21	    setValidStrings_("rt_mode", {"auto", "minutes", "seconds"});
    22	    registerDoubleOption_("rt_window", "<s>", 0.0, "Extract +- this many seconds around each precursor's mapped RT. "
    23	                          "0 (default): measured from the run, along the gradient: 7 x the RT scatter of the calibration "
    24	                          "anchors about the RT map + 2 x their peak width (FWHM). Negative: the whole run.", false);
    25	    registerDoubleOption_("mz_tol_ms2", "<ppm>", 0.0, "Fragment tolerance, +- ppm. 0 (default): measured from the run "
    26	                          "(-mz_accuracy_ms2 x the fragment mass-error width).", false);
    27	    registerDoubleOption_("mz_tol_ms1", "<ppm>", 0.0, "MS1 tolerance, +- ppm. 0 (default): measured from the run "
    28	                          "(-mz_accuracy_ms1 x the MS1 mass-error width).", false);
    29	    setMinFloat_("mz_tol_ms2", 0.0); setMinFloat_("mz_tol_ms1", 0.0);
    30	    registerStringOption_("scheme", "<name>", "A", "A (default): DIA-NN's extraction rules (max of the centroids in the window) "
    31	                          "with the measured mass correction. B: OpenSWATH's rules (sum of the centroids, MS1 isotopes at unit "
    32	                          "spacing, every isolation window containing the precursor, no mass correction); needs -mz_tol_ms2 and "
    33	                          "-mz_tol_ms1.", false);
    34	    setValidStrings_("scheme", {"A", "B"});
    35	    registerOutputFile_("out_mzml", "<file>", "", "Also write the run as a centroided mzML, one spectrum per isolation window "
    36	                        "(diaPASEF frames split by window, scans collapsed), for tools that do not read mzPeak. Without -out "
    37	                        "and -out_tsv only this is done and -lib is not needed.", false);
    38	    setValidFormats_("out_mzml", {"mzML"});
    39	
    40	    // ---- advanced
    41	    registerOutputFile_("out_axes", "<file>", "", "RT axes (Parquet) to join with the traces; default <out stem>.axes.parquet.", false, true);
    42	    registerOutputFile_("out_calibration", "<file>", "", "Calibration anchors (TSV), usable as -rt_anchors.", false, true);
    43	    registerStringOption_("run_name", "<name>", "", "Run name in the output (default: the input file name without extension).", false, true);
    44	    registerFlag_("targets_only", "Skip library decoys.", true);
    45	    registerFlag_("no_ms1", "No MS1 traces (fragments only).", true);
    46	    registerIntOption_("ms1_isotopes", "<n>", 2, "MS1 isotope peaks after the monoisotopic one (0: monoisotopic only).", false, true);
    47	    setMinInt_("ms1_isotopes", 0); setMaxInt_("ms1_isotopes", 200);
    48	    registerStringOption_("ms1_axis", "<axis>", "scans", "MS1 traces on the MS1 scans (their own times, in the axes file) or "
    49	                          "interpolated onto the MS2 cycle axis (cycles).", false, true);
    50	    setValidStrings_("ms1_axis", {"scans", "cycles"});
    51	    registerInputFile_("rt_anchors", "<file>", "", "Use this RT map instead: two columns, library RT and run RT (s); fitted with LOWESS.", false, true);
    52	    registerInputFile_("rt_trafo", "<file>", "", "Use this RT map instead: trafoXML, library RT -> run RT (s).", false, true);
    53	    setValidFormats_("rt_trafo", {"trafoXML"});
    54	    registerFlag_("rt_trafo_invert", "The trafoXML maps run RT -> library RT (as OpenSwathRTNormalizer writes it).", true);
    55	    registerDoubleOption_("mz_accuracy_ms2", "<k>", 0.0, "Measured fragment tolerance = k x the robust width of the fragment mass "
    56	                          "errors. 0 (default): by instrument class from the run's metadata (Orbitrap/Astral 9, TOF 4, unknown 6).", false, true);
    57	    registerDoubleOption_("mz_accuracy_ms1", "<k>", 4.0, "Measured MS1 tolerance = k x the robust width of the MS1 mass errors.", false, true);
    58	    setMinFloat_("mz_accuracy_ms2", 0.0); setMinFloat_("mz_accuracy_ms1", 0.1);
    59	    registerStringOption_("mz_calibration", "<mode>", "auto", "auto: correct the m/z by the offsets measured from the run. "
    60	                          "off: use -mz_offset_ms2 and -mz_offset_ms1 instead (needs -mz_tol_ms2 and -mz_tol_ms1).", false, true);
    61	    setValidStrings_("mz_calibration", {"auto", "off"});
    62	    registerDoubleOption_("mz_offset_ms2", "<ppm>", 0.0, "Fragment m/z correction with -mz_calibration off (ppm added to the target).", false, true);
    63	    registerDoubleOption_("mz_offset_ms1", "<ppm>", 0.0, "MS1 m/z correction with -mz_calibration off (ppm added to the target).", false, true);
    64	    registerStringOption_("encoding", "<enc>", "float32", "Trace intensities: float32, or log8 (1 byte per point, log-spaced "
    65	                          "over 6 decades below each trace's maximum).", false, true);
    66	    setValidStrings_("encoding", {"float32", "log8"});
    67	    registerFlag_("expand_rt", "Also write each trace's RT values in its row (larger file).", true);
    68	    registerFlag_("skip_empty", "Do not write traces that are all zero.", true);
    69	    registerIntOption_("write_parts", "<n>", 32, "Parquet writers working in parallel on the one -out file (1: a single "
    70	                       "writer, slower for large libraries). Fixed, so that the output does not depend on -threads.", false, true);
    71	    setMinInt_("write_parts", 1);
    72	    registerIntOption_("unit_precursors", "<n>", 256, "Precursors per parallel work unit.", false, true);
    73	    setMinInt_("unit_precursors", 1);
    74	    registerIntOption_("buffer_mb", "<MiB>", 512, "Memory for extracted traces waiting to be written.", false, true);
    75	    setMinInt_("buffer_mb", 16);
    76	    // TOPPBase::printUsage_ throws for an option whose subsection is not registered: register the subsection.
    77	    registerTOPPSubsection_("calibration", "Calibration from the run itself");
    78	    registerStringOption_("calibration:mz_model", "<model>", "linear", "Mass correction: linear (offset + slope x m/z, kept "
    79	                          "only where the m/z dependence is material, else constant) or constant.", false, true);
    80	    setValidStrings_("calibration:mz_model", {"constant", "linear"});
    81	    registerIntOption_("calibration:panel", "<n>", 0, "Precursors sampled for calibration (0: 5% of the library).", false, true);
    82	    setMinInt_("calibration:panel", 0);
    83	    registerDoubleOption_("calibration:search_ppm", "<ppm>", 50.0, "First-pass mass search half width, before calibration.", false, true);
    84	    setMinFloat_("calibration:search_ppm", 0.1);
    85	    registerDoubleOption_("calibration:lowess_span", "<f>", 0.15, "LOWESS span of the fitted RT map.", false, true);
    86	    setMinFloat_("calibration:lowess_span", 0.01); setMaxFloat_("calibration:lowess_span", 1.0);
    87	  }
    88	
    89	  ExitCodes main_(int argc, const char** argv) override
    90	  {
    91	    // TOPPBase's -threads defaults to 1; this tool uses all cores unless -threads (or an INI file) is given.
    92	    for (int i = 1; i < argc; ++i)
    93	    {
    94	      const std::string a = argv[i];
    95	      if (a == "-threads" || a == "--threads" || a == "-ini" || a == "--ini") { threads_given_ = true; }
    96	    }
    97	    // TOPPBase reports OpenMS exceptions; report everything else too instead of aborting.
    98	    try { return run_(argc, argv); }
    99	    catch (const OpenMS::Exception::BaseException&) { throw; }
   100	    catch (const std::exception& e)
   101	    {
   102	      OPENMS_LOG_ERROR << "Error: " << e.what() << std::endl;
   103	      return INTERNAL_ERROR;
   104	    }
   105	  }
   106	
   107	  bool threads_given_ = false;
   108	
   109	  int threadCount_()
   110	  {
   111	    if (threads_given_ && getIntOption_("threads") > 0) { return getIntOption_("threads"); }
   112	    // default: the physical cores the process may use (SMT siblings do not help the extraction); 0: every hardware
   113	    // thread, as TOPP documents
   114	    return threads_given_ ? std::max(1, omp_get_num_procs()) : ODIA::platform::physicalCores();
   115	  }
   116	
   117	  ExitCodes run_(int, const char**)
