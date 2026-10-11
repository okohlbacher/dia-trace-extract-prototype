// ODIA-V3 trace extractor -- minimal mzML <spectrum> parser for the run grid.
#pragma once

#include "RunGrid.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace ODIA
{
  /// What the run grid needs from one spectrum.
  struct GridSpectrum
  {
    std::uint32_t index = 0;
    double rt = 0;                        ///< seconds
    int ms_level = 0;
    double target = 0, lower = 0, upper = 0;   ///< isolation window (MS2+)
    std::uint32_t n = 0;                  ///< peaks
    std::vector<MzFixed> mz;              ///< ascending (empty when in_pool)
    std::vector<float> intensity;
    bool was_unsorted = false;
    bool in_pool = false;                 ///< the peaks were decoded straight into the run's PeakPool at pool_begin
    std::uint64_t pool_begin = 0;
  };

  /// The two binary arrays of a spectrum, located by parseSpectrumHeader (views into the XML).
  struct SpectrumArrays
  {
    std::string_view binary[2];
    int kind[2] = {0, 0};                 ///< 1 = m/z, 2 = intensity
    int bits[2] = {0, 0};
    bool zlib[2] = {false, false};
  };

  /// Parses one `<spectrum> ... </spectrum>` element directly from the file bytes: ms level, scan start time,
  /// isolation window (target / lower / upper offset, else selected ion m/z) and the m/z and intensity arrays
  /// (base64; 32/64-bit float; uncompressed or zlib). Everything else, e.g. multi-kilobyte vendor userParams, is
  /// skipped without being interpreted. Returns false for anything outside that subset (numpress, a missing time
  /// unit, missing or mismatched arrays): the caller then parses the chunk with OpenMS instead.
  /// `scratch` is reused across calls (decoded bytes).
  bool parseSpectrumFast(std::string_view xml, GridSpectrum& out, std::vector<unsigned char>& scratch,
                         std::vector<unsigned char>& scratch2);

  /// The metadata and peak count (out.n, from defaultArrayLength) of one spectrum and the location of its arrays,
  /// without decoding them; false outside the supported subset (see parseSpectrumFast).
  bool parseSpectrumHeader(std::string_view xml, GridSpectrum& out, SpectrumArrays& arrays);
  /// Decode the arrays into `mz` / `intensity` (n each), sorting by m/z in place if needed; false if the
  /// decoded length differs from n or the data is invalid.
  bool decodeSpectrumArrays(const SpectrumArrays& arrays, std::size_t n, MzFixed* mz, float* intensity, bool& was_unsorted,
                            std::vector<unsigned char>& scratch, std::vector<unsigned char>& scratch2);
}
