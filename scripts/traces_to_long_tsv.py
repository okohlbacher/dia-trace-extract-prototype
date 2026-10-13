#!/usr/bin/env python3
"""Reference traces (DIA-NN XICs as list columns, OpenSwathXicToTraces as float32 blobs) -> long TSV
(precursor_id, ms_level, annotation, rt, intensity), unquoted.  usage: traces_to_long_tsv.py IN.parquet OUT.tsv"""
import sys, numpy as np, pyarrow.parquet as pq, pyarrow as pa, pyarrow.csv as pc, pyarrow.compute as cp
src, dst = sys.argv[1], sys.argv[2]
t = pq.read_table(src)
keep = [c for c in ("precursor_id", "ms_level", "annotation") if c in t.column_names]
rt, it = t["rt"], t["intensity"]
if pa.types.is_binary(rt.type):   # OpenSwathXicToTraces: little-endian float32 blobs
    rts = [np.frombuffer(b, "<f4") for b in rt.to_pylist()]; its = [np.frombuffer(b, "<f4") for b in it.to_pylist()]
    n = np.array([len(x) for x in rts]); flat_rt = pa.array(np.concatenate(rts)); flat_it = pa.array(np.concatenate(its))
else:
    n = cp.list_value_length(rt).to_numpy(); flat_rt = cp.list_flatten(rt); flat_it = cp.list_flatten(it)
idx = pa.array(np.repeat(np.arange(len(t)), n))
out = {k: cp.take(t[k], idx) for k in keep}; out["rt"] = flat_rt; out["intensity"] = flat_it
tab = pa.table(out)
with open(dst, "wb") as f:
    f.write(("\t".join(tab.column_names) + "\n").encode())
    with pc.CSVWriter(f, tab.schema, write_options=pc.WriteOptions(delimiter="\t", quoting_style="none", include_header=False)) as w:
        w.write_table(tab)
print("->", dst, tab.num_rows, "rows")
