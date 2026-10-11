#!/usr/bin/env python3
"""Analyse scripts/profile_scaling.sh output: stage times, ingest and scheduler counters, idle reasons, per-task
inflation, context switches; figures: stacked stage wall times per thread count and scheduler occupancy timelines.

usage: analyze_profile_scaling.py RUN_DIR OUT_PREFIX
"""
import glob, os, re, sys
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

run_dir, out = sys.argv[1], sys.argv[2]

def num(rx, txt, default=np.nan, cast=float):
    m = re.search(rx, txt)
    return cast(m.group(1)) if m else default

def wallcpu(prefix, txt):
    m = re.search(r"^" + re.escape(prefix) + r".*?([0-9.]+) s wall, ([0-9.]+) s CPU", txt, re.M)
    return (float(m.group(1)), float(m.group(2))) if m else (np.nan, np.nan)

rows, counters, idle, classes = [], [], [], []
for log in sorted(glob.glob(os.path.join(run_dir, "*.log"))):
    name = os.path.basename(log)[:-4]
    if name == "runs": continue
    txt = open(log).read()
    t = num(r"-threads (\d+)", txt, cast=int)
    r = {"run": name, "threads": t}
    r["ingest_wall"], r["ingest_cpu"] = wallcpu("Run grid:", txt)
    r["panel_wall"], r["panel_cpu"] = wallcpu("Calibration panel library:", txt)
    m = re.search(r"^Calibration: .*; ([0-9.]+) s wall, ([0-9.]+) s CPU", txt, re.M)
    r["calib_wall"], r["calib_cpu"] = (float(m.group(1)), float(m.group(2))) if m else (np.nan, np.nan)
    m = re.search(r"^Extraction: .*; ([0-9.]+) s wall, ([0-9.]+) s CPU", txt, re.M)
    r["extract_wall"], r["extract_cpu"] = (float(m.group(1)), float(m.group(2))) if m else (np.nan, np.nan)
    r["drain_wall"] = num(r"final drain ([0-9.e-]+) s", txt)
    r["tool_total"] = num(r"^Total: ([0-9.]+) s wall", txt.replace("\r", ""), )
    if np.isnan(r["tool_total"]):
        r["tool_total"] = num(r"Total: ([0-9.]+) s wall", txt)
    r["tool_cpu"] = num(r"Total: [0-9.]+ s wall, ([0-9.]+) s CPU", txt)
    parts = num(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([0-9:.]+)", txt, cast=str)
    if isinstance(parts, str):
        p = [float(x) for x in parts.split(":")]; r["process_wall"] = sum(v * 60 ** (len(p) - 1 - i) for i, v in enumerate(p))
    r["vol_cs"] = num(r"Voluntary context switches: (\d+)", txt)
    r["invol_cs"] = num(r"Involuntary context switches: (\d+)", txt)
    r["peak_gib"] = num(r"Maximum resident set size \(kbytes\): (\d+)", txt) / 1048576
    m = re.search(r"^Scheduler: (\d+) workers;(.*)", txt, re.M)
    if m:
        for cls, n, busy, avg, mx in re.findall(r" ([a-z+]+) (\d+) tasks, ([0-9.]+) thread-s \(avg ([0-9.]+), max ([0-9.]+) workers\)", m.group(2)):
            classes.append({"run": name, "threads": t, "class": cls, "tasks": int(n), "busy_s": float(busy),
                            "avg_workers": float(avg), "max_workers": float(mx), "ms_per_task": float(busy) / max(1, int(n)) * 1e3})
    for cname, tot, cnt, mean in re.findall(r"^\[profile\] (\S+)\s+([0-9.]+)\s+(\d+)\s+([0-9.]+)$", txt, re.M):
        counters.append({"run": name, "threads": t, "counter": cname, "total_s": float(tot), "count": int(cnt), "mean_ms": float(mean)})
        if cname.startswith("sched.idle."):
            idle.append({"run": name, "threads": t, "reason": cname[len("sched.idle."):], "thread_s": float(tot)})
    rows.append(r)

df = pd.DataFrame(rows).sort_values(["run"])
df["other_wall"] = df.process_wall - df[["ingest_wall", "panel_wall", "calib_wall", "extract_wall"]].sum(axis=1)
cnt = pd.DataFrame(counters); idl = pd.DataFrame(idle); cls = pd.DataFrame(classes)
df.to_csv(out + "_stages.tsv", sep="\t", index=False, float_format="%.3f")
cnt.to_csv(out + "_counters.tsv", sep="\t", index=False, float_format="%.4f")
cls.to_csv(out + "_classes.tsv", sep="\t", index=False, float_format="%.4f")
if len(idl): idl.to_csv(out + "_idle.tsv", sep="\t", index=False, float_format="%.3f")

pd.set_option("display.width", 250); pd.set_option("display.max_columns", 40)
scal = df[df.run.str.match(r"^t\d+$")].sort_values("threads")
print("== stages (s)"); print(scal[["threads", "process_wall", "tool_total", "ingest_wall", "ingest_cpu", "panel_wall", "calib_wall", "calib_cpu",
                                   "extract_wall", "extract_cpu", "other_wall", "peak_gib", "vol_cs", "invol_cs"]].to_string(index=False))
print("\n== NUMA variants"); print(df[~df.run.str.match(r"^t\d+$")][["run", "threads", "process_wall", "ingest_wall", "calib_wall", "extract_wall", "peak_gib"]].to_string(index=False))
if len(cnt):
    piv = cnt[cnt.run.str.match(r"^t\d+$")].pivot_table(index="counter", columns="threads", values="total_s")
    print("\n== counters: total seconds (thread-s for per-task counters, wall for (wall))"); print(piv.round(2).to_string())
if len(cls):
    print("\n== scheduler classes"); print(cls[cls.run.str.match(r"^t\d+$")].pivot_table(index="class", columns="threads", values=["busy_s", "avg_workers", "ms_per_task"]).round(2).to_string())
if len(idl):
    print("\n== scheduler idle thread-seconds by reason"); print(idl[idl.run.str.match(r"^t\d+$")].pivot_table(index="reason", columns="threads", values="thread_s").round(1).to_string())

# ---- figure 1: stacked stage walls + efficiency
fig, axs = plt.subplots(1, 2, figsize=(13, 4.6))
st = ["ingest_wall", "panel_wall", "calib_wall", "extract_wall", "other_wall"]
lab = ["ingest (mzML -> grid)", "calibration panel load", "calibration", "extraction (scheduler)", "other (start-up, close, axes)"]
col = ["#264653", "#8ab17d", "#2a9d8f", "#e9c46a", "#bbbbbb"]
x = np.arange(len(scal)); bottom = np.zeros(len(scal))
for s_, l_, c_ in zip(st, lab, col):
    v = scal[s_].fillna(0).to_numpy(); axs[0].bar(x, v, bottom=bottom, color=c_, label=l_); bottom += v
for xi, tot in zip(x, bottom): axs[0].text(xi, tot + 0.2, f"{tot:.1f}", ha="center", fontsize=8)
axs[0].set_xticks(x); axs[0].set_xticklabels(scal.threads.astype(int)); axs[0].set_xlabel("threads"); axs[0].set_ylabel("wall time (s)")
axs[0].set_title("Wall time per stage, full 7.0 M-precursor library, run 01", loc="left", fontsize=10); axs[0].legend(fontsize=7.5, frameon=False)
t0 = scal.threads.iloc[0]
for s_, l_, c_ in zip(st[:-1] + ["process_wall"], lab[:-1] + ["whole run"], col[:-1] + ["#000000"]):
    w0 = scal[s_].iloc[0]
    axs[1].plot(scal.threads, (w0 * t0) / (scal[s_] * scal.threads), marker="o", color=c_, label=l_)
axs[1].set_xscale("log", base=2); axs[1].set_xticks(scal.threads); axs[1].set_xticklabels(scal.threads.astype(int))
axs[1].set_ylim(0, 1.2); axs[1].set_xlabel("threads"); axs[1].set_ylabel(f"parallel efficiency vs {t0} threads")
axs[1].set_title("Parallel efficiency per stage", loc="left", fontsize=10); axs[1].legend(fontsize=7.5, frameon=False); axs[1].grid(alpha=0.3)
fig.tight_layout(); fig.savefig(out + "_stages.png", dpi=160); plt.close(fig)

# ---- figure 2: timelines
tls = [f for f in sorted(glob.glob(os.path.join(run_dir, "t*.timeline.tsv")), key=lambda p: int(re.search(r"t(\d+)\.", os.path.basename(p)).group(1)))]
if tls:
    fig, axs = plt.subplots(len(tls), 1, figsize=(12, 1.9 * len(tls)), sharex=True)
    axs = np.atleast_1d(axs)
    for ax, f in zip(axs, tls):
        d = pd.read_csv(f, sep="\t")
        if d.empty: continue
        ax.stackplot(d.t, d.read, d.extract, d.encode, d.write, d.idle, colors=["#8ab17d", "#e9c46a", "#f4a261", "#e76f51", "#dddddd"],
                     labels=["read+plan", "extract", "encode", "write", "idle (waiting)"])
        ax.plot(d.t, d.process_cpu, color="k", lw=0.8, label="process CPU (cores)")
        ax.set_ylabel(os.path.basename(f).split(".")[0], rotation=0, ha="right", fontsize=9)
        ax.set_ylim(0, None)
    axs[0].legend(ncol=6, fontsize=7.5, frameon=False, loc="upper right")
    axs[-1].set_xlabel("time since extraction start (s)")
    fig.suptitle("Scheduler occupancy during extraction: workers per task class (stacked) and process CPU", x=0.01, ha="left", fontsize=10)
    fig.tight_layout(); fig.savefig(out + "_timelines.png", dpi=150); plt.close(fig)
print("figures ->", out + "_stages.png", out + "_timelines.png")
