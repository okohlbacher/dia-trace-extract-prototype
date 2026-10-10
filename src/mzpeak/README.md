# Vendored mzPeak reader

`MzPeakFile.h/.cpp` are taken from https://github.com/okohlbacher/OpenMS-mzPeakRW (OpenMS fork with
mzPeak read/write support; commit 0525e34, BSD-3-Clause, © OpenMS Inc.), unmodified except for the port to
the OpenMS develop (3.7) API: `OpenMS::String` was removed upstream, so it is replaced by `std::string`
plus two small helpers (`toStr_`, `replaceAll_`) at the top of the .cpp. Any further change is marked
`// odia:` in the source.
