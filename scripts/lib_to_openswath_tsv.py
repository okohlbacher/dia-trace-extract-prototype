#!/usr/bin/env python3
"""Convert a DIALibGen compact Parquet library to an OpenSWATH assay TSV.

NormalizedRetentionTime = library RT * --rt_scale (a refined library holds observed run minutes: use 60 so
OpenSwathWorkflow, run without iRT calibration, extracts around the observed RT in seconds).
TransitionGroupId = Precursor.Id; TransitionId = "<Precursor.Id>|<annotation>" (annotation as written by
DIATraceExtractor: b7, y10^2, ...), so traces join across tools without guessing.
usage: lib_to_openswath_tsv.py in.parquet out.tsv [--rt_scale 60]
"""
import argparse, re
import pyarrow as pa, pyarrow.compute as pc, pyarrow.parquet as pq, pyarrow.csv as pcsv

TYPES = {1: "a", 2: "b", 3: "c", 4: "x", 5: "y", 6: "z"}
LOSS = {0: "", 1: "-H2O", 2: "-NH3", 3: "-H3PO4", 4: "-HPO3", 5: "-CO", 6: "-loss"}
ap = argparse.ArgumentParser(); ap.add_argument("inp"); ap.add_argument("out")
ap.add_argument("--rt_scale", type=float, default=60.0)
a = ap.parse_args()
f = pq.ParquetFile(a.inp)
with open(a.out, "wb") as sink:
    writer = None
    for rg in range(f.num_row_groups):
        t = f.read_row_group(rg, columns=["Precursor.Id", "Modified.Sequence", "Precursor.Charge", "Decoy", "RT",
                                          "Precursor.Mz", "Product.Mz", "Relative.Intensity", "Fragment.Type",
                                          "Fragment.Charge", "Fragment.Series.Number", "Fragment.Loss.Type"])
        idx = pc.list_parent_indices(t["Product.Mz"])
        rep = lambda c: pc.take(t[c], idx)
        pid = [str(x) for x in rep("Precursor.Id").to_pylist()]
        seq = [str(x) for x in rep("Modified.Sequence").to_pylist()]
        ft = pc.list_flatten(t["Fragment.Type"]).to_pylist()
        fz = pc.list_flatten(t["Fragment.Charge"]).to_pylist()
        fo = pc.list_flatten(t["Fragment.Series.Number"]).to_pylist()
        fl = pc.list_flatten(t["Fragment.Loss.Type"]).to_pylist()
        ann = [f"{TYPES.get(x, '?')}{o}" + (f"^{z}" if z > 1 else "") + LOSS.get(l, "-loss")
               for x, o, z, l in zip(ft, fo, fz, fl)]
        n = len(pid)
        out = pa.table({
            "PrecursorMz": rep("Precursor.Mz"),
            "ProductMz": pc.list_flatten(t["Product.Mz"]),
            "LibraryIntensity": pc.multiply(pc.cast(pc.list_flatten(t["Relative.Intensity"]), pa.float64()), 10000.0),
            "NormalizedRetentionTime": pc.multiply(pc.cast(rep("RT"), pa.float64()), a.rt_scale),
            "PeptideSequence": [re.sub(r"\([^)]*\)", "", s) for s in seq],
            "ModifiedPeptideSequence": seq,
            "PrecursorCharge": rep("Precursor.Charge"),
            "ProductCharge": pa.array(fz, pa.uint8()),
            "FragmentType": [TYPES.get(x, "y") for x in ft],
            "FragmentSeriesNumber": pa.array(fo, pa.uint8()),
            "ProteinId": pa.array(["P"] * n),
            "TransitionGroupId": pid,
            "TransitionId": [p + "|" + an for p, an in zip(pid, ann)],
            "Decoy": rep("Decoy"),
            "DetectingTransition": pa.array([1] * n, pa.int8()),
            "IdentifyingTransition": pa.array([0] * n, pa.int8()),
            "QuantifyingTransition": pa.array([1] * n, pa.int8()),
        })
        if writer is None:
            sink.write(("\t".join(out.schema.names) + "\n").encode())
            writer = pcsv.CSVWriter(sink, out.schema, write_options=pcsv.WriteOptions(
                delimiter="\t", quoting_style="none", include_header=False))
        writer.write_table(out)
    writer.close()
print(f"{f.metadata.num_rows} precursors -> {a.out}")
