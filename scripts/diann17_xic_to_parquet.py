#!/usr/bin/env python3
"""Read a DIA-NN 1.7.12 binary .xic file (written with --xic N) into Parquet.

Requires DIA-NN 1.7.12 with the two --xic export fixes (tools/DiaNN-1.7.12/diann-1.7.12-xicfix):
buffer sized before the fragment list was known (heap overflow), and the MS1 intensity block
overwritten by the MS2 RTs. Layout (diann.cpp: XICGroup::write_header / write_values):
  int32 n_groups, int32 header_size, then per group a header:
    int32 N, int32 has_ms1, int64 address, float32 qvalue, float32 pr_mz, int32 run, int32 pr,
    string id (int32 len + bytes), vector<Ion> (int32 n + n * {int8 type, int8 index, int8 loss, int8 charge, float32 mz})
  then per group, at `address`, float32 values:
    [has_ms1] MS1 RT[N] (minutes), MS1 intensity[N] (monoisotope, max centroid within tolerance)
    MS2 RT[N] (minutes), then per fragment: extraction m/z, intensity[N]
Ion.type & 3: 1 = b, 2 = y (fTypeB/fTypeY); index = residues left of the cleavage.
Output: one row per trace, RT in seconds, intensity/rt as float32 lists (pandas-friendly).
"""
import struct, sys
import numpy as np, pyarrow as pa, pyarrow.parquet as pq

src, dst = sys.argv[1], sys.argv[2]
buf = open(src, "rb").read()
n, header_size = struct.unpack_from("<ii", buf, 0)
off = 8
rows = {k: [] for k in ("precursor_id", "qvalue", "ms_level", "annotation", "fragment_type", "fragment_ordinal",
                        "fragment_charge", "fragment_loss", "extraction_mz", "rt", "intensity")}
groups = []
for _ in range(n):
    N, has_ms1, address, q, pr_mz, run, pr = struct.unpack_from("<iiqffii", buf, off); off += 32
    (ln,) = struct.unpack_from("<i", buf, off); off += 4
    pid = buf[off:off + ln].decode(); off += ln
    (nf,) = struct.unpack_from("<i", buf, off); off += 4
    ions = [struct.unpack_from("<bbbbf", buf, off + 8 * k) for k in range(nf)]; off += 8 * nf
    groups.append((N, has_ms1, address, q, pr_mz, pid, ions))

def seq_len(pid):
    # residues of the stripped sequence: drop modifications in parentheses and the trailing charge digit(s)
    s, depth, k = pid.rstrip("0123456789"), 0, 0
    for ch in s:
        if ch == "(": depth += 1
        elif ch == ")": depth -= 1
        elif depth == 0 and ch.isalpha(): k += 1
    return k

# Payload: per group a std::vector<float> as written by write_vector (int32 count + floats), back to
# back from header_size. The stored `address` ignores those 4-byte counts, so walk sequentially.
data = header_size
for N, has_ms1, address, q, pr_mz, pid, ions in groups:
    (count,) = struct.unpack_from("<i", buf, data); data += 4
    v = np.frombuffer(buf, dtype="<f4", count=count, offset=data); data += 4 * count
    expected = 2 * N * has_ms1 + N + (N + 1) * len(ions)
    if count < expected: raise SystemExit(f"{pid}: {count} values < expected {expected} (unpatched DIA-NN?)")
    pos = 0
    def add(level, ann, ftype, ford, fz, floss, mz, rt, inten):
        rows["precursor_id"].append(pid); rows["qvalue"].append(q); rows["ms_level"].append(level)
        rows["annotation"].append(ann); rows["fragment_type"].append(ftype); rows["fragment_ordinal"].append(ford)
        rows["fragment_charge"].append(fz); rows["fragment_loss"].append(floss); rows["extraction_mz"].append(mz)
        rows["rt"].append((rt * 60.0).astype(np.float32)); rows["intensity"].append(inten.astype(np.float32))
    if has_ms1:
        add(1, "M+0", None, None, None, None, pr_mz, v[0:N], v[N:2 * N]); pos = 2 * N
    ms2_rt = v[pos:pos + N]; pos += N
    L = seq_len(pid)
    for t, idx, loss, z, _ in ions:
        fmz = float(v[pos]); inten = v[pos + 1:pos + 1 + N]; pos += N + 1
        typ = "b" if (t & 3) == 1 else "y"
        ordinal = idx if typ == "b" else L - idx
        ann = f"{typ}{ordinal}" + (f"^{z}" if z > 1 else "") + ("" if loss == 0 else f"-loss{loss}")
        add(2, ann, typ, ordinal, z, loss, fmz, ms2_rt, inten)

t = pa.table({k: (pa.array(v, pa.list_(pa.float32())) if k in ("rt", "intensity") else pa.array(v))
              for k, v in rows.items()})
pq.write_table(t, dst, compression="zstd")
print(f"{n} precursor groups, {t.num_rows} traces -> {dst}")
