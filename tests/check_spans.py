#!/usr/bin/env python3
"""Exit 0 iff every MS2 trace of an extraction with an RT window equals the whole-run extraction of the same target
cut to the trace's cycles [cycle_begin, cycle_begin + length). Also requires that the windowed run has, within one
window, a later precursor whose span ends before an earlier one's (the case the merge join once got wrong, writing
past the end of the trace), so that the check is not vacuous.
usage: check_spans.py WINDOWED.parquet WHOLE_RUN.parquet"""
import sys
import numpy as np
import pyarrow.parquet as pq

win = pq.read_table(sys.argv[1]).to_pandas()
whole = pq.read_table(sys.argv[2]).to_pandas()
win, whole = win[win.ms_level == 2], whole[whole.ms_level == 2]
key = ["precursor_id", "window", "annotation"]
ref = {tuple(r[k] for k in key): np.frombuffer(r.intensity, "<f4") for _, r in whole.iterrows()}

spans = win.assign(n=[len(b) // 4 for b in win.intensity]).groupby(["window", "precursor_id"]).agg(
    rt=("rt_center", "first"), begin=("cycle_begin", "first"), n=("n", "first")).reset_index()
spans["end"] = spans.begin + spans.n
inverted = 0
for _, g in spans.groupby("window"):
    g = g.sort_values("rt")
    inverted += int((g.end.cummax().shift(1) > g.end).sum())
if inverted == 0:
    sys.exit("no span ends before an earlier one: the check would not exercise the merge join's closing")

bad = 0
for _, r in win.iterrows():
    y = np.frombuffer(r.intensity, "<f4")
    expect = ref[tuple(r[k] for k in key)][r.cycle_begin:r.cycle_begin + len(y)]
    if not np.array_equal(y, expect):
        bad += 1
        print("differs:", r.precursor_id, r.annotation, "window", r.window, "cycles", r.cycle_begin, len(y), y, expect)
print(f"{len(win)} MS2 traces, {inverted} span(s) ending before an earlier one, {bad} differing from the whole run")
sys.exit(1 if bad else 0)
