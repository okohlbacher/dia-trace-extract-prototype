# DIATraceExtractor 0.3.0: command-line review (2026-10-19)

The released 0.3.0 command line, reviewed adversarially: my own pass with the binary, and Codex and Kimi on the same
brief (`review-brief.txt`: `--help`, `--helphelp`, the option registration and validation code, README usage;
`review-codex.md`, `review-kimi.md`). Vibe returned no output within 40 minutes. Every finding below was checked with
the binary (`help.txt`, `helphelp.txt`, `defaults.ini` are the dumps before the changes). Implemented on branch
`instrument-dirs` for 0.3.1; tests in `tests/cli_checks.py` (ctest `cli_combinations`, `cli_help`).

## Findings and what was done

| # | finding (who) | checked | done |
|---|---|---|---|
| 1 | Options silently without effect: `-no_ms1` with `-ms1_isotopes`/`-ms1_axis`/`-mz_tol_ms1`/`-mz_accuracy_ms1`/`-mz_offset_ms1`; `-scheme B` with `-mz_accuracy_*`/`-mz_calibration`/`-calibration:mz_model`; `-mz_accuracy_ms2` with a fixed `-mz_tol_ms2` (MS1 alike); `-rt_mode`/`-rt_anchors`/`-rt_trafo` with a negative `-rt_window`; `-calibration:*` when no calibration runs (own, Codex, Kimi) | yes, all accepted without a word | one `WARNING: without effect here: -a, -b (why); ...` line (an option counts as given when it differs from its default, on the command line or in an INI) |
| 2 | `-rt_trafo_invert` with `-rt_anchors` accepted (the check sat in the else branch) (Codex) | yes | error, independent of the other RT options |
| 3 | `-mz_calibration off` without fixed tolerances: help says they are needed; when the RT calibration runs, the tool measured tolerances anyway and centred them on the uncorrected m/z (Codex, Kimi; Kimi's "all zero" was wrong) | yes | error, as the help says |
| 4 | Two outputs on one path (`-out x -out_axes x`, or `-out` and the derived `x.axes.parquet`) and an output over an input were written without a word (Codex) | yes | error before any work, for all outputs and inputs (normalised paths) |
| 5 | `-lib` with only `-out_mzml`: the library was opened, the instrument files read, the calibration panel started; without `-lib` the same options were ignored silently (Codex, Kimi) | yes | export-only is decided by the outputs; `-lib` and the extraction options given are named in the warning of #1 |
| 6 | `-ids`: unknown ids dropped silently; none matching gave "Library is empty." (own, Codex, Kimi: the id syntax was undefined) | yes | warning with the count; an error that says no id matched and what an id is; the help defines the id with an example |
| 7 | `--helphelp`: 39 deprecated names listed as options, the calibration header three times (own, Codex, Kimi) | yes | TOPPBase patch: deprecated names skipped in the listing and named once at the end; one header (118 → 75 lines) |
| 8 | `--help` pointed at advanced-only options (`-mz_accuracy_ms2` from `-mz_tol_ms2`); "0 (default)" next to TOPP's "(default: '0.0')"; `-scheme B` "needs -mz_tol_ms1" also with `-no_ms1`; `-instrument_file` listed 6 of 9 keys; `-ms1_isotopes` showed min -1 unexplained; `-run_name` and `-out_calibration`/`-rt_anchors` vague; `-mz_calibration` help capitalised to "Auto:"; the window's fallback without calibration undocumented; mzPeak's centroided MS1 only in the README (own, Codex, Kimi) | yes | help texts rewritten |
| 9 | The instrument help implied validated support for every Orbitrap and TOF (Codex) | yes | the help says Astral and timsTOF are measured, the classes follow them |
| 10 | Calibration failures advised `-rt_anchors` also when no RT map was needed (own) | yes | the advice is left to the caller, which names what was needed |

## Not taken

- `-rt_mode` without a valid-strings list (Codex, Kimi, "high"): a typo is rejected at run time ("-rt_mode mintes: use
  auto, minutes or seconds", exit 6); a TOPP list would also reject 0.2.14's values before they are translated.
- `-threads` shows TOPP's "(default: '1')" (Codex, Kimi): TOPP owns the option; the real default is in the tool
  description line, as the guideline says.
- Renaming `-write_parts`/`-buffer_mb` (Codex) or deriving them (Kimi): renamed in 0.2.15 already; advanced, and the
  output does not depend on them.
- Descriptive scheme names (Codex): rejected in 0.2.15 already (A/B are documented in the help).
- Extension checks on `-out`, `-out_tsv`, `-out_stats` (Codex, Kimi): the files are written in their format whatever
  the name; low value against existing scripts.
- `-out_axes` without `-out` "ignored" (Kimi): wrong; the axes file is written with `-out_tsv` too.
