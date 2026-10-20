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
  struct RunModel
  {
    std::size_t anchors = 0, neighbours = 0;
    std::vector<double> rt, drt_center, drt_scale, drt_tail99, fwhm, support_lo, support_hi;
    bool empty() const { return rt.empty(); }
    /// `curve` (one of the members above) at run RT `t`; NaN when the model is empty or `curve` is not one of its curves.
    double at(const std::vector<double>& curve, double t) const;
    /// {"anchors": n, "neighbours": k, "rt": [...], "drt_center": [...], ...} (seconds, 3 decimals).
    std::string json() const;
  };

  /// The running estimators over anchors (run RT, residual, FWHM; equal lengths; non-finite entries are dropped).
  /// Fewer than `min_anchors` finite anchors give an empty model. Neighbourhood: the k anchors nearest in run RT,
  /// k = max(kmin, span x anchors), at least 5; grid: `grid_points` (>= 2) points. Invalid arguments throw.
  RunModel estimateRunModel(const std::vector<double>& rt, const std::vector<double>& residual, const std::vector<double>& fwhm,
                            std::size_t grid_points = 40, double span = 0.2, std::size_t kmin = 15, std::size_t min_anchors = 50);
}
