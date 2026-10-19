# Scaling beyond 64 threads and the memory peak (0.4.0, 2026-10-19)

Workload: Astral run 01 with the 7.0 M-precursor entrapment library at the defaults (`-rt_mode minutes`, measured RT
window), on `dax` (2 × AMD EPYC 9654: 192 cores, SMT, 2 NUMA nodes, 2.3 TB; load ≤ 0.06 per hardware thread unless
noted). Scripts and logs: `results/scaling64/`, `results/memory/` (not in git).

## Threads

Before (`results/scaling64/base`, two repeats each; wall s / CPU s / voluntary context switches):

| threads | wall | CPU | context switches | extract thread-s (40,848 tasks) |
|---|---|---|---|---|
| 64 | 11.9 | 548–558 | 0.14 M | 210 |
| 96 | 10.5–10.7 | 606–614 | 0.65–0.69 M | |
| 192 | 21.9–22.2 | 1,118–1,123 | 3.5–3.6 M | 465 |
| 384 | 40.6 | 1,483–1,670 | 7.5–7.7 M | 495 |

At 192 threads the same tasks took twice the thread time, while no more than about 57 cores were active on average:
the extra threads added CPU, not throughput. Two causes, both in the scheduler (`src/Scheduler.cpp`):

1. **Broadcast wake-ups.** Every task end called `notify_all()`: all idle workers woke, queued for the one central
   lock, found nothing and slept again. With 130+ idle workers that is the 3.6 M context switches, and the working
   threads wait for the same lock to commit. Now a state change wakes one sleeping worker, and a worker that takes a
   task wakes the next before releasing the lock, so new work spreads in a cascade that ends at the first worker
   finding nothing (at most one wasted wake-up instead of one per idle worker). Context switches at 192 threads:
   3.6 M → 0.56 M.
2. **The in-flight cap** (`ahead`, units extracted but not yet committed) was `min(4 × threads, 64)`, so beyond 64
   threads no more units could run. It is now `max(min(4 × threads, 64), 2 × threads)`: unchanged up to 32 threads,
   2 per thread beyond. The trace pool (`-buffer_mb`) still bounds the memory these units hold (+0.15 GiB at 192
   threads).

After (`results/scaling64/final`, two repeats): 16 threads 39.2–39.5 s, 32 21.7–21.9, 64 12.2, 96 10.6–11.3,
**192 11.6–12.0** (was 22), **384 12.6–12.8** (was 40.6). The output is identical at 16, 64 and 192 threads (all
62,977,623 rows, `scripts/same_parquet_stream.py`). Measured separately: wake-one alone gave 192 → 13.6 s and 384 →
15.1 s; raising the cap took them to 11.8 and 12.8 s. The fallback of exponential back-off with jitter was not needed.

What remains above 96 threads is not the scheduler: the extract tasks themselves take 1.9× the thread time at 192
threads (memory bandwidth across the two sockets), and the 32 Parquet writers keep the busiest one busy for about 7 of
the 8.5 s of extraction. 96 threads stays the fastest point on this machine (10.3–10.6 s); the default (the physical
cores, 192 here) is now as fast as 64 threads instead of twice as slow.

## Memory

The backlog item ("library-load transient: decoded row groups, parts and a joined copy coexist, ~4.3 GiB") dates
from whole-library loading. The library is streamed in parts since V0.2: an RSS timeline (`ODIA_PROFILE_TIMELINE`,
which now also records RSS, glibc in-use and held-free bytes, the Arrow pools and the bytes waiting for the writers)
shows no transient at library loading. The peak (5.2 GiB at 64 threads, 5.9 at 192) is the plateau of the
extraction:

| part | GiB |
|---|---|
| run grid (141 M centroids) and process | ~1.5 |
| Arrow pools: library parts (decoded zero-copy, 5–11 resident) and Parquet encoding | 0.8 → 1.8 |
| of which the recycling pool's reuse cache | up to 1.0 |
| trace pool (`-buffer_mb 512`) | 0.5 |
| glibc in use / held free | ~1.5 / ~0.7 |

Levers measured (64 / 192 threads): a smaller reuse cache (256 MiB: −0.45 GiB, +20% / +130% time; none: −0.6 GiB,
6–7× slower), fewer glibc arenas (`MALLOC_ARENA_MAX=8`: −0.45 to −0.65 GiB, 5–20% slower), fewer library parts read
ahead (14 → 4: −0.1 GiB), a smaller trace pool (256 MiB: −0.17 to −0.3 GiB), fewer Parquet writers (larger and much
slower), a fixed mmap threshold (no gain). None is worth its cost at 5–6 GiB against the 64 GB budget; the defaults
stay.

The TSV path did have a problem: the human library (3.5 M precursors) to `-out_tsv` peaked at 11.2 GiB (the 7 M
library at 23 GiB). Each unit's text (about 25 MB) grew as a `std::string`, whose doubling copies left 3.5 GiB of
freed blocks in glibc's per-thread arenas, and up to 2.3 GiB of encoded text queued for the one TSV writer (the queue
was bounded by units, not bytes). Now the text is one buffer of the recycling Arrow pool, sized from the unit's
points, and the bytes waiting for the writers are bounded (1 GiB) for extraction and encoding (encoding proceeds
when no writer can write, so the order of the writers cannot stall it). Human library: 11.2 → 6.3 GiB, 85 → 66 s;
the TSV is byte-identical (10.4 GB checked).
