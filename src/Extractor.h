// ODIA-V3 trace extractor -- gathers traces from the run grid.
#pragma once

#include "BufferPool.h"
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
    std::size_t block_floats = 0;     ///< trace-buffer block size; plan() counts each unit's blocks (0 = none)
    /// MS1 traces on the MS1 scans themselves (raw values; times on the axes' lane -2) instead of linearly
    /// interpolated onto the global cycle axis.
    bool ms1_scan_axis = true;
  };

  /// One precursor's traces. MS2 on the global cycle axis: cycles [cycle_begin, cycle_begin + cycles); MS1
  /// on [ms1_begin, ms1_begin + ms1_count): MS1 scan indices (ms1_scan_axis) or the same cycle range
  /// (interpolated). `values` holds `fragments` MS2 traces of `cycles` points, then `isotopes` MS1 traces of
  /// `ms1_count` points (trace-major), in the block's arena.
  struct TraceRecord
  {
    std::uint32_t precursor = 0;
    std::int32_t window = -1;
    std::uint32_t cycle_begin = 0, cycles = 0;
    std::uint32_t ms1_begin = 0, ms1_count = 0;
    float rt_center = 0;
    std::uint16_t fragments = 0;
    std::uint8_t isotopes = 0;
    float* values = nullptr;
  };

  /// Output of one work unit: records plus the arena holding their values (recycled pool blocks).
  struct TraceBlock
  {
    std::size_t unit = 0;
    std::vector<TraceRecord> records;
    TraceArena arena;
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
    struct Assignment
    {
      std::uint32_t precursor;   ///< index in the library part
      std::uint32_t window;
      double rt_center;
    };
    struct Unit
    {
      std::size_t begin, end;   ///< range in Plan::assignments
      std::size_t blocks = 0;   ///< trace-buffer blocks its values need (ExtractOptions::block_floats)
    };
    /// Work plan of one library part: precursor-window assignments ordered by (window, part index),
    /// cut into units of up to `unit_precursors` that never straddle a window (a unit touches one lane).
    struct Plan
    {
      std::vector<Assignment> assignments;
      std::vector<Unit> units;
      std::uint64_t precursors = 0, without_window = 0;
    };

    Extractor(const RtMap& rt_map, const RunGrid& grid, const ExtractOptions& opt);

    /// Thread-safe (reads only shared, immutable data).
    Plan plan(const Library& part) const;
    /// Extract a unit into `out`: records are cleared (capacity kept), values go to `out.arena`,
    /// which the caller has reset with the unit's reserved blocks (or none: heap).
    void extract(const Library& part, const Plan& plan, std::size_t unit, TraceBlock& out) const;

    /// Fold a planned part / a finished block into the statistics (from one thread at a time).
    void account(const Plan& plan);
    void account(const std::vector<TraceRecord>& records, std::uint64_t lookups);
    void setTime(const StageTime& t) { stats_.time = t; }
    const ExtractStats& stats() const { return stats_; }

  private:
    std::pair<std::size_t, std::size_t> cycleRange_(double rt_center) const;
    /// MS1 points of a record: scans with times in the window (scan axis) or the cycle range.
    std::pair<std::size_t, std::size_t> ms1Range_(double rt_center, std::size_t lo, std::size_t hi) const;
    float aggregate_(const MzFixed* mz, const float* in, std::size_t a, std::size_t b, MzFixed lo, MzFixed hi,
                     std::size_t& pos) const;

    const RtMap& rt_map_;
    const RunGrid& grid_;
    ExtractOptions opt_;
    ExtractStats stats_;
  };
}
