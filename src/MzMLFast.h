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
    std::vector<MzFixed> mz;              ///< ascending
    std::vector<float> intensity;
    bool was_unsorted = false;
  };

  /// Parses one `<spectrum> ... </spectrum>` element directly from the file bytes: ms level, scan start time,
  /// isolation window (target / lower / upper offset, else selected ion m/z) and the m/z and intensity arrays
  /// (base64; 32/64-bit float; uncompressed or zlib). Everything else, e.g. multi-kilobyte vendor userParams, is
  /// skipped without being interpreted. Returns false for anything outside that subset (numpress, a missing time
  /// unit, missing or mismatched arrays): the caller then parses the chunk with OpenMS instead.
  /// `scratch` is reused across calls (decoded bytes).
  bool parseSpectrumFast(std::string_view xml, GridSpectrum& out, std::vector<unsigned char>& scratch,
                         std::vector<unsigned char>& scratch2);
}
