#!/usr/bin/env python3
"""DIA-NN 1.7.12 writes report.tsv only; DIALibGen's refine/tune read report.parquet. Convert.

1.7.12 reports contain target identifications only and have no Decoy column; one is added (all 0), which
lets DIALibGen apply its q-value gates instead of bypassing them as for an empirical library.
"""
import sys, pyarrow as pa, pyarrow.csv as pcsv, pyarrow.parquet as pq
t = pcsv.read_csv(sys.argv[1], parse_options=pcsv.ParseOptions(delimiter="\t"))
if "Decoy" not in t.column_names:
    t = t.append_column("Decoy", pa.array([0] * t.num_rows, pa.int8()))
pq.write_table(t, sys.argv[2], compression="zstd")
print(f"{t.num_rows} rows, {t.num_columns} columns -> {sys.argv[2]}")
