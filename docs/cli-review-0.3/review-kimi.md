• # Adversarial CLI review — DIATraceExtractor 0.3.0

  Scope note: only the pasted registration and the shown head of `run_()` were available; anything depending on code after the "--- calibration ---" cut is marked **[inferred]**. Everything checked against the pasted help text or registration code is **[verified]**.

  ## High

  **1. `-rt_mode` has no valid-strings restriction.** — `-rt_mode` is registered with `registerStringOption_` but no `setValidStrings_`, unlike `scheme`, `ms1_axis`, `mz_calibration`, `encoding` [verified: absent from registration]. A typo is accepted at parse time:
  ```
  DIATraceExtractor -in run.mzML -lib lib.parquet -out o.parquet -rt_mode mintes
  ```
  `mode != "auto"`, so no RT map is fitted and no error is raised in the shown code; the library RT is then (presumably) taken as run time under a garbage mode [inferred for the fallback, verified that nothing rejects it]. A wrong RT mapping is the single most result-destroying mistake and the tool's own README says it only *warns* when RTs land on a small part of the run. Fix: `setValidStrings_("rt_mode", {"auto","minutes","seconds"})`.

  **2. `-mz_calibration off` without fixed tolerances produces a silently empty extraction.** — Help says `off` "needs -mz_tol_ms2 and -mz_tol_ms1", and `scheme B` gets exactly this validation (`scheme_b && (mz_tol <= 0 || ...)`) [verified]. But with `scheme A -mz_calibration off`, `mass_auto` is false, so nothing measures the tolerance; `-mz_tol_ms2` still defaults to 0, which its own help defines as "measured from the run" — a measurement that now never happens [inferred for downstream effect]:
  ```
  DIATraceExtractor -in run.mzML -lib lib.parquet -mz_calibration off -mz_offset_ms2 3 -out o.parquet
  ```
  accepts 0 tolerances → MS2 traces extracted with ±0 ppm → all zero, no error. Fix: replicate the scheme-B check: `off` requires positive `-mz_tol_ms2` (and `-mz_tol_ms1` unless `-no_ms1`).

  ## Medium

  **3. `-threads` help says `(default: '1')`; behavior is all physical cores.** The code logs `"(the physical cores; -threads sets the number)"` when the flag wasn't given [verified: `threads_given_` branch in `run_()`], and README says "all physical cores". TOPP owns the flag so the help line can't be edited directly, but a user reading `--help` believes they get 1 thread. Fix: TOPP can't change it; at minimum state the real default in README's option table (it is buried in prose) and consider intercepting `-threads 1` given explicitly vs. absent more loudly. (Partially verified — the help string is TOPP's.)

  **4. `-ms1_isotopes -1` is an undocumented magic value.** Registration comment says `-1: 0.2.14's "no MS1"` [verified], but the help only says `(0: monoisotopic only)` while `--helphelp` shows `(min: '-1' max: '200')`. Users see -1 accepted with no explanation of what it does, and it duplicates `-no_ms1`. Fix: reject -1 at run time with a pointer to `-no_ms1` (keep translating it only inside `resolveCompatibility_`).

  **5. `-out_axes` is silently ignored unless `-out` is given.** `if (out_axes.empty() && !out.empty())` derives it [verified]; there is no check that a user-supplied `-out_axes` actually takes effect. With TSV-only output it can't work (TSV is long format), but nothing says so:
  ```
  DIATraceExtractor -in run.mzML -lib lib.parquet -out_tsv t.tsv -out_axes a.parquet
  ```
  silently writes no axes file. Fix: error ("-out_axes needs -out") or write the axes anyway. Same pattern [inferred]: `-expand_rt`, `-encoding` with `-out_tsv` only.

  **6. `-out` / `-out_tsv` / `-out_stats` have no format validation.** `registerOutputFile_` without `setValidFormats_` [verified], so `-out traces.tsv` (help says Parquet), `-out_stats stats.tsv`, `-out_tsv x.parquet` all parse; whether the writer errors depends on file content, not the name [inferred]. `-out_mzml` does it right — apply `setValidFormats_` to the rest.

  **7. `-rt_window 0` "measured from the run" cannot be measured under `-rt_mode minutes`/`seconds`.** `rt_auto = mode == "auto" && rt_needed` [verified], so with an explicit RT map no anchors are fitted, yet the default window's whole selling point is "7 × RT scatter of the calibration anchors". What the window falls back to is below the code cut [inferred] — if it silently uses some constant while `--help` says "measured from the run", that is misleading. Fix: define and document the fallback (e.g. error, or instrument-file `rt_window_sigma/fwhm` defaults), and say so in the help.

  **8. `--helphelp` prints the `Calibration from the run itself:` header three times, two with no options under them.** [verified in pasted output.] TOPPBase emits the subsection before each advanced option in some builds; the empty repeats are pure clutter in the "all options" view. Fix: register the subsection once (or whatever the TOPPBase idiom is to avoid repetition).

  **9. `-write_parts` (default 32) and `-unit_precursors` (256) are knobs the tool could set itself.** The code already caps ingest threads at 32 "memory-bandwidth bound" and fixes part sizes so output doesn't depend on `-threads` [verified] — the same reasoning applies to the writer count. Asking users to tune parquet writer parallelism is dev-level detail; default 32 writers also costs memory on small runs. Fix: derive from core count / library size, keep the option advanced.

  **10. `-out_mzml` + library loads the whole library for nothing.** Validation passes with `-lib x -out_mzml y -out` absent [verified: the early return happens after `LibraryReader` is constructed and the panel future may be launched]. The help says "Without -out and -out_tsv only this is done" but doesn't say the library is still parsed. Fix: check the export-only case before opening the library.

  ## Low

  **11. Redundant `(default: ...)` noise** on every auto-resolving option: the prose already says "0 (default):" and TOPP appends `(default: '0.0')` [verified in both help dumps] — six options read "0 (default): measured... (default: '0.0')". Cosmetic; drop the in-text "(default)" since TOPP adds it.

  **12. `-mz_accuracy_ms2/ms1` silently inert when `-mz_tol_ms2/ms1` are fixed** [inferred — `multiple()` computes them but extraction uses the fixed tol]. `-mz_tol_ms2 6 -mz_accuracy_ms2 12` accepts both; one is ignored with no warning. Fix: warn or reject the combination.

  **13. `-mz_calibration` is silently ignored under `-scheme B`** [verified that `mass_auto` is forced false for scheme B; no error for `-scheme B -mz_calibration auto`]. Fix: reject, or document in the `-mz_calibration` help.

  **14. `-no_ms1` makes `-ms1_isotopes`, `-mz_tol_ms1`, `-ms1_axis` inert** [inferred — scheme-B check already special-cases `ms1_enabled`, showing the pattern exists]. Fix: one-line warning listing ignored options.

  **15. `-instrument` + `-instrument_file` conflict not checked in export-only mode.** The check sits after the `-out_mzml`-only early return [verified: ordering in `run_()`]:
  ```
  DIATraceExtractor -in run.mzpeak -out_mzml x.mzML -instrument Astral -instrument_file my.yaml
  ```
  silently ignores both. Fix: move the conflict check above the early return.

  **16. `-out_calibration` (advanced) is the natural companion of `-rt_anchors`** — the reuse-calibration-across-runs workflow is hidden from `--help`, while `-rt_anchors`/`-rt_trafo` are also advanced. Fine that both are advanced, but the help texts never cross-reference them (`-rt_anchors` doesn't mention `-out_calibration` produces its input). Fix: cross-link the two help strings.

  **17. `--help` text points into `--helphelp`-only options.** `-mz_tol_ms2` help references `-mz_accuracy_ms2`; `-instrument` help says "see -instrument" [verified] — options a first-time user cannot see or find. Fix: make the main help self-contained ("the instrument's multiples, overridable with an advanced option").

  **18. `-ids` doesn't say what an id is.** "ids, one per line" [verified] — modified peptide? stripped sequence? charge-separated? First-time user has to read the README source or guess. Fix: one example line in the help.

  **19. `-scheme B` help overstates: "needs -mz_tol_ms2 and -mz_tol_ms1"** but the code only requires `-mz_tol_ms1` when MS1 is enabled [verified: `ms1_enabled &&`]. Trivial wording fix: "(and -mz_tol_ms1 unless -no_ms1)".

  **20. `-run_name` help is vague** — "Run name in the output" [verified]; output where (footer? stats? axes?). Fix: name the place.

  ## First five changes

  1. `setValidStrings_` on `-rt_mode` (finding 1) — closes the worst silent-failure hole.
  2. Add the missing tolerance validation for `-mz_calibration off` (finding 2) — mirrors the scheme-B check already written.
  3. Reject or honor inert combinations: `-out_axes` without `-out`, `-mz_accuracy_*` with fixed tols, `-mz_calibration` under scheme B, `-instrument`/`-instrument_file` in export mode (findings 5, 12, 13, 15) — one "silently ignored options" sweep.
  4. Define and document what `-rt_window 0` falls back to when the RT map is user-supplied (finding 7) — either error out or use documented instrument-file values; make the help say which.
  5. Clean the help surfaces: fix the triple calibration header, drop in-text `(default:)` duplicates, cross-link `-out_calibration`/`-rt_anchors`, and document the real `-threads` default (findings 3, 8, 11, 16, 17).

