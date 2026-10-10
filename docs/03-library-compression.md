# Compacting the predicted library with Parquet (2026-10-10)

Library: DIALibGen 0.11.0 human library, 3,488,457 precursors, 41.7 M fragment values (compact layout:
one row per precursor, fragment lists), 4 row groups. Size and full read time (pyarrow, 32 threads, warm
cache) per variant; scripts were throwaway benchmarks run on rewritten copies of the same table.

## 1. Global string dictionaries (fixed in DIALibGen: https://github.com/okohlbacher/DIALibGen/pull/4)

DIALibGen's compact writer built `Precursor.Id`, `Modified.Sequence` and `Protein.Group` with Arrow
dictionary builders: one global dictionary that Parquet repeats as the dictionary page of every row group.

| layout | size | load in DIATraceExtractor |
|---|---|---|
| current (global dictionaries, 4 row groups) | 569.6 MB | 2.05 s |
| DIALibGen with the PR (real writer, regenerated; identical content) | 457.9 MB (−20%) | 1.19 s |
| plain strings, 4 row groups | 423.6 MB (−26%) | 1.17 s |
| plain strings, 32 row groups | 455.9 MB | 0.79 s |

## 2. Codecs and encodings (on the plain-string layout)

| variant | size | read |
|---|---|---|
| zstd 3, dictionary encoding (Parquet default) | 423.6 MB | 0.40 s |
| zstd 1 / 9 / 19 | 457.3 / 399.5 / 387.5 MB | 0.42 / 0.47 / 0.38 s |
| lz4 / snappy | 548.8 / 532.0 MB | 0.38 / 0.41 s |
| brotli | 394.4 MB | 0.78 s |
| BYTE_STREAM_SPLIT for float columns | 526.9 MB | 0.56 s |
| + DELTA_BYTE_ARRAY strings | 521.6 MB | 0.46 s |
| + DELTA_BINARY_PACKED integers | 529.5 MB | 0.37 s |

The "smarter" encodings lose: fragment m/z values recur across peptides, and Parquet's default dictionary
encoding exploits that better than byte-stream splitting of the float bits. Higher zstd levels buy 6–9% at
higher write cost. Not worth changing the codec.

## 3. Representation (where the bytes are)

Column breakdown of the plain-string file: `Relative.Intensity` 148 MB, `Product.Mz` 137 MB,
`Fragment.Series.Number` 24 MB, `Precursor.Id` 20 MB, everything else together < 100 MB.

| variant | size | change | precision |
|---|---|---|---|
| plain strings (PR) | 423.6 MB | — | lossless |
| `Product.Mz` as uint32 fixed-point (1e-5 Th) | 405.3 MB | −4% | lossless: DIALibGen's m/z are already multiples of 1e-5 Th (max deviation 2e-10 ppm) |
| + `Relative.Intensity` as uint16 (scaled by the library maximum) | 337.8 MB | −20% | ≤ 8.3e-6 absolute; 0.8% relative at intensity 0.001 |
| same, zstd 9 | 326.2 MB | −23% | |
| also without `Product.Mz` and `Precursor.Id` | 199.7 MB | −53% | both recomputable: m/z from sequence + annotation, id = sequence + charge |

## Recommendation

1. Plain string columns (done: DIALibGen PR) — −26% size, ~2× faster loads, no format change for readers.
2. If a smaller library matters more than strict DIA-NN-dialect column types: a "compact-v2" layout with
   fixed-point uint32 m/z (lossless for DIALibGen output) and uint16 intensities (−20%). Readers expecting
   `Product.Mz` as double (DIA-NN, OpenSWATH converters) would need a conversion step, so keep it opt-in.
3. Dropping derivable columns halves the file but moves fragment-mass computation into every reader; only
   worthwhile for a library format private to ODIA tools.
4. Codec changes (zstd level, brotli) and alternative encodings are not worth it.
