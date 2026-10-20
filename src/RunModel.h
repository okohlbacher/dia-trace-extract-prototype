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
  ///   fwhm:       trimmed mean (25-75%) of the anchors' peak widths at half height (s).
  /// The anchors are the clean part of the calibration panel: their tail is near Gaussian (drt_tail99 ~ 2.6 drt_scale),
  /// lighter than that of the identified precursors, so a window that must hold the identifications still needs the
  /// multiple calibrated against them (the RT window's 7 sigma), not drt_tail99 itself.
  struct RunModel
  {
    std::size_t anchors = 0, neighbours = 0;
    std::vector<double> rt, drt_center, drt_scale, drt_tail99, fwhm;
    bool empty() const { return rt.empty(); }
    /// `curve` (one of the members above) at run RT `t`.
    double at(const std::vector<double>& curve, double t) const;
    /// {"anchors": n, "neighbours": k, "rt": [...], "drt_center": [...], ...} (seconds, 3 decimals).
    std::string json() const;
  };

  /// The running estimators over anchors (run RT, residual, FWHM; equal lengths). Fewer than `min_anchors` give an
  /// empty model. Neighbourhood: max(kmin, span x anchors); grid: `grid_points` points.
  RunModel estimateRunModel(const std::vector<double>& rt, const std::vector<double>& residual, const std::vector<double>& fwhm,
                            std::size_t grid_points = 40, double span = 0.2, std::size_t kmin = 15, std::size_t min_anchors = 50);
}
