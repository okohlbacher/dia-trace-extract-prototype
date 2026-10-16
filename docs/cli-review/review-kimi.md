
  Reviewed against `docs/HANDOFF-instrument-defaults-and-cli.md`. Overall the proposal is faithful to the guideline's shape (inventory first, aliases, provenance, per-value precedence), but there are real holes. Findings below, by your six areas, then per-decision recommendations.

  ---

  ## 1. Compatibility plan (proposal §Compatibility; guideline §3.4)

  **High — the "until 0.3" timeline is internally inconsistent.** Decision 7 ships the cleanup *as* 0.3.0, but the alias block says "for one minor release, **until 0.3**" and the deprecation strings read "Deprecated (0.2.15, removed in 0.3)". The aliases are *introduced* in 0.3.0 — if they're removed in 0.3, they exist for zero releases and every old command line/INI breaks again the moment the cleanup lands. The label is also backdated: 0.2.15 is when the names changed *without* aliases. Fix: "Deprecated (0.3.0, removed in 0.4.0)".

  **High — value-level mappings can't work through alias registration.** "Removed options with a fixed equivalent map onto it (`-rt_mode identity` -> `seconds`)". Registering `rt_mode` as an alias doesn't help: `identity` is no longer in `setValidStrings_({"auto","minutes","seconds"})` (src/DIATraceExtractor.cpp:21), so TOPP's valid-string check fires *during parse*, before `main_` runs. Concrete failing case: the old command `-rt_mode identity` does not get the promised translation — it dies with an invalid-value error. Same for `-rt_mode linear`/`anchors`/`trafo` in old INIs. You must either keep the old strings registered as deprecated values and translate in `main_`, or translate argv before parse. The proposal specifies neither.

  **High — `-ms1_isotopes -1` breaks with a confusing error, and the default changed silently.** `ms1_isotopes` still exists (advanced, min 0, default 2, line 46–47). Old usage `-ms1_isotopes -1` (= "no MS1") passes alias registration but fails `setMinInt_(0)` validation — the user gets "Invalid value" instead of the promised WARNING mapping to `-no_ms1`. Worse, this is exactly the guideline's "old INIs that carry old default values" case (§3.4, changed-defaults table keyed on the INI's recorded version): a 0.2.14 INI that doesn't carry the key silently acquires 2 isotope peaks where it previously had the -1 behaviour — output size and content change with no warning. The proposal has no version-keyed table at all; that guideline rule is simply missing.

  **Medium — "accepted at old default" needs the removed options to be *registered*, not just tolerated.** Unknown INI keys are fatal in TOPP (your own finding 1), so all ~25 removed options (`-aggregation`, `-ingest_threads`, 21 `calibration:*` thresholds, …) must be registered as hidden inert options or every old INI still dies. The proposal implies this but never says it; it's the difference between the plan working and not.

  **Medium — `-write_ini` "moves an old INI's values onto the new names" is wrong as stated.** TOPP's `-write_ini` writes *defaults*, it doesn't read an INI. The actual mechanism is: aliases make the old INI parse, then `main_` translates old→new values with the conflict check. The test list has the round trip, but the proposal text describes a feature TOPP doesn't have.

  **Fine:** renamed-option aliases as hidden advanced registrations, the old+new conflict error, and "archived scripts keep working" are all correct per §3.4 — *provided* the value-translation problems above are solved.

  ## 2. Instrument data files (proposal §Instrument defaults; guideline §2)

  **High — silent regression for Exploris/Lumos/QE users.** Today `instrumentClass()` maps any "exploris"/"lumos"/"fusion"/… string to multiple 9 (src/DIATraceExtractor.cpp:113). The proposal (with its own recommendation on decision 3) gives only Astral a file, so every non-Astral Orbitrap run drops from 9 to the generic 6 — extraction tolerances widen, results change, no error. Guideline §2.4 says all three reviewers flagged exactly this class of silent change and says *ask the owner* — decision 3 does ask, good — but the recommendation buries the fact that this breaks the §6 identity check against V0.2.15 for every Exploris user, and unlike the current code (line 442–445 warns on unknown class), the proposal doesn't promise a `WARNING:` when a *recognized but fileless* instrument falls back to 6. If the owner picks "Astral and timsTOF only", a WARNING naming the fallback and how to override is mandatory.

  **Medium — provenance hashes vs. fingerprint.** Guideline §2.3: record file names and content hashes, but *keep the files out of the output fingerprint — the same values give the same output, whichever file supplied them*. The proposal puts `odia.instrument_files` (names **and hashes**) in the Parquet footer. If the output fingerprint is computed over the footer (the natural place), a user-supplied `astral.yaml` with identical values produces a different fingerprint than the shipped one — violating the rule. Specify that the fingerprint excludes the file hashes (values' sources can stay in).

  **Medium — `instrument: auto` in `default.yaml` sidesteps the "no instrument" rule.** Guideline §2.3: "Without an instrument, use the default instrument and say so in one log line, including what it implies." With `auto` as the default there's no named default instrument: a metadata-less mzML silently gets the generic 6. The current code at least warns (lines 442–445). The proposal keeps the warning for that case only implicitly; make it explicit, and define what `default.yaml`'s `instrument:` key is even for.

  **Medium — name matching.** Current matching is substring `find` on raw lowercase (lines 110–115); the proposal must replace it with the §2.2 normalization (case, space, `-`, `_`, vendor prefix, trailing model number — "and nothing more") and state what happens on *no* match vs *ambiguous* match (two files whose normalized names/aliases both match — e.g. a user file `orbitrap.yaml` alongside shipped `astral.yaml` both matching "Orbitrap Astral"). Unspecified. Also per §5: grep every raw-string comparison of the resolved name — line 442's `cls.first == "unknown"` is exactly the pattern that broke in DIALibGen when `auto` was added.

  **Medium — no packaging work item.** Guideline §2.1 requires the files embedded in every package (macOS `.app`, Windows installer, Linux tarball). The tool ships no share data today (nothing in the inventory or `ci/package.sh` path suggests a data-directory search exists), so this invents a data-dir search mechanism plus per-platform packaging on five CI platforms — for two files holding three non-default values. That's the biggest cost of decision 2 and the proposal doesn't budget it. Not a reason to say no, but phase it: shipped dir + `-instrument_file` first, `$DIATRACEEXTRACTOR_INSTRUMENT_DIR` and user-config dir can follow in 0.3.x without breaking anything (they're additive).

  **Low — yaml-cpp cost.** Per guideline, never hand-roll YAML, so yaml-cpp is the sanctioned choice, and the vcpkg-manifest patch + LF `.gitattributes` mirror what was done for boost-json. Costs to acknowledge: the manifest patch must be carried across OpenMS bumps, the licence-bundle notice is required, and CI build time grows on all five platforms. Check whether yaml-cpp is already in the manifest transitively before adding it. Acceptable, just not free.

  **Fine:** per-value precedence, strict validation with file+key in errors, the two log-line format, and `-instrument_file` are all per §2.2/§2.3. Recording the run's raw metadata string alongside the resolved instrument (the DIALibGen "record both" pitfall, §5) would be a cheap addition — `odia.instrument` should hold both what the user named and what the run said.

  ## 3. Is `-instrument` basic-help material?

  Yes, keep it — finding 3 makes the case: converted mzML/mzPeak without an instrument entry is a *common* situation, and without `-instrument` the user's only remedy is knowing about the advanced `-mz_accuracy_ms2`. That's precisely the "defaults that force users to spell out what the tool could infer" failure, inverted. Two caveats: (a) the target `--help` is now 17 tool options against the guideline's "~15 per mode" — close enough, but don't add more, and the implied-values rule (§2.3, your own finding 7) still isn't applied: the target text says "measured (the instrument's multiple x the width)" without showing the multiple. Write "Default: the run's instrument (Astral 9, timsTOF 4, others 6), else 6". (b) Define the override-vs-metadata conflict: `-instrument timsTOF` on a run whose metadata says Astral should print one `WARNING:` that the explicit value wins.

  ## 4. The unknown-option pre-parse in `main()`

  **High — the stated insertion point doesn't exist.** TOPPBase parses argv (`parseCommandLine_`) and *aborts on unknown options before `main_` runs* — that's why the current argv scan in `main_` (lines 92–96) only detects `-threads`/`-ini` presence, it can't intercept parse errors. A pre-parse "in main() before TOPPBase parses" is only possible in the free-standing `main()` that constructs the tool, i.e. a third argv parser that must replicate TOPP's own tokenization. Doable, but the proposal names the wrong hook and underestimates the replication:

  **High — values starting with `-`.** Concrete failing cases for a naive "token starts with `-` and isn't registered → error" scan:
  - `-rt_window -1` (whole-run extraction, documented at line 22–24) → false "Unknown option '-1'".
  - `-mz_offset_ms2 -1.5` with `-mz_calibration off` (no min registered, line 62) → same false positive.
  - `--threads=64` / `-threads=64` `=` forms → token doesn't match a registered name unless `=` is split.
  - `-debug 1`, `-no_progress`, `-force`, `-test`, `--helphelp`, `-ini old.ini`, `-write_ini x` → all TOPP common options must be in the checked set (fine if you scan the *full* registry including TOPP's, fatal if you scan only the tool's 38).

  The scan must consume the value after every known value-taking option before classifying tokens. Any bug here turns legitimate commands into exit-6 with a misleading "did you mean".

  **Medium — the example message contradicts the alias plan.** With `-mz_tol` registered as a hidden alias, TOPP *accepts* it — the pre-parse will never emit `Unknown option '-mz_tol' (did you mean -mz_tol_ms2?)` for it; aliases and the message are mutually exclusive for that name. The pre-parse's real job is typos of *current* names (`-mz_tol_ms1` vs `_ms2`, `-rt_windows`). Use a real typo in the example, or the test will encode a case that can't fire.

  **Fine:** exit code 6, one line, no full help, closest-name suggestion — all per §3.3.

  ## 5. Diagnostics behind `-debug 1`

  **Medium — it collides with "archived benchmark scripts must keep working" (§3.4).** The proposal moves the `Calibration:` and `Scheduler:` lines behind `-debug 1` *and* updates the live benchmark scripts in the same change — but archived scripts can't be updated, and per §4, greppable lines either stay stable or every consumer moves in the same commit. A harness that greps `Calibration:` will not error; it will silently parse nothing. Mitigation that satisfies both rules: keep the greppable prefixes at default level with the new one-line content (`Calibration: 1 247 anchors, LOWESS RT map, offsets -0.8…+1.2 ppm, widths 5.2 ppm` / `Scheduler: …`) and put the 900-character detail, stage counts and buffer-block lists behind `-debug 1`. Guideline §4 wants short one/two-line messages at default anyway — this gives you both.

  **Fine:** `WARNING:` prefix once per run, full paths and search dirs behind `-debug 1`, and the before/after messages table (`docs/messages-review.md`, checklist §7) — the proposal covers all of these. Note the messages-review table is missing from the proposal's test/work list; add it.

  ## 6. Guideline items the proposal ignores

  - **Changed-defaults table keyed on INI version (§3.4)** — absent (see §1 above; `ms1_isotopes` is the concrete victim).
  - **Old valid strings must stay registered (implicit in §3.4)** — absent; `-rt_mode identity` case above.
  - **`--helphelp` content test (§6)** — "lists the advanced and deprecated options and the instrument directory" is in the guideline's minimum test set; the proposal's test list omits it.
  - **Messages before/after table + CHANGELOG + docs verification against the binary (checklist §7)** — not mentioned in the proposal's plan.
  - **UTF-8/BOM fixture pitfall (§5)** — only LF is covered via `.gitattributes`; the Windows BOM pitfall needs the fixture written as UTF-8 explicitly.
  - **Fingerprint exclusion of file hashes (§2.3)** — see §2 above.
  - **Per-mode help / GUI migration (§3.1, §3.4)** — correctly N/A here (one mode, no GUI).
  - **Implied-vs-opt-in windows and charges (§2.4)** — N/A here since `mz_accuracy_*` is a measurement multiplier, not a method window; the *silent-change* spirit of that rule does apply to the Exploris 9→6 drop, which is why decision 3 must be framed as an identity/compatibility question, not just "which files to ship".

  Nothing here kills the proposal; items 1–2 of this list are the ones that would make the compatibility promise false in shipping code.

  ---

  ## Recommendations on the 7 owner decisions

  1. **Aliases: Yes** — but only with the corrections: deprecation string "Deprecated (0.3.0, removed in 0.4.0)"; old `rt_mode` values kept registered and translated in `main_`; `-ms1_isotopes -1` mapped to `-no_ms1` before min-validation; removed options registered as inert hidden options; version-keyed changed-defaults handling for `ms1_isotopes`.
  2. **YAML files + `-instrument` + yaml-cpp: Yes** — phase it: shipped directory + `-instrument_file` in 0.3.0; env-var and user-config search directories can land in 0.3.x (additive, no compatibility surface). Budget the packaging/embed work on all five platforms up front; check for a transitive yaml-cpp first.
  3. **Which instruments: keep today's class multiples** (all Orbitraps 9, TOFs 4) expressed as data — e.g. a class map in `default.yaml` or files for the classes — so 0.3.0 output is identical to 0.2.15 for Exploris/Lumos/QE users. Ship `astral.yaml`/`timsTOF` files only on top of that. If the owner instead wants Astral-only files, accept it only with a `WARNING:` on fallback and an explicit note that identity with 0.2.15 breaks for non-Astral Orbitraps. Do not let "Astral and timsTOF only" be the silent default.
  4. **RT-window rule in `default.yaml`: Yes** — the multiple is library/gradient-dependent, not instrument-dependent; per-value override stays available. Make sure `rt_window_sigma`/`rt_window_fwhm` being data actually means they're logged with their source like every other resolved value.
  5. **One-line unknown-option message: Yes, with fixes** — implement in the free `main()` (not the tool's `main_`, which runs after parse), consume values of value-taking options so `-rt_window -1` and `-mz_offset_ms2 -1.5` don't false-positive, handle `--opt=value`, include TOPP common options in the checked set, and use a real typo in the example message.
  6. **Detail behind `-debug 1`: Yes, but keep the greppable prefixes stable at default level** with the new short content; only the long form moves. Update live scripts in the same commit (as proposed) and accept that archived harnesses keep matching the prefix lines.
  7. **Release plan: Yes** — 0.2.16 now with the pending work, cleanup as 0.3.0 after phase 2 — with the timeline fix from §1: aliases introduced in 0.3.0 and removed no earlier than 0.4.0, otherwise "old command lines keep working until 0.3" promises nothing.

