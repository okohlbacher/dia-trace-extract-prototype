// ODIA-V3 trace extractor -- gathers traces from the run grid.
#pragma once

#include "Library.h"
#include "RtMap.h"
#include "RunGrid.h"

#include <cstdint>
#include <vector>

namespace ODIA
{
  struct ExtractOptions
  {
    double ms2_ppm = 10.0;            ///< fragment tolerance, +- ppm (half width)
    double ms1_ppm = 10.0;            ///< precursor/isotope tolerance, +- ppm (half width)
    double ms2_ppm_offset = 0.0;      ///< mass correction: target * (1 + offset * 1e-6)
    double ms1_ppm_offset = 0.0;
    double rt_half_window = 60.0;     ///< +- seconds around the mapped library RT; <= 0: whole run
    bool aggregate_sum = true;        ///< sum all centroids in tolerance (OpenSWATH) or take the max (DIA-NN)
    bool extract_ms1 = true;
    int ms1_isotopes = 2;             ///< isotopes after the monoisotope (M+1 .. M+n)
    bool isotope_unit_spacing = false; ///< OpenSWATH places isotope k at mz + k*1.00336 (no /z); default divides by z
    bool assign_all_windows = false;  ///< false: nearest window centre only; true: every window containing it
    int threads = 1;                  ///< extraction threads (explicit; never inherited)
    std::size_t unit_precursors = 256; ///< precursors per work unit
    std::size_t max_units_ahead = 0;  ///< back-pressure for the ordered commit (0 = 4 x threads)
  };

  /// One precursor's traces on the global cycle axis: cycles [cycle_begin, cycle_begin + cycles).
  /// values[value_offset ...] holds `fragments` MS2 traces then `isotopes` MS1 traces, each
  /// `cycles` long (trace-major).
  struct TraceRecord
  {
    std::uint32_t precursor = 0;
    std::int32_t window = -1;
    std::uint32_t cycle_begin = 0, cycles = 0;
    float rt_center = 0;
    std::uint16_t fragments = 0;
    std::uint8_t isotopes = 0;
    std::uint64_t value_offset = 0;
  };

  /// Output of one work unit: records plus one value arena (no per-trace allocation).
  struct TraceBlock
  {
    std::size_t unit = 0;
    std::vector<TraceRecord> records;
    std::vector<float> values;
    std::uint64_t lookups = 0;   ///< peak-array searches spent on this block
  };

  struct ExtractStats
  {
    std::uint64_t precursors = 0, without_window = 0, assignments = 0, empty_range = 0;
    std::uint64_t units = 0, traces = 0, ms2_points = 0, ms1_points = 0;
    std::uint64_t peak_lookups = 0;
    StageTime time;
  };

  class Extractor
  {
  public:
    Extractor(const Library& lib, const RtMap& rt_map, const RunGrid& grid, const ExtractOptions& opt);

    /// Work units: runs of up to `unit_precursors` precursors of one isolation window, ordered
    /// by (window, library index). extract(u) is thread-safe (reads only shared, immutable data).
    std::size_t units() const { return units_.size(); }
    TraceBlock extract(std::size_t unit) const;

    /// Fold a finished block into the statistics (call once per block, from one thread).
    void account(const std::vector<TraceRecord>& records, std::uint64_t lookups);
    void setTime(const StageTime& t) { stats_.time = t; }
    const ExtractStats& stats() const { return stats_; }

  private:
    struct Assignment
    {
      std::uint32_t precursor;
      std::uint32_t window;
      double rt_center;
    };
    struct Unit
    {
      std::size_t begin, end;   ///< range in assignments_
    };

    float aggregate_(const MzFixed* mz, const float* in, std::size_t a, std::size_t b, MzFixed lo, MzFixed hi,
                     std::size_t& pos) const;

    const Library& lib_;
    const RunGrid& grid_;
    ExtractOptions opt_;
    std::vector<Assignment> assignments_;   ///< ordered by (window, library index)
    std::vector<Unit> units_;
    ExtractStats stats_;
  };
}
