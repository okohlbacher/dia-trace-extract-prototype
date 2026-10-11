#!/usr/bin/env python3
"""Analyse scripts/diagnose_contention.sh output: per-task wall/CPU/faults/context switches per class, lock waits,
core frequency during extraction, stage times; experiments A (threads), B (processes), C (unit size), D (SMT).

usage: analyze_contention.py RUN_DIR OUT_PREFIX
"""
import glob, os, re, sys
import numpy as np, pandas as pd

run_dir, out = sys.argv[1], sys.argv[2]
pd.set_option("display.width", 250); pd.set_option("display.max_columns", 50); pd.set_option("display.max_rows", 200)

def stage(rx, txt):
    m = re.search(rx + r".*?([0-9.]+) s wall, ([0-9.]+) s CPU", txt, re.M)
    return (float(m.group(1)), float(m.group(2))) if m else (np.nan, np.nan)

runs, tasks, locks = [], [], []
for log in sorted(glob.glob(os.path.join(run_dir, "*.log"))):
    name = os.path.basename(log)[:-4]
    if name == "runs": continue
    txt = open(log).read()
    t = int(re.search(r"-threads (\d+)", txt).group(1))
    r = {"run": name, "threads": t}
    r["ingest"], _ = stage(r"^Run grid:", txt)
    m = re.search(r"^Extraction: .*; ([0-9.]+) s wall, ([0-9.]+) s CPU", txt, re.M)
    r["extract_wall"], r["extract_cpu"] = (float(m.group(1)), float(m.group(2))) if m else (np.nan, np.nan)
    m = re.search(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([0-9:.]+)", txt)
    if m:
        p = [float(x) for x in m.group(1).split(":")]; r["process_wall"] = sum(v * 60 ** (len(p) - 1 - i) for i, v in enumerate(p))
    for k, rx in (("vol_cs", r"Voluntary context switches: (\d+)"), ("invol_cs", r"Involuntary context switches: (\d+)"),
                  ("minflt", r"Minor \(reclaiming a frame\) page faults: (\d+)")):
        mm = re.search(rx, txt); r[k] = int(mm.group(1)) if mm else np.nan
    for cls, n, w, c, cw, flt, vcs, ivcs in re.findall(r"^\[task\] (\S+)\s+(\d+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+(\d+)\s+(\d+)\s+(\d+)$", txt, re.M):
        n = int(n)
        tasks.append({"run": name, "threads": t, "class": cls, "tasks": n, "wall_s": float(w), "cpu_s": float(c),
                      "cpu_per_wall": float(cw), "ms_wall_per_task": float(w) / max(n, 1) * 1e3, "ms_cpu_per_task": float(c) / max(n, 1) * 1e3,
                      "faults_per_task": int(flt) / max(n, 1), "vcs_per_task": int(vcs) / max(n, 1), "ivcs_per_task": int(ivcs) / max(n, 1)})
    for cname, tot, cnt in re.findall(r"^\[profile\] (lock\.\S+|sched\.lock_acquire|sched\.idle\.\S+)\s+([0-9.]+)\s+(\d+)", txt, re.M):
        locks.append({"run": name, "threads": t, "counter": cname, "total_s": float(tot), "count": int(cnt)})
    tl = os.path.join(run_dir, name + ".timeline.tsv")
    if os.path.exists(tl):
        d = pd.read_csv(tl, sep="\t")
        if "mhz_mean" in d and len(d):
            busy = d[(d.extract + d.encode + d.write) > 0]
            r["mhz_mean"] = busy.mhz_mean.replace(0, np.nan).mean(); r["mhz_p10"] = busy.mhz_min.replace(0, np.nan).quantile(0.5)
            r["running_threads"] = busy.running_threads.mean(); r["process_cpu"] = busy.process_cpu.mean()
    runs.append(r)

R = pd.DataFrame(runs); T = pd.DataFrame(tasks); L = pd.DataFrame(locks)
R.to_csv(out + "_runs.tsv", sep="\t", index=False, float_format="%.3f")
T.to_csv(out + "_tasks.tsv", sep="\t", index=False, float_format="%.4f")
L.to_csv(out + "_locks.tsv", sep="\t", index=False, float_format="%.4f")

def show(title, df): print(f"\n== {title}"); print(df.to_string(index=False))

show("runs", R[["run", "threads", "process_wall", "ingest", "extract_wall", "extract_cpu", "mhz_mean", "running_threads", "process_cpu",
                "vol_cs", "invol_cs", "minflt"]].round(2))
show("per task (all runs)", T[["run", "class", "tasks", "ms_wall_per_task", "ms_cpu_per_task", "cpu_per_wall", "faults_per_task",
                               "vcs_per_task", "ivcs_per_task"]].round(3))
if len(L):
    piv = L.pivot_table(index="counter", columns="run", values="total_s").round(2)
    print("\n== lock and idle counters (thread-seconds)"); print(piv.to_string())
