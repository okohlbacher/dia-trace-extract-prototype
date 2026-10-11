// ODIA-V3 trace extractor -- self-calibration of RT and m/z from the run grid (no external search).
#pragma once

#include "Library.h"
#include "RtMap.h"
#include "RunGrid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ODIA
{
  struct CalibrationOptions
  {
    std::size_t panel = 0;            ///< precursors sampled (stratified over library RT); 0 = panel_fraction of the eligible
    double panel_fraction = 0.05;
    double search_ppm = 50.0;         ///< first-pass half width (uncalibrated); later passes narrow around the measured offset
    double search_narrow_factor = 3.0; ///< pass n+1 searches +- factor x the accuracy measured in pass n (the outer part
                                      ///< of that window measures the background of random centroids)
    int search_rounds = 3;            ///< at most this many stage-2 passes (stops when the window changes < 25%)
    double accuracy_sigma = 6.0;      ///< extraction tolerance = this multiple of the robust (MAD) width of the apex mass errors
    double min_relative_intensity = 0.0; ///< apex centroids below this share of the anchor's strongest fragment are left out of the mass model
    int top_fragments = 6;            ///< fragments per panel precursor used for co-elution
    int half_window_cycles = 4;       ///< co-elution window around a candidate apex (+- cycles)
    int candidates = 5;               ///< local maxima of the summed trace evaluated per precursor
    // Stage 1 (whole run, no prior): strict co-elution criteria.
    int strict_min_fragments = 5;
    double strict_min_correlation = 0.90;
    double strict_min_cosine = 0.85;
    int mode_bins = 20;               ///< library-RT bins for the robust initial curve (run-RT mode per bin)
    double mode_bandwidth_s = 15.0;   ///< half width of the sliding window that finds the mode
    // Stage 2 (within +- max(4 sigma, min_window_s) of the stage-1 curve): relaxed criteria.
    int min_fragments = 4;            ///< fragments with signal at the apex
    double min_correlation = 0.80;    ///< mean pairwise Pearson of fragment traces around the apex
    double min_library_cosine = 0.75; ///< cosine of sqrt(apex intensities) vs sqrt(library intensities)
    double min_window_s = 30.0;
    std::string model = "lowess";     ///< OpenMS transformation model for library RT -> run RT
    double lowess_span = 0.15;
    int rejection_rounds = 3;         ///< refits dropping anchors beyond max(3 sigma, min_reject_s)
    double min_reject_s = 3.0;
    int threads = 1;
  };

  /// A panel precursor whose fragments co-elute convincingly at one apex.
  struct Anchor
  {
    std::uint32_t precursor = 0;
    double library_rt = 0, run_rt = 0;   ///< run_rt = global cycle time of the apex (s)
    std::uint32_t apex_cycle = 0;
    std::uint8_t fragments = 0;          ///< fragments with signal at the apex
    float correlation = 0, cosine = 0;
    float residual = 0;                  ///< run_rt - map(library_rt) after the final fit
    bool used = false;                   ///< survived outlier rejection
  };

  struct CalibrationResult
  {
    std::vector<Anchor> anchors;         ///< all accepted apexes (used = in the final fit)
    RtMap rt_map;
    std::size_t panel = 0, used = 0;
    std::size_t stage1_anchors = 0, stage1_used = 0;
    double stage2_window_s = 0;
    double rt_residual_p50 = 0, rt_residual_p95 = 0, rt_residual_p99 = 0;  ///< |residual|, s, used anchors
    double ms2_offset_ppm = 0, ms2_sigma_ppm = 0, ms1_offset_ppm = 0, ms1_sigma_ppm = 0;
    /// Extraction accuracy (half width around the offset) = accuracy_sigma x the robust width of the apex errors.
    double ms2_accuracy_ppm = 0, ms1_accuracy_ppm = 0;
    double ms2_search_ppm = 0, ms1_search_ppm = 0;            ///< half width of the final search pass
    int search_rounds = 0;                                    ///< stage-2 passes run
    std::size_t strict_mass_anchors = 0;                      ///< anchors the mass model comes from
    std::uint64_t ms2_mass_points = 0, ms1_mass_points = 0;
    StageTime time;
  };

  /// Derive the library-RT -> run-RT map and systematic m/z offsets from the run itself:
  /// whole-run extraction of a stratified panel, co-elution apex picking, robust model fit,
  /// median mass errors at the apexes (unweighted: strong centroids drift upward on Astral, about +1.45 ppm
  /// per decade above 6e4, which biased an intensity-weighted median by ~0.55 ppm). Heuristic
  /// calibration -- not identification.
  CalibrationResult calibrate(const Library& lib, const RunGrid& grid, const CalibrationOptions& opt);

  /// Anchors as TSV (library_rt, run_rt, ...): usable as -rt_anchors input.
  void writeAnchors(const std::string& filename, const Library& lib, const CalibrationResult& r);
}
