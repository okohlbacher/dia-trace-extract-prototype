#!/usr/bin/env python3
"""Exit 0 iff two DIATraceExtractor outputs (files or part directories) hold identical tables: the same rows in the same
order, values and schema metadata. From V0.2.15 the bytes may differ: the parts' row groups land in the one file in the
order they complete, and the footer lists them in a fixed order (reported for information)."""
import hashlib, os, sys, pyarrow.parquet as pq
def files(p): return sorted(os.path.join(p, f) for f in os.listdir(p)) if os.path.isdir(p) else [p]
def digest(p):
    h = hashlib.sha256()
    for f in files(p): h.update(open(f, "rb").read())
    return h.hexdigest()
a, b = sys.argv[1], sys.argv[2]
ta, tb = pq.read_table(a), pq.read_table(b)
same_bytes, same_table = digest(a) == digest(b), ta.equals(tb)
print("rows", ta.num_rows, tb.num_rows, "identical table:", same_table, "identical bytes:", same_bytes)
sys.exit(0 if same_table else 1)
