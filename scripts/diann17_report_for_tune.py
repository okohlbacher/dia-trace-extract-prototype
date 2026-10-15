#!/usr/bin/env python3
"""Make a DIA-NN 1.7.12 report usable by DIALibGen -mode tune.

The tune trainer requires Precursor.Mz (finite, > 0) and a non-empty Protein.Group (it defines the train/val/test
cohorts). A 1.7.12 report searched with a library but without --fasta has no Precursor.Mz column and empty protein
columns. Both are taken from the library that was searched, joined on (Modified.Sequence in UniMod form, charge);
every report row must match. Usage: diann17_report_for_tune.py report.parquet library.parquet out.parquet
"""
import re, sys
import pyarrow as pa, pyarrow.compute as pc, pyarrow.parquet as pq

UNIMOD = {"Carbamidomethyl": "UniMod:4", "Oxidation": "UniMod:35"}
pat = re.compile(r"\(([^)]*)\)")
rep_path, lib_path, out = sys.argv[1:4]
rep = pq.read_table(rep_path)
if "Precursor.Mz" in rep.column_names:
    sys.exit("report already has Precursor.Mz")
want = {(s, int(z)) for s, z in zip(rep["Modified.Sequence"].to_pylist(), rep["Precursor.Charge"].to_pylist())}
mz = {}
f = pq.ParquetFile(lib_path)
for rg in range(f.num_row_groups):
    t = f.read_row_group(rg, columns=["Modified.Sequence", "Precursor.Charge", "Precursor.Mz", "Protein.Group"])
    for s, z, m, g in zip(pc.cast(t["Modified.Sequence"], pa.string()).to_pylist(), t["Precursor.Charge"].to_pylist(),
                          t["Precursor.Mz"].to_pylist(), pc.cast(t["Protein.Group"], pa.string()).to_pylist()):
        k = (pat.sub(lambda mm: "(" + UNIMOD.get(mm.group(1), mm.group(1)) + ")", s), int(z))
        if k in want:
            mz[k] = (m, g)
missing = want - mz.keys()
if missing:
    sys.exit(f"{len(missing)} report precursors not found in the library, e.g. {sorted(missing)[:3]}")
keys = [(s, int(z)) for s, z in zip(rep["Modified.Sequence"].to_pylist(), rep["Precursor.Charge"].to_pylist())]
rep = rep.append_column("Precursor.Mz", pa.array([mz[k][0] for k in keys], pa.float64()))
pg_empty = all(not x for x in rep["Protein.Group"].to_pylist())
if pg_empty:
    rep = rep.set_column(rep.schema.get_field_index("Protein.Group"), "Protein.Group",
                         pa.array([mz[k][1] for k in keys], pa.string()))
pq.write_table(rep, out, compression="zstd")
print(f"{rep.num_rows} rows, all matched; Precursor.Mz added" + (", empty Protein.Group filled" if pg_empty else "") + f" from {lib_path} -> {out}")
