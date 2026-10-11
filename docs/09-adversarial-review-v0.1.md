# Adversarial review before V0.1 (2026-10-11)

Three independent reviewers read commit d0109d7 against the brief in `docs/review/brief.md` (invariants, ten
numbered attack targets, documentation congruence): **Codex** (gpt, `codex exec`, no sandbox in an isolated git
worktree, since bwrap is blocked on this node), **Vibe** (Mistral, isolated worktree), and **Claude** (an
in-session subagent; `claude -p` had no valid login). None modified the tree (verified by `git status` on every
worktree). Full reports: `docs/review/codex.md`, `docs/review/vibe.md`, `docs/review/claude.md`.

Both Codex and Claude went beyond reading: Codex ran a 10,000-case randomized model of the merge join, Claude a
differential fuzz of the real `join` (ASan/UBSan, 1.55 M values) against a naive per-target search. **Neither found a
mismatch.** All three independently rated the scheduler deadlock-free and the calibration's offset handling correct.

## Confirmed and fixed (commit after d0109d7)

| # | finding | found by | fix |
|---|---|---|---|
| 1 | Scheduler members: the recycling pools were destroyed before the queues whose `Part` deleters use them (UB on the error exit) | Codex | declaration order |
| 2 | `-rt_mode auto -mass_calibration off` applied the measured offsets and ignored `-mz_offset*` (regression from the same day's fallback change) | Vibe, Claude | offsets follow `-mass_calibration` |
| 3 | Fast mzML parser keys windows on the isolation target, the OpenMS fallback on the selected-ion m/z: `-threads 1` vs `>= 2` could grid a file differently | Claude | `setPrecursorMZSelectedIon(false)` on every OpenMS loader |
| 4 | `-ids` with a non-compact library shrank the calibration panel (fraction of the filtered rows) | Codex, Claude | panel from the unfiltered row count |
| 5 | Strict-anchor predicate of the mass model omitted the fragment count | Codex | anchors carry their fragment count |
| 6 | `-ms1_isotopes 255` wrapped a `uint8`; `-top_fragments 0` with > 65,535 fragments truncated; negative `calibration:mode_bandwidth` ran out of bounds; `log8_decades 0` divided by zero | Codex, Vibe, Claude | option bounds, explicit error |
| 7 | Stage-1 mode filter silently skipped when < 20 anchors agreed, then LOWESS fitted garbage | Claude | error instead |
| 8 | Mass-search convergence ignored offset changes | Codex | offsets in the stop test |
| 9 | `-rt_anchors` refitted rejected anchors (`used` column ignored) | Codex | honoured |
| 10 | Fast parser: last precursor / time term won; declared-empty spectra with arrays accepted; absurd declared lengths | Codex, Vibe | first term wins; fall back to OpenMS |
| 11 | Exceptions escaping the OpenMP region of `examineWholeRun`; per-thread stage-1 tiles unbounded in run length × panel | Codex, Vibe | captured and rethrown; windows processed in ≤ 256 MiB batches |
| 12 | `massModel` on an empty / tiny sample gave accuracy 0 (or 0.1 ppm) silently; only `runtime_error` took the fallback | Vibe, Claude | 50-centroid floor, explicit error, `std::exception` |
| 13 | Unsorted cycle axis and missing mzPeak retention times undetected | Vibe | checked at load |
| 14 | NaN library RT broke the plan's sort comparator | Claude | skipped as "without window" |
| 15 | TSV writes and the stats JSON unchecked; control characters in JSON | Codex, Vibe | checked / escaped |
| 16 | Pool reservation (file bytes / 2) too small for highly compressible zlib arrays | Codex, Vibe | file bytes (virtual only) |
| 17 | Tests: parity test relied on the old `sum` default, determinism compared tables not bytes, `tools/pyenv` hard-coded | Codex, Vibe, Claude | explicit options, byte digest, `python3` fallback |
| 18 | `orderedParallel(ahead = 0)` would self-deadlock | Vibe | clamped |

Documentation statements corrected in `docs/EXTRACTION.md` / `README.md`: `target_mz` is the uncorrected library
m/z (offsets in the footer); the panel is 5% of the library rows; candidate apexes are ≥ 8 cycles apart; the mode
bins are equal-count and adaptive; the tolerance floor and the search-width cap; which thresholds are options and
which are floors; parallel parsing applies to uncompressed mzML; only compact Parquet libraries are streamed; MS1
rows are on the scan axis (README snippet); fixed-offset example needs `-mass_calibration off`; the anchor search
uses ≤ 32 threads.

## Disagreements and judgement calls

- **MS1-only extraction when the MS2 cycle range is empty** (Codex): a precursor whose RT window lies outside the
  acquired cycles gets no MS1 trace either. Left as is: a record outside the run is reported as such; filed.
- **Sorting ties between the two mzML parsers** (Codex): centroids equal at 1e-5 Th are ordered differently by the
  fast parser (stable sort of the quantized values) and OpenMS (sort of the doubles). With `max` aggregation the
  result is identical; with `sum` the float addition order can differ in the last bit. Not fixed; noted.
- **`keep_every` panel sampling exists only for compact Parquet** (Vibe): other formats load the whole library
  for the panel. Correct but memory-heavy for large TSV libraries; filed.
- **Vibe's "tests cannot run"**: true for a checkout without `tools/pyenv`; the Python fallback covers it, the
  OpenMS binaries are still required.

## Verified by running, not only by reading

After the fixes: 7/7 ctest; runs 01/02 with the ID libraries and run 01 with the full library give the same
calibration (MS2 ±5.5–6.2, MS1 ±2.2–2.4 ppm) and the same agreement with DIA-NN as before the review (run 01:
91.2% of MS2 traces with r > 0.9, MS1 M+0 r 0.980; run 02: 80.7%, 0.996).
