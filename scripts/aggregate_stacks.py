#!/usr/bin/env python3
"""Aggregate gdb 'thread apply all bt' samples (scripts/bench_run.py --stacks): per thread, its state from the innermost
frames (running, condition-variable wait, mutex/futex wait, mmap/munmap, page-fault-ish kernel entry, sleep, I/O) and
the first frame in our code (ODIA::...). Prints counts per (state, location), per sample and in total.

usage: aggregate_stacks.py STACKS.txt [STACKS.txt ...]
"""
import re, sys
from collections import Counter

WAIT_PATTERNS = [
    ("cv_wait", r"pthread_cond_(timed)?wait|__pthread_cond_wait|std::condition_variable::wait"),
    ("mutex_wait", r"__lll_lock_wait|pthread_mutex_lock|__pthread_mutex_lock|lll_lock"),
    ("mmap/munmap", r"\b(__)?mmap(64)?\b|\bmunmap\b|\bmprotect\b|\bmadvise\b"),
    ("malloc", r"\b(malloc|free|_int_malloc|_int_free|posix_memalign|realloc|tcache)\b"),
    ("sleep", r"nanosleep|clock_nanosleep|usleep"),
    ("io", r"\b(read|pread64|write|pwrite64|fsync|openat|__libc_read|__libc_write)\b"),
    ("omp_idle", r"libgomp"),
    ("futex(other)", r"futex|syscall"),
]
OURS = re.compile(r"\bin (ODIA::[A-Za-z_:<>~]+|[A-Za-z_]+Scheduler[A-Za-z_:]*|TOPPDIATraceExtractor[A-Za-z_:]*)")

def parse(path):
    samples, cur, thread, frames = [], None, None, []
    def flush():
        if thread is not None and cur is not None: cur.append((thread, frames[:]))
    for line in open(path, errors="replace"):
        if line.startswith("### sample"):
            flush(); thread = None; frames = []
            cur = []; samples.append(cur); continue
        m = re.match(r"^Thread \d+ .*?\"([^\"]*)\"", line)
        if m:
            flush(); thread = m.group(1); frames = []; continue
        if line.startswith("#"): frames.append(line.strip())
    flush()
    return samples

def classify(frames):
    inner = " ".join(frames[:4])
    state = "running"
    for name, rx in WAIT_PATTERNS:
        if re.search(rx, inner):
            state = name; break
    ours = next((OURS.search(f).group(1) for f in frames if OURS.search(f)), "(no ODIA frame)")
    return state, ours

for path in sys.argv[1:]:
    samples = parse(path)
    total = Counter()
    print(f"== {path}: {len(samples)} samples")
    for i, s in enumerate(samples):
        c = Counter(classify(fr) for _, fr in s)
        total.update(c)
        states = Counter()
        for (st, _), n in c.items(): states[st] += n
        print(f"  sample {i}: {len(s)} threads; " + ", ".join(f"{k} {v}" for k, v in states.most_common()))
    print("  total by (state, first ODIA frame):")
    for (st, ours), n in total.most_common(25):
        print(f"    {n:5d}  {st:14s} {ours}")
