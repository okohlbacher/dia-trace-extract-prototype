#!/usr/bin/env python3
"""Exit 0 iff two DIATraceExtractor Parquet files hold identical tables."""
import sys, pyarrow.parquet as pq
a, b = pq.read_table(sys.argv[1]), pq.read_table(sys.argv[2])
print("rows", a.num_rows, b.num_rows, "identical:", a.equals(b))
sys.exit(0 if a.equals(b) else 1)
