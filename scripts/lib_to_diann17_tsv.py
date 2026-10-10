#!/usr/bin/env python3
"""Convert a DIALibGen compact Parquet library to a DIA-NN 1.7.12 TSV spectral library.

Modifications are renamed to UniMod (Carbamidomethyl -> UniMod:4, Oxidation -> UniMod:35); the RT
column (DIALibGen raw 0..1 model output) is written as iRT * --rt_scale (DIA-NN fits its own RT map).
Fragments: all stored ones (DIALibGen keeps 3..12 by intensity). Usage:
  lib_to_diann17_tsv.py in.parquet out.tsv [--ids ids.txt] [--rt_scale 100]
"""
import argparse, re
import pyarrow as pa, pyarrow.compute as pc, pyarrow.parquet as pq, pyarrow.csv as pcsv

UNIMOD = {"Carbamidomethyl": "UniMod:4", "Oxidation": "UniMod:35"}
TYPES = {1: "a", 2: "b", 3: "c", 4: "x", 5: "y", 6: "z"}
LOSS = {0: "noloss", 1: "H2O", 2: "NH3"}

ap = argparse.ArgumentParser()
ap.add_argument("inp"); ap.add_argument("out")
ap.add_argument("--ids"); ap.add_argument("--rt_scale", type=float, default=100.0)
a = ap.parse_args()
keep = set(open(a.ids).read().split()) if a.ids else None
pat = re.compile(r"\(([^)]*)\)")
def unimod(s): return pat.sub(lambda m: "(" + UNIMOD.get(m.group(1), m.group(1)) + ")", s)

f = pq.ParquetFile(a.inp)
writer = None
for rg in range(f.num_row_groups):
    t = f.read_row_group(rg, columns=["Precursor.Id", "Modified.Sequence", "Protein.Group", "Precursor.Charge",
                                      "RT", "Precursor.Mz", "Product.Mz", "Relative.Intensity", "Fragment.Type",
                                      "Fragment.Charge", "Fragment.Series.Number", "Fragment.Loss.Type"])
    for c in ("Precursor.Id", "Modified.Sequence", "Protein.Group"):
        t = t.set_column(t.schema.get_field_index(c), c, pc.cast(t[c], pa.string()))
    if keep is not None:
        t = t.filter(pc.is_in(t["Precursor.Id"], pa.array(sorted(keep))))
    n = pc.list_value_length(t["Product.Mz"])
    idx = pc.list_parent_indices(t["Product.Mz"])
    rep = lambda col: pc.take(t[col], idx)
    seq = [unimod(s) for s in rep("Modified.Sequence").to_pylist()]
    ft = [TYPES.get(x, "y") for x in pc.list_flatten(t["Fragment.Type"]).to_pylist()]
    fl = [LOSS.get(x, "noloss") for x in pc.list_flatten(t["Fragment.Loss.Type"]).to_pylist()]
    out = pa.table({
        "ModifiedPeptide": seq,
        "PrecursorCharge": rep("Precursor.Charge"),
        "PrecursorMz": rep("Precursor.Mz"),
        "iRT": pc.multiply(pc.cast(rep("RT"), pa.float64()), a.rt_scale),
        "ProductMz": pc.list_flatten(t["Product.Mz"]),
        "LibraryIntensity": pc.list_flatten(t["Relative.Intensity"]),
        "ProteinName": rep("Protein.Group"),
        "FragmentCharge": pc.list_flatten(t["Fragment.Charge"]),
        "FragmentType": ft,
        "FragmentSeriesNumber": pc.list_flatten(t["Fragment.Series.Number"]),
        "FragmentLossType": fl,
        "Decoy": pa.array([0] * len(seq), pa.int8()),
    })
    if writer is None:
        # DIA-NN 1.7.12 matches most column names only unquoted: write the header ourselves.
        sink = open(a.out, "wb")
        sink.write(("\t".join(out.schema.names) + "\n").encode())
        writer = pcsv.CSVWriter(sink, out.schema, write_options=pcsv.WriteOptions(
            delimiter="\t", quoting_style="none", include_header=False))
    writer.write_table(out)
    print(f"row group {rg}: {t.num_rows} precursors, {out.num_rows} transitions", flush=True)
writer.close()
sink.close()
