// ODIA-V3 trace extractor -- compact grid representation of a DIA run on ONE global RT axis.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ODIA
{
  /// m/z in fixed point: uint32 in units of 1e-5 Th (0 .. 42,949.67 Th). At 1,000 m/z one unit
  /// is 0.01 ppm, far below any extraction tolerance.
  using MzFixed = std::uint32_t;
  inline constexpr double MZ_FIXED_SCALE = 1e5;
  inline MzFixed toFixed(double mz) { return static_cast<MzFixed>(std::lround(mz * MZ_FIXED_SCALE)); }
  inline double fromFixed(MzFixed m) { return static_cast<double>(m) / MZ_FIXED_SCALE; }

  /// Peaks of many spectra in one arena (CSR): cell i owns [offset[i], offset[i+1]), m/z ascending.
  /// 8 bytes per peak.
  struct PeakArena
  {
    std::vector<std::uint64_t> offset{0};
    std::vector<MzFixed> mz;
    std::vector<float> intensity;
    std::size_t cells() const { return offset.size() - 1; }

    /// m/z bucket index (buildIndex, after ingest): the arena's m/z range is cut into `buckets` buckets of
    /// 2^bucket_shift fixed-point units from bucket_base; bucket_start[cell * buckets + k] = position (relative
    /// to the cell start) of the cell's first peak in bucket k or above. uint16 per entry (~64 per cell:
    /// ~4% of the peak data); no index (buckets = 0) if a cell holds 65,535 peaks or more.
    std::vector<std::uint16_t> bucket_start;
    MzFixed bucket_base = 0;
    std::uint32_t bucket_shift = 0, buckets = 0;
    void buildIndex(std::uint32_t target_buckets = 64);
    /// A peak position in `cell` at or before the first peak >= lo (the cell start without an index).
    std::size_t seek(std::size_t cell, MzFixed lo) const
    {
      const std::size_t a = offset[cell];
      if (buckets == 0 || lo <= bucket_base) { return a; }
      const std::size_t k = (lo - bucket_base) >> bucket_shift;
      return k >= buckets ? offset[cell + 1] : a + bucket_start[cell * buckets + k];
    }
  };

  /// One DIA isolation window: its cell for global cycle c is cell c of `peaks`
  /// (empty when the window was not acquired in that cycle).
  struct WindowLane
  {
    double target = 0, lower = 0, upper = 0;
    PeakArena peaks;                    ///< one cell per global cycle
    std::vector<double> acquired_rt;    ///< exact RT of the spectrum in each cycle (NaN if none)
    std::vector<std::uint32_t> spectrum; ///< file position per cycle (UINT32_MAX if none)
  };

  /// MS1 survey scans in acquisition order (their own cells; mapped onto the global axis by
  /// interpolation at extraction time).
  struct Ms1Lane
  {
    PeakArena peaks;
    std::vector<double> rt;
    std::vector<std::uint32_t> spectrum;
  };

  struct StageTime
  {
    double wall = 0, cpu = 0;           ///< seconds (cpu = user + system of the process)
  };

  struct RunGridStats
  {
    std::uint64_t spectra = 0, ms1_spectra = 0, ms2_spectra = 0, other_spectra = 0;
    std::uint64_t ms1_peaks = 0, ms2_peaks = 0;
    std::uint64_t unsorted_spectra = 0;   ///< spectra that had to be sorted by m/z
    std::uint64_t missing_cells = 0;      ///< (window, cycle) cells without a spectrum
    std::uint64_t duplicate_cells = 0;    ///< extra spectra of a window within one cycle (merged)
    StageTime ingest;
  };

  /// The run on one global RT axis.
  ///
  /// A DIA cycle visits every isolation window once; global cycle c ends when a window
  /// repeats. `cycle_rt[c]` is the cycle's time (mean acquisition time of its MS2 spectra).
  /// Every window lane is indexed by the same c, so the traces of all fragments of all
  /// precursors share one time axis, and MS1 is interpolated onto it.
  class RunGrid
  {
  public:
    /// Ingest a centroided mzML (streamed; the serial XML parse overlaps with routing on a
    /// second thread) or a Thermo .raw (through the OpenMS Thermo bridge).
    /// `decode_threads` bounds OpenMS's internal (OpenMP) binary-array decoding in the parser
    /// thread; the parse itself is serial, and more decode threads only spin.
    static RunGrid load(const std::string& filename, int decode_threads = 1);

    std::vector<double> cycle_rt;        ///< the global RT axis (seconds)
    std::vector<WindowLane> windows;     ///< sorted by isolation target
    Ms1Lane ms1;
    RunGridStats stats;

    std::size_t cycles() const { return cycle_rt.size(); }
    std::size_t bytes() const;
    /// First cycle with cycle_rt >= t / first cycle with cycle_rt > t.
    std::size_t lowerCycle(double t) const;
    std::size_t upperCycle(double t) const;
  };

  StageTime stageClock();                          ///< current (wall, cpu) for stage deltas
  StageTime operator-(const StageTime& a, const StageTime& b);
}
