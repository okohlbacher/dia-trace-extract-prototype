The proposal identifies real problems, but I would revise it before implementation. The main risks are changed extraction results disguised as defaults cleanup, incomplete compatibility semantics, and a second argument parser. This review uses only the supplied material; the claimed binary checks and external sources are not independently verified.

References below use **Proposal** for `docs/cli-review/CLI-REVIEW.md`, **Guideline** for `docs/HANDOFF-instrument-defaults-and-cli.md`, and the supplied line numbers for `src/DIATraceExtractor.cpp`.

### 1. Compatibility is not specified well enough to promise old commands work

**High — accepting removed options at their old defaults does not preserve their effects.**  
Reference: Proposal → Compatibility; Guideline §§0, 3.4.

A removed parameter can equal its historical default and still influence results. Accepting it with a warning is safe only if the new implementation reproduces that behavior.

Concrete case: an old INI carries a removed `calibration:*` threshold at its old default. If the replacement algorithm uses a different threshold, accepting and ignoring the key changes calibration and extraction. The proposal needs an exhaustive mapping table: old name, type, historical default/version, replacement, and whether behavior is preserved or rejected. “About 30 options” and “21 thresholds” are insufficient.

**High — mapping several old options to `-scheme B` is not generally equivalent.**  
Reference: Proposal → Compatibility; current `--helphelp` → `-scheme`; registration lines 30–34.

Scheme B also changes isolation-window assignment and requires fixed tolerances. The proposed translation:

```text
-aggregation sum -ms1_isotope_spacing unit -mz_calibration off
```

does not establish the old `-window_assignment` value. Mapping it to B could change which windows contribute. Translating `-aggregation sum` alone to B would change several additional behaviors.

Translate only complete combinations with demonstrated equivalence. Reject unsupported combinations with a specific explanation.

**High — some “aliases” require value migration, not name registration.**  
Reference: Proposal → Findings 1 and Compatibility; registration lines 21, 46–47.

Concrete failures:

- `-ms1_isotopes -1` fails the current minimum of zero unless translated before validation.
- `-rt_mode identity` fails the current enum unless accepted before validation.
- `-rt_mode linear -rt_intercept … -rt_slope …` has no proposed equivalent.
- An old INI containing `rt_mode=anchors` but no anchors file needs an explicit error policy.

For shared names with changed value domains, hidden aliases alone cannot solve compatibility.

**High — explicit presence, conflicts, and version handling are missing.**  
Reference: Proposal → Compatibility; Guideline §3.4.

Examples:

- `-mz_tol 5 -mz_tol_ms2 0`: the new value is explicitly present even though it equals today’s default; this must conflict.
- An INI contains the old name while the CLI supplies the new name: define the interaction between alias conflicts and ordinary CLI-over-INI precedence.
- An old INI carries historical defaults: preserve them and identify them using its recorded version, as the guideline requires. The proposal supplies no versioned changed-defaults table.

**Medium — the migration window contradicts the release plan.**  
Reference: Proposal → Compatibility and owner decisions 1, 7.

If aliases first ship in **0.3.0**, “removed in 0.3” offers no transition release. If they ship in 0.2.16, that contradicts releasing the cleanup only in 0.3.0. State the first release containing aliases and a later removal release. Deprecation text must name the actual introduction version.

**Fine:** hidden registered aliases, CLI/INI conflict detection, and new-name-only `-write_ini` are the right goals. They need round-trip tests that prove hidden aliases are actually excluded from generated INIs.

### 2. Instrument files introduce a material results change

**High — the recommended fallback changes existing Orbitrap and TOF results.**  
Reference: Proposal → Instrument defaults and decision 3; source lines 108–115.

Today:

- Exploris, Exactive, Lumos, Fusion, Eclipse, Velos, Elite, Tribrid and Ascend match **9**.
- Other metadata containing `tof` matches **4**.

Under the proposal, instruments without a matching file get **6**. For an Exploris run, the measured MS2 half-window shrinks by one third; for another TOF it widens by half. Either can change traces and downstream identifications. Explicit historical `-mz_accuracy_ms2 0` still means automatic, so merely retaining that key does not preserve old results.

A default-level log makes this observable, but does not make it compatible. Preserve existing class fallbacks during the cleanup. Describe them as inherited behavior, with a source citation to the old implementation, rather than newly measured optima. Any scientific change should be separately approved and validated.

**High — matching run strings to files is unspecified.**  
Reference: Proposal → Instrument defaults; Guideline §§2.2, 5.

The current code uses broad substring matching. The proposed names and aliases do not define how strings such as these resolve:

```text
Thermo Scientific Orbitrap Astral
Bruker timsTOF Pro 2
Q Exactive HF
```

Define normalization, accepted vendor prefixes, exact alias matching, and collision errors. Explicitly test ambiguous names after normalization. The guideline itself is awkward here: it permits stripping trailing model numbers while warning about conflating Exploris variants. Preserve model distinctions whenever their defaults differ.

Also specify:

- Missing or unrecognized metadata under `auto`: warning and fallback.
- Misspelled explicit `-instrument Astrl`: error with a suggestion.
- Multiple instrument descriptions in one run: deterministic handling.
- Both `-instrument` and `-instrument_file`: agreement, override, or conflict.

Do not turn metadata strings directly into filesystem paths.

**High — precedence conflicts with registered defaults and automatic sentinels.**  
Reference: Proposal → Instrument defaults; registration lines 55–58; Guideline §2.3.

`mz_accuracy_ms1` is registered with default **4**, and old generated INIs contain it. Distinguish an absent value from that framework default; otherwise an instrument file can never override it.

For MS2, explicit `-mz_accuracy_ms2 0` means **automatic**, not a literal zero multiplier. Specify how that sentinel passes through resolution. A generic “explicit option > file” rule is insufficient.

Similarly, fixed `-mz_tol_ms2 6` makes the measured multiplier irrelevant to extraction. Logs and provenance must distinguish effective values from unused configured values.

**Medium — provenance is a good start but cannot yet support replay.**  
Reference: Proposal → Provenance; Guideline §§2.3, 5.

`given` merges CLI and INI sources. Record the raw metadata string, selected instrument, selection method, resolved values, and actual supplying files. An automatically measured tolerance has both a configured multiplier and a measured width; one source label cannot explain the whole derivation.

Concrete replay failure: a user’s `astral.yaml` changes from 9 to 8. The previous footer contains a hash, but rerunning the command selects 8 unless the resolved settings can be restored.

Specify schema versioning and that file names, hashes, and provenance do **not** affect the scientific output fingerprint when effective values are identical. Define provenance availability for TSV-only output too.

**Medium — yaml-cpp is reasonable, but the dependency and packaging work are understated.**  
Reference: Proposal → Instrument defaults; Guideline §§2.1–2.3.

An established parser is correct if YAML is chosen. Adding yaml-cpp to a patched OpenMS manifest still needs a pinned version, license notice, linkage verification, and installation tests across packages. Do not assume parser adoption automatically rejects duplicate keys; test the validator.

For this prototype, defer elaborate discovery machinery before compromising compatibility or validation. If the full guideline remains mandatory, directory overrides and single-file loading are required scope. A simpler format using an existing parser is an owner-approved deviation, not compliance with the YAML requirement.

### 3. `-instrument` belongs in basic help

**Fine — keep it basic.**  
Reference: Proposal → Target help; Guideline §0.

It is the understandable remedy for missing metadata and directly supports “name the instrument and inputs.” Keep `-instrument_file` advanced.

**Medium — the target help removes essential explanations.**

Concrete examples:

- `-rt_mode` no longer explains how library RT maps to run RT.
- `-scheme B` no longer says it requires fixed tolerances or changes window assignment.
- `-rt_window` no longer says the value is a ± half-window.
- “Others 6” documents the proposed compatibility regression without identifying it as a change.

The target contains **14 tool options**, plus three TOPP options: **17 total**, not “17 … own options + TOPP’s 3.” Pin option counts separately from wrapped line counts.

### 4. Do not approve an unspecified pre-parser in `main()`

**High — scanning dash-prefixed tokens rejects valid values.**  
Reference: Proposal → Messages; registration lines 22–24, 62–63.

Concrete valid cases include:

```text
-rt_window -1
-mz_offset_ms2 -2.5
-mz_offset_ms1 -1e-3
```

Negative offsets can also be legitimate. String and path values beginning with `-` must follow TOPP’s supported conventions rather than newly invented ones.

**High — this duplicates framework grammar and may break framework operations.**

The checker must recognize `--help`, `--helphelp`, `-ini`, every registered common flag, deprecated aliases, and all supported spelling forms. The supplied code also recognizes `--threads` and `--ini` when detecting presence; actual TOPP acceptance needs verification.

Concrete regressions to test:

```text
--helphelp
-write_ini defaults.ini
-write_ctd output-directory
-ini old.ini
```

A command-line scan cannot solve unknown parameters *inside* an INI. A checker that loads the INI itself risks duplicating TOPP’s section, instance, version and precedence handling.

Prefer a framework error-formatting hook. If none exists, first prove that a narrowly scoped checker uses the same registered schema and grammar.

**Medium — the example contradicts compatibility.**  
Reference: Proposal → Messages.

`-mz_tol` cannot simultaneously be a working deprecated alias and the unknown-option example. Use an actual typo, such as `-mz_tlo_ms2`.

### 5. Moving development detail behind `-debug 1` is appropriate

**Fine, with conditions.**  
Reference: Proposal → Messages and decision 6; Guideline §4.

Keep calibration success/failure, fallback reasons, selected instrument, effective tolerances, RT-window rule and output destinations visible by default. Unknown metadata and failed calibration are user-relevant events.

**Medium — preserve consumers and structured information.**

Adding `-debug 1` to benchmark scripts works only if the original greppable lines remain stable. Otherwise update their parsers and fixtures too. Keep useful detailed measurements in `-out_stats` independently of debug verbosity.

The required `docs/messages-review.md` before/after table is missing. Also audit OpenMS-originated warnings separately: prefixing the tool’s own warnings does not guarantee every warning satisfies the guideline.

### 6. Other gaps against the guideline

- **Medium — incomplete identity evidence.** Proposal → Tests compares instrument files with V0.2.15 but does not explicitly compare migrated V0.2.14 workflows with V0.2.14. Test legacy combinations, unsupported values, absent metadata, and every existing instrument class.
- **Medium — broken before/after claim.** Proposal → Before/after drops explicit `-threads 64`. A machine allowed eight physical cores will use eight, so the examples are not equivalent configurations.
- **Medium — threads “fine as is” is overstated.** Source lines 91–95 mark *any* `-ini` as `threads_given_`. An INI omitting `threads` therefore appears to select TOPP’s default 1. Test absence separately from an explicit INI value of 1.
- **Medium — schema, discovery and packaging are incomplete.** Specify required keys, finite numeric ranges, replacement versus merge semantics for user files, missing/malformed-file behavior, installed data discovery, and package inclusion. `--helphelp` must expose the directory behavior.
- **Medium — validation promises exceed the evidence.** Add explicit tests for inert combinations involving instrument multipliers and fixed tolerances; the guideline calls for warnings during the transition.
- **Low — remaining deliverables are absent.** Add the changelog, adding-an-instrument documentation, message table, and any applicable GUI migration. If no GUI or recipe interface exists, mark those requirements inapplicable explicitly.

### Recommendations for the seven owner decisions

1. **Compatibility aliases:** **Yes, with a complete migration table and a real transition release.** Do not accept-and-ignore old defaults unless behavior is preserved.
2. **Instrument files, `-instrument`, yaml-cpp:** **Yes in principle; revise before approval.** Keep `-instrument` basic. Resolve matching, sentinels, packaging and provenance; justify the dependency cost.
3. **Which instruments get files:** **Reject the recommended generic-6 fallback.** Add sourced Astral/timsTOF overrides while preserving existing Orbitrap-9 and TOF-4 fallback behavior.
4. **RT-window rule in `default.yaml`:** **Yes.** Preserve the current formula and units, resolve each coefficient independently, and log the effective rule.
5. **Unknown-option pre-parser:** **No as proposed.** Approve the concise error goal; prefer framework integration and require grammar tests before any pre-parser.
6. **Diagnostics behind `-debug 1`:** **Yes.** Retain actionable summaries and warnings, structured statistics, and working benchmark consumers.
7. **Release plan:** **Revise.** A separately validated 0.2.16 is reasonable, but aliases must ship before their removal version. Keep this cleanup behavior-preserving and treat any new scientific defaults as an explicit release decision.
