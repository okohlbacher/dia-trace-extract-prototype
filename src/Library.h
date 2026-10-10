// ODIA-V3 trace extractor -- assay library in struct-of-arrays form.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ODIA
{
  /// Many short strings in one buffer (one allocation instead of one per string).
  class StringColumn
  {
  public:
    std::string_view operator[](std::size_t i) const
    {
      return std::string_view(data_).substr(offset_[i], offset_[i + 1] - offset_[i]);
    }
    std::size_t size() const { return offset_.size() - 1; }
    void push_back(std::string_view s)
    {
      data_.append(s);
      offset_.push_back(data_.size());
    }
    void append(const StringColumn& o)
    {
      const std::uint64_t base = data_.size();
      data_ += o.data_;
      for (std::size_t i = 1; i < o.offset_.size(); ++i) { offset_.push_back(base + o.offset_[i]); }
    }
    void reserve(std::size_t n, std::size_t bytes) { offset_.reserve(n + 1); data_.reserve(bytes); }
    std::size_t bytes() const { return data_.size(); }
    /// Concatenate columns in order, copying the parts concurrently.
    static StringColumn concat(const std::vector<const StringColumn*>& parts, int threads);

  private:
    std::string data_;
    std::vector<std::uint64_t> offset_{0};
  };

  /// Assay library: one entry per precursor, fragments in CSR layout.
  ///
  /// Only what extraction and the export need is kept. Fragments of a precursor
  /// are stored in descending library intensity, so "top N" is a prefix.
  struct Library
  {
    // --- precursors (size P)
    StringColumn precursor_id;                  ///< Precursor.Id / TransitionGroupId (join key)
    StringColumn modified_sequence;
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

    /// Append another library (fragment offsets are rebased).
    void append(const Library& o);
    /// Concatenate libraries in order; the copies run concurrently.
    static Library concat(const std::vector<Library>& parts, int threads);
  };

  struct LibraryLoadOptions
  {
    /// Keep the N most intense fragments per precursor (0 = all).
    std::size_t top_fragments = 6;
    /// Keep only these precursor ids (empty = all).
    std::unordered_set<std::string> keep_ids;
    /// Drop decoys from the library (extraction of targets only).
    bool targets_only = false;
    /// Threads for parsing (compact Parquet libraries are parsed in parallel row chunks).
    int threads = 1;
  };

  /// Load a library. Supported:
  ///  - DIA-NN / DIALibGen Parquet (compact: one row per precursor with list columns;
  ///    flat: one row per transition);
  ///  - TSV in DIA-NN or OpenSWATH column dialects (column synonyms resolved);
  ///  - OpenSWATH PQP and TraML via OpenMS loaders.
  /// The result is identical for any thread count.
  void loadLibrary(const std::string& filename, const LibraryLoadOptions& opt, Library& lib);

  /// Read a list of precursor ids (one per line, or the first column of a TSV with header).
  std::unordered_set<std::string> readIdList(const std::string& filename);
}
