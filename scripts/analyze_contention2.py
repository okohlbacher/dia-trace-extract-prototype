#!/usr/bin/env python3
"""Metrics per the parallel-efficiency skill (metrics.md) for scripts/diagnose_contention2.sh output:
speed-up / efficiency / Karp-Flatt against the 1-thread run, noise from repeats, whole-process active cores from
procsample (incl. time at <= 1.5 cores, thread states), others' CPU, output hashes, stack-sample aggregation.

usage: analyze_contention2.py RUN_DIR OUT_PREFIX
"""
import glob, os, re, subprocess, sys
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

run_dir, out = sys.argv[1], sys.argv[2]
pd.set_option("display.width", 250); pd.set_option("display.max_columns", 40)

rows = []
for meta in sorted(glob.glob(os.path.join(run_dir, "*.meta.tsv"))):
    name = os.path.basename(meta)[:-len(".meta.tsv")]
    m = pd.read_csv(meta, sep="\t").iloc[0].to_dict()
    txt = open(os.path.join(run_dir, name + ".log")).read()
    t = int(re.search(r"-threads (\d+)", txt).group(1))
    r = {"run": name, "threads": t, **m}
    for key, rx in (("ingest", r"^Run grid:"), ("panel", r"^Calibration panel library:")):
        mm = re.search(rx + r".*?([0-9.]+) s wall, ([0-9.]+) s CPU", txt, re.M)
        r[key] = float(mm.group(1)) if mm else np.nan
    mm = re.search(r"^Calibration: .*; ([0-9.]+) s wall", txt, re.M); r["calib"] = float(mm.group(1)) if mm else np.nan
    mm = re.search(r"^Extraction: .*; ([0-9.]+) s wall, ([0-9.]+) s CPU", txt, re.M)
    r["extract"], r["extract_cpu"] = (float(mm.group(1)), float(mm.group(2))) if mm else (np.nan, np.nan)
    mm = re.search(r"Maximum resident set size \(kbytes\): (\d+)", txt); r["peak_gib"] = int(mm.group(1)) / 1048576 if mm else np.nan
    ps = os.path.join(run_dir, name + ".procsample", "proc.tsv")
    if os.path.exists(ps):
        p = pd.read_csv(ps, sep="\t", na_values="NA")
        ac = p.get("active_cores")
        if ac is not None and len(p) > 2:
            dt = p["t"].diff() if "t" in p else pd.Series(np.full(len(p), 0.2))
            r["mean_active_cores"] = float(np.nansum(ac * dt) / np.nansum(dt))
            r["time_le_1p5_cores_s"] = float(np.nansum(dt[ac <= 1.5]))
            r["max_active_cores"] = float(np.nanmax(ac))
            r["mean_running_threads"] = float(p["n_running"].mean()) if "n_running" in p else np.nan
            r["mean_threads"] = float(p["threads"].mean()) if "threads" in p else np.nan
            last = p.iloc[-1]
            r["system_share"] = float(last.stime_s / max(last.cpu_s, 1e-9)) if "stime_s" in p else np.nan
            r["vcsw"] = last.get("vcsw", np.nan); r["ivcsw"] = last.get("ivcsw", np.nan)
    rows.append(r)

df = pd.DataFrame(rows).sort_values(["threads", "run"])
df.to_csv(out + "_runs.tsv", sep="\t", index=False, float_format="%.3f")
print("== runs (hygiene: others_cores = CPU used by other processes during the run, in cores)")
print(df[["run", "threads", "wall_s", "own_cpu_s", "others_cores", "ingest", "panel", "calib", "extract", "peak_gib",
          "mean_active_cores", "time_le_1p5_cores_s", "mean_running_threads", "system_share", "vcsw", "ivcsw",
          "output_sha256_16"]].round(2).to_string(index=False))

# noise from repeats
rep = df[df.run.str.match(r"^S_t\d+_r\d$")]
if len(rep):
    g = rep.groupby("threads").agg(n=("wall_s", "size"), wall_mean=("wall_s", "mean"), wall_sd=("wall_s", "std"),
                                   extract_mean=("extract", "mean"), extract_sd=("extract", "std"))
    g["wall_cv_%"] = 100 * g.wall_sd / g.wall_mean
    print("\n== noise (3 interleaved repeats)"); print(g.round(3).to_string())

# speed-up, efficiency, Karp-Flatt vs the 1-thread run (whole run and extraction stage)
base = df[df.run == "S_t1"]
if len(base):
    b = base.iloc[0]
    agg = df[df.run.str.match(r"^S_t")].groupby("threads").agg(wall=("wall_s", "mean"), extract=("extract", "mean"),
                                                                 ingest=("ingest", "mean"), calib=("calib", "mean")).reset_index()
    for col in ("wall", "extract", "ingest", "calib"):
        T1 = {"wall": b.wall_s, "extract": b.extract, "ingest": b.ingest, "calib": b.calib}[col]
        S = T1 / agg[col]; p = agg.threads
        agg[f"S_{col}"] = S; agg[f"E_{col}"] = S / p
        agg[f"KF_{col}"] = np.where(p > 1, (1 / S - 1 / p) / (1 - 1 / p), np.nan)
    print("\n== speed-up S, efficiency E, Karp-Flatt serial fraction e (vs the 1-thread run)")
    print(agg.round(3).to_string(index=False))

# stacks
for f in sorted(glob.glob(os.path.join(run_dir, "*.stacks.txt"))):
    if os.path.getsize(f) > 0:
        print(); print(subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "aggregate_stacks.py"), f],
                                      capture_output=True, text=True).stdout)

# active-cores timelines (whole process)
tl = [n for n in df.run if os.path.exists(os.path.join(run_dir, n + ".procsample", "proc.tsv")) and re.match(r"S_t(1|32_r1|64_r1)$|K_t128", n)]
if tl:
    fig, axs = plt.subplots(len(tl), 1, figsize=(11, 1.9 * len(tl)), sharex=False)
    for ax, n in zip(np.atleast_1d(axs), tl):
        p = pd.read_csv(os.path.join(run_dir, n + ".procsample", "proc.tsv"), sep="\t", na_values="NA")
        x = p["t"] - p["t"].iloc[0] if "t" in p else np.arange(len(p)) * 0.2
        ax.plot(x, p["active_cores"], color="#264653", lw=1)
        if "n_running" in p: ax.plot(x, p["n_running"], color="#e76f51", lw=0.8, alpha=0.7)
        ax.set_ylabel(n, rotation=0, ha="right", fontsize=8); ax.set_ylim(0, None); ax.grid(alpha=0.3)
    np.atleast_1d(axs)[-1].set_xlabel("seconds since start (whole process: ingest, calibration, extraction)")
    fig.suptitle("Active cores (dark) and running threads (red), procsample.py, 0.2 s", x=0.01, ha="left", fontsize=10)
    fig.tight_layout(); fig.savefig(out + "_active_cores.png", dpi=150)
    print("figure ->", out + "_active_cores.png")
