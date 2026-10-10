#!/usr/bin/env python3
"""Centroid-level re-extraction for the trace-quality study (results/analysis/quality/).

For a fixed random sample of identified precursors (present in DIA-NN's XIC export and in OpenSWATH), records
EVERY centroid within +-WIDE ppm of each library target at every point of every trace, so that any extraction
rule (window centre, width, max/sum) can be emulated offline:
  MS2: every library fragment, in the precursor's isolation-window lane, at the cycles of ours-A's trace
  MS1: M+0, M+1, M+2 (charge spacing) at the MS1 scans bracketing that RT range
The trace geometry (window, cycle range) is taken from ours A (odiaA.parquet / axes) so that the sample lives
on exactly the same spectra as the exported traces. Spectra are read straight from the (uncompressed,
indexed) mzML: byte offsets from <indexList>, 16 worker processes, base64 m/z float64 + intensity float32.

Output (OUT_DIR): queries.parquet (qid, precursor_id, annotation, ms_level, target_mz, lane, c0, c1,
library_intensity, ...) and centroids.parquet (qid, pos = cycle (MS2) or MS1 scan row, mz, intensity).
usage: quality_reextract.py RUN_DIR MZML OUT_DIR [--n 3000] [--seed 20261011] [--wide 25] [--threads 16]
"""
import argparse, base64, mmap, os, re
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS"): os.environ.setdefault(_v, "1")
from multiprocessing import Pool
import numpy as np, pandas as pd, pyarrow as pa, pyarrow.parquet as pq

C13 = 1.0033548378
ap = argparse.ArgumentParser()
ap.add_argument("run_dir"); ap.add_argument("mzml"); ap.add_argument("out_dir")
ap.add_argument("--n", type=int, default=3000); ap.add_argument("--seed", type=int, default=20261011)
ap.add_argument("--wide", type=float, default=25.0); ap.add_argument("--threads", type=int, default=16)
a = ap.parse_args()
os.makedirs(a.out_dir, exist_ok=True)
R = a.run_dir
UNI = {"UniMod:4": "Carbamidomethyl", "UniMod:35": "Oxidation"}
def from_unimod(s):
    for k, v in UNI.items(): s = s.replace(k, v)
    return s

# ---------------------------------------------------------------- sample and queries
o = pq.read_table(R + "/odiaA.parquet", columns=["precursor_id", "precursor_mz", "precursor_charge", "ms_level",
                  "annotation", "fragment_charge", "target_mz", "library_intensity", "window", "cycle_begin",
                  "rt_center", "intensity"]).to_pandas()
o["n"] = o.intensity.map(len) // 4; o = o.drop(columns="intensity")
dn = pq.read_table(R + "/diann/diann_xic.parquet", columns=["precursor_id", "ms_level", "qvalue"]).to_pandas()
dn = dn[dn.ms_level == 1]; dn["precursor_id"] = dn.precursor_id.map(from_unimod)
osw = set(pq.read_table(R + "/osw_traces.parquet", columns=["precursor_id"]).column(0).to_pylist())
pool = sorted(set(dn.precursor_id) & osw & set(o.precursor_id))
rng = np.random.default_rng(a.seed)
sample = set(np.array(pool)[rng.choice(len(pool), min(a.n, len(pool)), replace=False)])
q = o[o.precursor_id.isin(sample)].reset_index(drop=True)
q = q.merge(dn[["precursor_id", "qvalue"]], on="precursor_id", how="left")
axes = pq.read_table(R + "/odiaA.axes.parquet").to_pandas()
cycle_rt = axes[axes.lane == -1].sort_values("cycle").rt.to_numpy()
ms1 = axes[axes.lane == -2].sort_values("cycle")
ms1_rt = ms1.rt.to_numpy()
q["c0"] = q.cycle_begin.astype(np.int64); q["c1"] = q.c0 + q.n
q["lane"] = np.where(q.ms_level == 2, q.window, -2)
# MS1: isotopes at charge spacing; positions = MS1 scan rows bracketing the cycle range
is1 = q.ms_level == 1
iso = q.annotation.str.extract(r"M\+(\d)")[0].astype(float)
q.loc[is1, "target_mz"] = q.precursor_mz[is1] + iso[is1] * C13 / q.precursor_charge[is1]
t0 = cycle_rt[q.c0[is1]]; t1 = cycle_rt[q.c1[is1] - 1]
j0 = np.maximum(np.searchsorted(ms1_rt, t0, "right") - 1, 0); j1 = np.minimum(np.searchsorted(ms1_rt, t1, "left"), len(ms1_rt) - 1)
q.loc[is1, "c0"] = j0; q.loc[is1, "c1"] = j1 + 1
q["qid"] = np.arange(len(q))
q.drop(columns=["cycle_begin", "n"]).to_parquet(a.out_dir + "/queries.parquet", index=False)
print(f"{len(sample)} precursors (seed {a.seed}, from {len(pool)}), {len(q)} queries")

# spectrum index -> (lane, position)
smap = {}
for lane, cyc, sp in axes[axes.lane >= 0][["lane", "cycle", "spectrum"]].itertuples(index=False): smap[int(sp)] = (int(lane), int(cyc))
for cyc, sp in ms1[["cycle", "spectrum"]].itertuples(index=False): smap[int(sp)] = (-2, int(cyc))
lanes = {}
for lane, g in q.groupby("lane"):
    g = g.sort_values("target_mz")
    w = g.target_mz.to_numpy() * a.wide * 1e-6
    lanes[int(lane)] = (g.qid.to_numpy(), (g.target_mz - w).to_numpy(), (g.target_mz + w).to_numpy(),
                        g.c0.to_numpy(), g.c1.to_numpy())

# ---------------------------------------------------------------- mzML byte offsets
size = os.path.getsize(a.mzml)
with open(a.mzml, "rb") as fh:
    fh.seek(max(0, size - 64 * 2**20)); tail = fh.read()
s0 = tail.index(b'<index name="spectrum">'); s1 = tail.index(b"</index>", s0)
offs = np.array([int(x) for x in re.findall(rb">(\d+)</offset>", tail[s0:s1])], dtype=np.int64)
end_spectra = size - len(tail) + s1  # beyond the last spectrum
print(f"{len(offs)} spectra indexed")

SPEC = re.compile(rb'<spectrum [^>]*?index="(\d+)".*?<binary>([^<]*)</binary>.*?<binary>([^<]*)</binary>', re.S)
def work(rng_):
    i0, i1 = rng_
    lo = int(offs[i0]); hi = int(offs[i1]) if i1 < len(offs) else end_spectra
    out_q, out_p, out_mz, out_in = [], [], [], []
    with open(a.mzml, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        buf = mm[lo:hi]
    for m in SPEC.finditer(buf):
        idx = int(m.group(1))
        if idx not in smap: continue
        lane, pos = smap[idx]
        if lane not in lanes: continue
        qid, qlo, qhi, c0, c1 = lanes[lane]
        act = (c0 <= pos) & (pos < c1)
        if not act.any(): continue
        mz = np.frombuffer(base64.b64decode(m.group(2)), "<f8")
        it = np.frombuffer(base64.b64decode(m.group(3)), "<f4")
        a_ = np.searchsorted(mz, qlo[act], "left"); b_ = np.searchsorted(mz, qhi[act], "right")
        for k in np.nonzero(b_ > a_)[0]:
            sl = slice(a_[k], b_[k]); n = b_[k] - a_[k]
            out_q.append(np.full(n, qid[act][k], np.int32)); out_p.append(np.full(n, pos, np.int32))
            out_mz.append(mz[sl]); out_in.append(it[sl].astype(np.float32))
    cat = lambda L, t: np.concatenate(L) if L else np.zeros(0, t)
    return cat(out_q, np.int32), cat(out_p, np.int32), cat(out_mz, np.float64), cat(out_in, np.float32)

chunks = np.linspace(0, len(offs), a.threads * 4 + 1).astype(int)
with Pool(a.threads) as p:
    res = p.map(work, list(zip(chunks[:-1], chunks[1:])))
cols = [np.concatenate([r[i] for r in res]) for i in range(4)]
t = pa.table({"qid": cols[0], "pos": cols[1], "mz": cols[2], "intensity": cols[3]})
pq.write_table(t, a.out_dir + "/centroids.parquet", compression="zstd")
print(f"{t.num_rows} centroids -> {a.out_dir}/centroids.parquet")
