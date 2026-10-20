// ODIA-V3 trace extractor -- what the calibration measured about the run, as functions of run RT.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ODIA
{
  /// The run's RT error and peak width along the gradient, estimated from the calibration anchors and kept for the
  /// algorithms downstream of the extraction (and recorded with the output). Each curve is a running estimator over
  /// the `neighbours` anchors nearest in run RT, on a grid spanning the 5-95% quantiles of the anchors' RT; between grid
  /// points linear, outside it constant (docs/14-rt-window-estimation.md):
  ///   drt_center: median of the residuals (apex RT - mapped library RT, s): the RT map's local bias;
  ///   drt_scale:  1.4826 x MAD of the residuals about drt_center (s);
  ///   drt_tail99: 99th percentile of |residual - drt_center| (s);
  ///   fwhm:       mean of the central half (by rank) of the anchors' peak widths at half height (s);
  ///   support_lo/hi: the run-RT extent of each point's neighbourhood (s): at the ends of the range it is one-sided.
  /// Descriptive: the anchors' residuals are in-sample to the RT map and the anchors are a selected population, and a
  /// 99th percentile from ~k/5 neighbours is close to their maximum; none of the curves is a coverage guarantee.
  /// On the benchmark data the identifications' RT errors had a heavier tail than the anchors' (the window needs 7 x
  /// drt_scale for 99.4% of DIA-NN's peaks, the anchors' drt_tail99 is 2-5 x drt_scale), so a window must keep its
  /// multiple calibrated against identifications, not take drt_tail99 as its width.
  /// The m/z errors of the calibration anchors' apex centroids (the most intense centroid in each fragment's or the
  /// precursor's search window), about the final mass model (offset + slope x m/z), for one level and one search:
  ///   "broad": stage 1, the strict anchors found at the fixed +-search_ppm, before any narrowing -- not truncated by
  ///            the narrowed window, but containing random centroids (background) from that wide window;
  ///   "final": the last stage-2 pass, inside the narrowed window -- what the mass model and the tolerance come from.
  /// Their comparison is the check for the selection loop (a narrowed window keeps the matches that fit it).
  /// Scales are "core" robust scales: 1.4826 x MAD, recomputed twice over the points within 10 scales of the median
  /// (the background of a wide window inflates a plain MAD). Intensity: the centroid's raw intensity, in equal-count
  /// tenths by rank (fewer bins with few points). Descriptive, like the RT curves.
  struct MzErrors
  {
    std::string level, search;              ///< "ms2" / "ms1"; "broad" / "final"
    double search_ppm = 0;                  ///< half width of the search window the centroids were found in
    double tolerance_ppm = 0;               ///< the extraction tolerance (half width) of this level
    std::size_t points = 0, neighbours = 0; ///< centroids; centroids per estimate along RT
    double center = 0, scale = 0;           ///< median and core scale of the residuals (ppm), all points
    double beyond_tolerance = 0;            ///< share of points with |residual| > tolerance_ppm
    double background = 0;                  ///< twice the share in the window's outer half: the random centroids if the
                                            ///< background is flat and the true errors lie in the inner half (broad search);
                                            ///< for a narrow window it also counts true tails (an upper bound)
    std::vector<double> rt, center_rt, scale_rt;   ///< along RT: grid over the 5-95% quantiles of the points' RT
    std::vector<double> intensity_lo, intensity_hi, center_by_intensity, scale_by_intensity;   ///< by intensity bin
    std::vector<std::size_t> count_by_intensity;
    bool empty() const { return points == 0; }
    std::string json() const;
  };

  struct RunModel
  {
    std::size_t anchors = 0, neighbours = 0;
    std::vector<double> rt, drt_center, drt_scale, drt_tail99, fwhm, support_lo, support_hi;
    std::vector<MzErrors> mz;   ///< m/z errors (MS2, MS1; broad and final search) -- present with a calibration
    bool empty() const { return rt.empty() && mz.empty(); }
    /// `curve` (one of the members above) at run RT `t`; NaN when the model is empty or `curve` is not one of its curves.
    double at(const std::vector<double>& curve, double t) const;
    /// {"anchors": n, "neighbours": k, "rt": [...], "drt_center": [...], ..., "mz": [{...}, ...]} (seconds, ppm; 3 decimals).
    std::string json() const;
  };

  /// The running estimators over anchors (run RT, residual, FWHM; equal lengths; non-finite entries are dropped).
  /// Fewer than `min_anchors` finite anchors give an empty model. Neighbourhood: the k anchors nearest in run RT,
  /// k = max(kmin, span x anchors), at least 5; grid: `grid_points` (>= 2) points. Invalid arguments throw.
  RunModel estimateRunModel(const std::vector<double>& rt, const std::vector<double>& residual, const std::vector<double>& fwhm,
                            std::size_t grid_points = 40, double span = 0.2, std::size_t kmin = 15, std::size_t min_anchors = 50);

  /// The m/z-error summary of one level and search (MzErrors) from its centroids: run RT, residual about the mass
  /// model (ppm), position in the search window (ppm from its centre), intensity; equal lengths, non-finite entries
  /// dropped. Fewer than `min_points` give an empty summary. Along RT: the k nearest points, k = max(kmin, span x n).
  MzErrors estimateMzErrors(const std::string& level, const std::string& search, const std::vector<double>& rt,
                            const std::vector<double>& residual, const std::vector<double>& window, const std::vector<double>& intensity,
                            double search_ppm, double tolerance_ppm, std::size_t grid_points = 30, double span = 0.2,
                            std::size_t kmin = 50, std::size_t min_points = 200);
}
