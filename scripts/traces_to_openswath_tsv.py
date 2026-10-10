#!/usr/bin/env python3
"""Build an OpenSWATH assay TSV from a DIATraceExtractor traces Parquet, so OpenSwathWorkflow extracts
exactly the same precursors and fragments around exactly the same RT centres.

NormalizedRetentionTime = rt_center (run seconds): run OpenSwathWorkflow without iRT calibration
(identity transformation) and with -rt_extraction_window = 2 x the extractor's half window.
TransitionId = "<precursor_id>|<annotation>" so traces join back without guessing.
"""
import sys
import pyarrow as pa, pyarrow.compute as pc, pyarrow.parquet as pq, pyarrow.csv as pcsv

src, dst = sys.argv[1], sys.argv[2]
cols = ["precursor_id", "modified_sequence", "precursor_charge", "decoy", "precursor_mz", "ms_level", "annotation",
        "fragment_type", "fragment_ordinal", "fragment_charge", "target_mz", "library_intensity", "rt_center"]
t = pq.read_table(src, columns=cols)
t = t.filter(pc.equal(t["ms_level"], 2))
pid = pc.cast(t["precursor_id"], pa.string())
seq = pc.cast(t["modified_sequence"], pa.string())
plain = pc.replace_substring_regex(seq, r"\([^)]*\)", "")
out = pa.table({
    "PrecursorMz": t["precursor_mz"],
    "ProductMz": t["target_mz"],
    "LibraryIntensity": pc.multiply(pc.cast(t["library_intensity"], pa.float64()), 10000.0),
    "NormalizedRetentionTime": t["rt_center"],
    "PeptideSequence": plain,
    "ModifiedPeptideSequence": seq,
    "PrecursorCharge": t["precursor_charge"],
    "ProductCharge": t["fragment_charge"],
    "FragmentType": t["fragment_type"],
    "FragmentSeriesNumber": t["fragment_ordinal"],
    "ProteinId": pa.array(["P"] * t.num_rows),
    "TransitionGroupId": pid,
    "TransitionId": pc.binary_join_element_wise(pid, pc.cast(t["annotation"], pa.string()), "|"),
    "Decoy": t["decoy"],
    "DetectingTransition": pa.array([1] * t.num_rows, pa.int8()),
    "IdentifyingTransition": pa.array([0] * t.num_rows, pa.int8()),
    "QuantifyingTransition": pa.array([1] * t.num_rows, pa.int8()),
})
with open(dst, "wb") as sink:
    sink.write(("\t".join(out.schema.names) + "\n").encode())
    pcsv.write_csv(out, sink, pcsv.WriteOptions(delimiter="\t", quoting_style="none", include_header=False))
print(f"{pc.count_distinct(pid).as_py()} precursors, {out.num_rows} transitions -> {dst}")
