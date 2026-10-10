// ODIA-V3 trace extractor -- library RT -> run RT (seconds).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace OpenMS { class TransformationDescription; }

namespace ODIA
{
  /// Maps a library retention time to the run's retention time in seconds.
  ///
  /// Modes:
  ///  - identity: the library RT is already in run seconds (or minutes, scaled by 60);
  ///  - linear:   run = intercept + slope * lib;
  ///  - trafoXML: an OpenMS TransformationDescription (library -> run), as written by
  ///              OpenSwathRTNormalizer/MapAligner (use `invert` when it maps run -> library);
  ///  - anchors:  a two-column table (library RT, observed run RT in seconds) fitted with an
  ///              OpenMS transformation model (linear, b_spline, lowess, interpolated).
  class RtMap
  {
  public:
    RtMap();
    ~RtMap();
    RtMap(RtMap&&) noexcept;
    RtMap& operator=(RtMap&&) noexcept;

    static RtMap identity(double scale = 1.0);
    static RtMap linear(double intercept, double slope);
    static RtMap fromTrafoXML(const std::string& filename, bool invert);
    static RtMap fromAnchors(const std::string& filename, const std::string& model);
    /// Fit `model` to (library RT, run RT) points; `lowess_span` applies to model "lowess".
    static RtMap fromPoints(const std::vector<std::pair<double, double>>& points, const std::string& model,
                            double lowess_span = 2.0 / 3.0);

    double operator()(double library_rt) const;
    std::string describe() const;

  private:
    double intercept_ = 0.0, slope_ = 1.0;
    std::unique_ptr<OpenMS::TransformationDescription> trafo_;
    std::string description_ = "identity";
  };
}
