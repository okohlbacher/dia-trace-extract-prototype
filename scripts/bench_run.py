#!/usr/bin/env python3
"""Run one benchmark command with measurement hygiene (parallel-efficiency skill, instrumentation.md §1, §3):

- an exclusive flock around the run (our own jobs never overlap);
- CPU used by OTHER processes during the run (/proc/stat busy jiffies minus our own CPU), plus load average;
- external /proc sampling of the run (procsample.py: active cores, thread states, per-thread context switches);
- optional gdb stack samples of all threads at given seconds after start (poor man's profiler);
- output hash (sha256 over the Parquet parts, sorted by name) for equivalence.

usage: bench_run.py OUT_PREFIX [--stacks 8,12] [--hash DIR] -- command ...
Writes OUT_PREFIX.meta.tsv, OUT_PREFIX.procsample/, OUT_PREFIX.stacks.txt; the command's own stdout/stderr go to
OUT_PREFIX.log.
"""
import argparse, fcntl, glob, hashlib, os, subprocess, sys, time

SKILL = os.path.expanduser("~/.claude/skills/parallel-efficiency/scripts/procsample.py")
ap = argparse.ArgumentParser()
ap.add_argument("prefix"); ap.add_argument("--stacks", default=""); ap.add_argument("--hash", default="")
ap.add_argument("--lock", default=f"/scratch/{os.environ.get('USER','u')}/odia-bench/bench.lock")
argv = sys.argv[1:]
if "--" not in argv: sys.exit("usage: bench_run.py OUT_PREFIX [options] -- command ...")
k = argv.index("--")
a = ap.parse_args(argv[:k])
cmd = argv[k + 1:]

def busy_jiffies():
    v = [int(x) for x in open("/proc/stat").readline().split()[1:]]
    return sum(v) - v[3] - v[4]          # minus idle and iowait

os.makedirs(os.path.dirname(os.path.abspath(a.prefix)), exist_ok=True)
lock = open(a.lock, "w"); fcntl.flock(lock, fcntl.LOCK_EX)
tck = os.sysconf("SC_CLK_TCK")
load0 = open("/proc/loadavg").read().split()[:3]
b0, t0 = busy_jiffies(), time.time()
log = open(a.prefix + ".log", "w")
p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
# the tool re-execs itself once (OMP_WAIT_POLICY); /usr/bin/time stays the parent: sample its child
time.sleep(0.5)
def target_pid(pid):
    try:
        kids = open(f"/proc/{pid}/task/{pid}/children").read().split()
        return int(kids[0]) if kids else pid
    except OSError:
        return pid
tp = target_pid(p.pid)
ps = subprocess.Popen([sys.executable, SKILL, str(tp), a.prefix + ".procsample", "--interval", "0.2"],
                      stdout=subprocess.DEVNULL, stderr=open(a.prefix + ".procsample.err", "w"))
stacks = sorted(float(x) for x in a.stacks.split(",") if x)
with open(a.prefix + ".stacks.txt", "w") as st:
    for s in stacks:
        while time.time() - t0 < s and p.poll() is None: time.sleep(0.05)
        if p.poll() is not None: break
        st.write(f"### sample at {time.time() - t0:.2f} s\n"); st.flush()
        subprocess.run(["gdb", "-p", str(tp), "-batch", "-nx", "-ex", "set pagination off", "-ex", "thread apply all bt 12"],
                       stdout=st, stderr=subprocess.DEVNULL, timeout=120)
rc = p.wait()
t1, b1 = time.time(), busy_jiffies()
ps.wait(timeout=30)
log.close()
txt = open(a.prefix + ".log").read()
def grab(key):
    for line in txt.splitlines():
        if key in line: return line.split(":")[-1].strip()
    return ""
own = float(grab("User time (seconds)") or 0) + float(grab("System time (seconds)") or 0)
wall = t1 - t0
others_cpu = (b1 - b0) / tck - own
h = ""
if a.hash:
    hh = hashlib.sha256()
    for f in sorted(glob.glob(os.path.join(a.hash, "*.parquet"))):
        with open(f, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 22), b""): hh.update(chunk)
    h = hh.hexdigest()[:16]
with open(a.prefix + ".meta.tsv", "w") as m:
    m.write("rc\twall_s\town_cpu_s\tothers_cpu_s\tothers_cores\tload_before\tload_after\toutput_sha256_16\n")
    m.write(f"{rc}\t{wall:.2f}\t{own:.1f}\t{others_cpu:.1f}\t{others_cpu / max(wall, 1e-9):.2f}\t{' '.join(load0)}\t"
            f"{' '.join(open('/proc/loadavg').read().split()[:3])}\t{h}\n")
print(open(a.prefix + ".meta.tsv").read().splitlines()[-1])
sys.exit(rc)
