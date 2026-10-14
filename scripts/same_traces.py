#!/usr/bin/env python3
"""Are two trace outputs (Parquet directories or files from DIATraceExtractor) the same extraction?
Compares the rows (order-independent: sorted on the descriptive columns) including the intensity blobs bit for
bit, and the odia.* metadata except the input and library paths. Prints one line; exit 0 when identical.
usage: same_traces.py NEW OLD"""
import sys
import pyarrow.parquet as pq

new, old = sys.argv[1], sys.argv[2]
a, b = pq.read_table(new), pq.read_table(old)
skip = {b"odia.input", b"odia.library"}
ma = {k: v for k, v in (a.schema.metadata or {}).items() if k not in skip}
mb = {k: v for k, v in (b.schema.metadata or {}).items() if k not in skip}
meta = sorted(k.decode() for k in set(ma) | set(mb) if ma.get(k) != mb.get(k))
a, b = a.replace_schema_metadata(None), b.replace_schema_metadata(None)
if a.schema != b.schema:
    print(f"DIFFERENT schema: {a.schema.names} vs {b.schema.names}"); sys.exit(1)
keys = [(n, "ascending") for n in a.schema.names if n not in ("intensity", "rt")]
a, b = a.sort_by(keys), b.sort_by(keys)
rows = f"{a.num_rows} vs {b.num_rows} rows"
if a.num_rows != b.num_rows:
    print(f"DIFFERENT {rows}; metadata differs in {meta}"); sys.exit(1)
cols = [n for n in a.schema.names if not a.column(n).equals(b.column(n))]
if cols or meta:
    print(f"DIFFERENT ({rows}): columns {cols}, metadata {meta}"); sys.exit(1)
print(f"identical ({a.num_rows} rows, {len(a.schema.names)} columns, {len(ma)} metadata keys)")
