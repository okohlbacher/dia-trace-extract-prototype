#!/usr/bin/env python3
"""Restrict a DIALibGen compact Parquet library to the precursors a DIA-NN report identified.

Everything else stays as in the library (predicted RT, fragments, schema, footer metadata); only rows are
dropped. Matching: library Modified.Sequence with modifications renamed to UniMod (as in
lib_to_diann17_tsv.py) + Precursor.Charge == report Modified.Sequence + Precursor.Charge.
Usage: filter_library.py library.parquet report.(tsv|parquet) out.parquet [--q 0.01]
"""
import argparse, re
import pyarrow as pa, pyarrow.compute as pc, pyarrow.csv as pcsv, pyarrow.parquet as pq

UNIMOD = {"Carbamidomethyl": "UniMod:4", "Oxidation": "UniMod:35"}
pat = re.compile(r"\(([^)]*)\)")
def unimod(s): return pat.sub(lambda m: "(" + UNIMOD.get(m.group(1), m.group(1)) + ")", s)

ap = argparse.ArgumentParser()
ap.add_argument("lib"); ap.add_argument("report"); ap.add_argument("out")
ap.add_argument("--q", type=float, default=0.01)
a = ap.parse_args()

cols = ["Modified.Sequence", "Precursor.Charge", "Q.Value"]
rep = (pq.read_table(a.report, columns=cols) if a.report.endswith(".parquet") else
       pcsv.read_csv(a.report, parse_options=pcsv.ParseOptions(delimiter="\t"),
                     convert_options=pcsv.ConvertOptions(include_columns=cols)))
rep = rep.filter(pc.less_equal(rep["Q.Value"], a.q))
keys = set(f"{s}/{z}" for s, z in zip(rep["Modified.Sequence"].to_pylist(), rep["Precursor.Charge"].to_pylist()))

f = pq.ParquetFile(a.lib)
writer, kept = None, 0
for rg in range(f.num_row_groups):
    t = f.read_row_group(rg)
    seq = pc.cast(t["Modified.Sequence"], pa.string()).to_pylist()
    mask = pa.array([f"{unimod(s)}/{z}" in keys for s, z in zip(seq, t["Precursor.Charge"].to_pylist())])
    t = t.filter(mask)
    kept += t.num_rows
    if writer is None:
        writer = pq.ParquetWriter(a.out, f.schema_arrow.with_metadata(f.schema_arrow.metadata), compression="zstd")
    writer.write_table(t)
writer.close()
print(f"{len(keys)} identified precursors (q <= {a.q}); kept {kept} of {f.metadata.num_rows} library precursors -> {a.out}")
if kept < len(keys): print(f"WARNING: {len(keys) - kept} identified precursors not found in the library")
