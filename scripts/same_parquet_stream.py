#!/usr/bin/env python3
"""Exit 0 iff two Parquet files hold the same rows in the same order (batch by batch, so tables of any size): the
traces of two extractions with the same library and options, whose row order does not depend on the thread count.
Schema metadata keys that differ are reported, not counted. usage: same_parquet_stream.py A.parquet B.parquet"""
import sys, pyarrow.parquet as pq
a, b = pq.ParquetFile(sys.argv[1]), pq.ParquetFile(sys.argv[2])
if a.metadata.num_rows != b.metadata.num_rows:
    sys.exit(f"DIFFERENT row counts: {a.metadata.num_rows} vs {b.metadata.num_rows}")
ma, mb = a.schema_arrow.metadata or {}, b.schema_arrow.metadata or {}
meta = sorted(k.decode() for k in set(ma) | set(mb) if ma.get(k) != mb.get(k))
rows = 0
for x, y in zip(a.iter_batches(batch_size=1 << 20), b.iter_batches(batch_size=1 << 20)):
    if not x.equals(y):
        cols = [n for n in x.schema.names if not x.column(n).equals(y.column(n))]
        sys.exit(f"DIFFERENT after row {rows}: columns {cols}")
    rows += x.num_rows
print(f"identical ({rows} rows{'; metadata keys differing: ' + ', '.join(meta) if meta else ''})")
