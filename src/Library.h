// ODIA-V3 trace extractor -- assay library in struct-of-arrays form.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace ODIA
{
  /// Assay library: one entry per precursor, fragments in CSR layout.
  ///
  /// Only what extraction and the export need is kept. Fragments of a precursor
  /// are stored in descending library intensity, so "top N" is a prefix.
  struct Library
  {
    // --- precursors (size P)
    std::vector<std::string> precursor_id;      ///< Precursor.Id / TransitionGroupId (join key)
    std::vector<std::string> modified_sequence;
    std::vector<double> precursor_mz;
    std::vector<std::uint8_t> charge;
    std::vector<std::uint8_t> decoy;
    std::vector<double> library_rt;             ///< as read (iRT, raw 0..1, minutes or seconds)
    std::vector<std::uint32_t> fragment_begin;  ///< size P + 1

    // --- fragments (size F)
    std::vector<double> fragment_mz;
    std::vector<float> fragment_intensity;
    std::vector<char> fragment_type;            ///< 'b', 'y', ... or '?' if unknown
    std::vector<std::uint8_t> fragment_ordinal;
    std::vector<std::uint8_t> fragment_charge;
    std::vector<std::uint8_t> fragment_loss;    ///< 0 = none (DIA-NN/DIALibGen loss code)

    std::size_t size() const { return precursor_mz.size(); }
    std::size_t fragmentCount() const { return fragment_mz.size(); }
    std::uint32_t fragmentsOf(std::size_t p) const { return fragment_begin[p + 1] - fragment_begin[p]; }

    /// Fragment annotation in OpenMS style, e.g. "y7^2" or "b3-H2O" ("?" if unknown).
    std::string annotation(std::size_t f) const;
  };

  struct LibraryLoadOptions
  {
    /// Keep the N most intense fragments per precursor (0 = all).
    std::size_t top_fragments = 6;
    /// Keep only these precursor ids (empty = all).
    std::unordered_set<std::string> keep_ids;
    /// Drop decoys from the library (extraction of targets only).
    bool targets_only = false;
  };

  /// Load a library. Supported:
  ///  - DIA-NN / DIALibGen Parquet (compact: one row per precursor with list columns;
  ///    flat: one row per transition);
  ///  - TSV in DIA-NN or OpenSWATH column dialects (column synonyms resolved);
  ///  - OpenSWATH PQP and TraML via OpenMS loaders.
  void loadLibrary(const std::string& filename, const LibraryLoadOptions& opt, Library& lib);

  /// Read a list of precursor ids (one per line, or the first column of a TSV with header).
  std::unordered_set<std::string> readIdList(const std::string& filename);
}
