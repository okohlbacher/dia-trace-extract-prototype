# Messages: before and after (0.3.0)

The default log of Astral run 01 (shared tuned library, 82,333 precursors, 32 threads) went from 3.1 kB to 1.4 kB
(`docs/cli-review/log-before.txt`, `log-after.txt`; `log-after-debug.txt` with `-debug 1`). The line prefixes are
unchanged, and each line still ends with its timing (`; <wall> s wall, <cpu> s CPU (<%>)`), so the analysis scripts'
patterns match. What moved behind `-debug 1` is listed per line.

| line | 0.2.16 (default level) | 0.3.0 (default level) | behind `-debug 1` |
|---|---|---|---|
| `Library:` | rows, parts, sources, "streamed during extraction", open time, memory, page faults (172 characters) | `Library: 82333 rows` | parts, sources, open time, memory |
| `Run grid:` | cycles, windows, spectra, peaks, empty cells, overlapping pairs, MiB, time, memory (205) | cycles x windows, MS1/MS2 spectra; time | peaks, empty cells, overlapping windows, MiB, memory |
| `Instrument:` | `'Orbitrap Astral' -> Orbitrap; tolerances 9 sigma (MS2), 4 sigma (MS1)` | `Astral, from the run's metadata ('Orbitrap Astral'); defaults astral.yaml (Astral) over orbitrap.yaml over default.yaml` | `Instrument files:` directory, directories searched, file hashes |
| `<instrument> defaults:` | – | the multiples an instrument file implied and the options that change them (only for values not given) | – |
| `Calibration panel library:` | panel rows, time, wait, memory (194) | – | as before |
| `Calibration:` | stage counts, search window, panel, RT residual p50/p95/p99, FWHM in cycles and s, co-elution window, score and strict cuts, decoys, offsets, widths, accuracies, coverage widths, co-elution curve, search passes, time (732) | anchors (used); RT residual p50/p95; peak FWHM; MS2 and MS1 offsets (with the m/z slope when used) and widths; time | `Calibration detail:` everything else |
| `Scheduler:` | task classes, thread-s, workers, pool blocks, parts (401) | – | as before |
| `Extraction:` | precursors, fragments, parts, assignments, units, without window, outside RT range, traces, points, lookups, time, memory (359) | precursors, traces (and the precursors in no isolation window, when any); time with threads | fragments, parts, assignments, units, points, lookups, memory |
| `Written:` | rows, points, parts, busiest writer, final drain, append time (188) | rows -> file | points, parts, writer times |
| `Values used:` | – | – | every resolved value with its source |

Warnings (all now start with `WARNING:`):

| 0.2.16 | 0.3.0 |
|---|---|
| `Instrument class unknown: MS2 tolerance 6 sigma; set -mz_accuracy_ms2 (Orbitrap 9, TOF 4).` | `WARNING: instrument not recognised ('LCQ Deca' in the run's metadata): the generic defaults (fragment tolerance 6 x the measured width); -instrument names it (Astral, Orbitrap, timsTOF, TOF).` |
| – | `WARNING: the instrument given (timsTOF) differs from 'Orbitrap Astral' in the run's metadata (Astral); using timsTOF.` |
| `Calibration failed: <why>; no mass correction, the given tolerances.` | `WARNING: calibration failed (<why>): no mass correction; the given tolerances apply.` |
| `No calibration ran: -out_calibration not written.` | `WARNING: no calibration ran, so -out_calibration is not written.` |
| `RT window: +-60 s (too few calibration anchors to measure the RT scatter; -rt_window sets it).` | `WARNING: RT window +-60 s: too few calibration anchors to measure the RT scatter (-rt_window sets it).` |
| `The library's RTs map onto ... check -rt_mode ...` | `WARNING: the library's RTs map onto ... check -rt_mode ...` |
| `WARNING: options of 0.2.14 (accepted until 0.4): ...` | unchanged |
| – | `WARNING: not carried over into the new INI (removed in 0.2.15): ...` (`-write_ini -ini <old INI>`) |

Errors:

| 0.2.16 | 0.3.0 |
|---|---|
| `Unknown option(s) '[-mz_tolx]' given. Aborting!` followed by the 33-line usage | `Unknown option '-mz_tolx' (did you mean -mz_tol_ms2?).` per option, then `'--help' lists the options, '--helphelp' all of them.` |
| – | `Error: unknown instrument 'Fancy' (-instrument): use auto or one of Astral, Orbitrap, timsTOF, TOF; other instruments: -instrument_file` |
| – | `Error: instrument file <path>, key '<key>': <unknown key (allowed: ...) / given twice / expected a number / out of range / missing>` |
| – | `Give either -instrument or -instrument_file, not both.` |
