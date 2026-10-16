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
