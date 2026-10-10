# Handoff: requests to mzpeak-convert (okohlbacher/mzPeakConverter) from the ODIA-V3 trace extractor (2026-10-10)

**Requester:** `DIATraceExtractor`, an OpenMS TOPP tool that extracts DIA fragment and MS1 traces
(repo okohlbacher/dia-trace-extract-prototype). It reads mzPeak files written by mzpeak-convert, through
the OpenMS-mzPeakRW reader. Today its input is a vendor-centroided mzML.

## Example data (Thermo Orbitrap Astral, 15 min DIA, ProteoBench entrapment benchmark)

| what | path |
|---|---|
| Thermo raw (input) | `/ceph/ibmi/abi/oliver/Claude/ODIA-V3/benchmark-data/DIA-astral-entrapment/raw/LFQ_Astral_DIA_15min_50ng_Human_01.raw` |
| mzPeak from **v0.17.2**, default options | `/ceph/ibmi/abi/oliver/Claude/ODIA-V3/benchmark-data/DIA-astral-entrapment/mzpeak-0.17.2/LFQ_Astral_DIA_15min_50ng_Human_01.mzpeak` (conversion log: `convert_01.log` next to it) |
| reference mzML, centroided (OpenMS `FileConverter`, Thermo RAW reader, OpenMS develop 2026-10-09) | `/ceph/ibmi/abi/oliver/Claude/ODIA-V3/benchmark-data/DIA-astral-entrapment/mzML/LFQ_Astral_DIA_15min_50ng_Human_01.mzML` |

Runs `_02` and `_03` are in the same directories. Command used:
`mzpeak-convert-0.17.2 <raw> -o <mzpeak>`, with a .NET 8 runtime.

## What the v0.17.2 file contains, compared with the reference

| | v0.17.2 mzPeak | reference mzML |
|---|---|---|
| MS2 | 292,883 spectra, centroid, 133,484,467 peaks (`spectra_peaks`), 23 empty | identical counts |
| MS1 | 2,928 spectra, **profile only**, 65,714,146 points (`spectra_data`, zero-run-masked from 103 M) | 2,928 spectra, **centroid**, 7,501,972 peaks |

Everything we need for MS2 is there. In particular, the m/z error is well within what we need
(fidelity block: numpress-linear ≤ 0.0022 ppm). **MS1 is the problem.**

## Requests

### 1. Centroided MS1 for Thermo `.raw` (required)
**Check done:** v0.17.2 has no option for this. `--representation` is honoured only by the Shimadzu `.lcd`
and Bruker BAF readers, and nothing else in `--help` or the USER_MANUAL centroids or peak-picks. For
Thermo MS1 the archive therefore holds only profile data.

**Request:** write the vendor's centroids for Thermo scans that carry profile data, i.e. the
RawFileReader centroid stream (`GetCentroidStream` / label data for FT and Astral scans; for scans
without one, the vendor's centroiding as msconvert `peakPicking vendor` uses it). Put them in
`spectra_peaks` and set `number_of_peaks` in the metadata row, the same way the Shimadzu lane already does.
- **Default `--representation both`:** profile goes to `spectra_data` and centroids to `spectra_peaks`.
- **`--representation centroid`:** only centroids. This is what we would run: the MS1 profile facet is
  255 MB of the 1.29 GB archive.
- **`--representation profile`:** today's behaviour.
- **Per-level choice:** if possible, choose per MS level (e.g. `--representation centroid` applied to
  MS1, or a `--centroid-levels 1-` style option). Astral MS2 is already centroid-only.
- **Provenance:** record the vendor peak picking in `data_processing_method_list` and in
  `transformations` / `fidelity`, so a reader can tell vendor centroids from profile data.

**Acceptance on run 01:**
- 2,928 MS1 spectra with 7,501,972 peaks in total;
- per spectrum, m/z and intensity equal to the reference mzML within the stored precision (the
  `fidelity` bound);
- MS2 unchanged.

If the reference mzML's peak list was produced differently from the RawFileReader centroid stream
(OpenMS reads Thermo through the same RawFileReader), please report the per-spectrum differences rather
than forcing a match.

### 2. Row groups never split a spectrum, stated in the archive (required for parallel reading)
We will decode signal row groups in parallel, each thread on its own row groups. That only works if every
row group holds complete spectra.
- **Run 01 already satisfies this:** `spectrum_index` ranges of consecutive row groups are disjoint in
  both facets, and the min/max statistics are present.
- **Missing:** the manual does not promise it.

**Request:**
- guarantee that no spectrum's chunks are split across row groups, for chunked and point layouts and for
  all lanes;
- declare it in `mzpeak_index.json`, for example `"row_groups": "spectrum-aligned"` per facet;
- keep writing the `spectrum_index` min/max statistics for every row group.

A reader can then partition by row group without checking for a carried-over spectrum.

### 3. Enough row groups for parallel reads (nice to have)
- **Current counts:** run 01 has 16 row groups in `spectra_data` and 28 in `spectra_peaks`. With 32+
  threads, MS1 is the bottleneck.
- **Current controls:** `MZPC_ROW_GROUP_ROWS` / `MZPC_ROW_GROUP_MB` already exist, but only as
  environment variables.

**Request:**
- a CLI option (e.g. `--row-group-rows`, `--row-group-mb`);
- a note in the manual on the size cost of smaller row groups;
- for Astral DIA, the size cost of about 128 row groups per signal facet: is it negligible?

### 4. Keep as is (confirmations, no change wanted)
- **Empty spectra:** spectra with 0 peaks (23 MS2 in run 01) stay in the metadata tables with their
  precursor/isolation window. We rely on them for DIA cycle detection.
- **Uncompressed storage:** all ZIP entries stay stored, not deflated, so a reader can memory-map entries
  at their offset.
- **Isolation windows:** target plus lower/upper offsets for every MS2 in
  `spectra_metadata_precursors.parquet`.
- **Time:** in seconds, as declared in the column mapping.

## Contact point

The reader side, a parallel partitioned API in OpenMS-mzPeakRW, is specified separately in
`/ceph/ibmi/abi/oliver/Claude/ODIA-V3/docs/04-mzpeak-reader-handoff.md`. Request 2 above is the guarantee
that reader design relies on.
