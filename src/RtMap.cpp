// ODIA-V3 trace extractor -- library RT -> run RT (seconds).
#include "RtMap.h"

#include <OpenMS/ANALYSIS/MAPMATCHING/TransformationDescription.h>
#include <OpenMS/DATASTRUCTURES/Param.h>
#include <OpenMS/FORMAT/TransformationXMLFile.h>

#include <charconv>
#include "Parse.h"
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace ODIA
{
  RtMap::RtMap() = default;
  RtMap::~RtMap() = default;
  RtMap::RtMap(RtMap&&) noexcept = default;
  RtMap& RtMap::operator=(RtMap&&) noexcept = default;

  RtMap RtMap::identity(double scale)
  {
    RtMap m;
    m.slope_ = scale;
    m.description_ = scale == 1.0 ? "identity (library RT in run seconds)" : "identity x" + std::to_string(scale);
    return m;
  }

  RtMap RtMap::linear(double intercept, double slope)
  {
    RtMap m;
    m.intercept_ = intercept;
    m.slope_ = slope;
    m.description_ = "linear: run_s = " + std::to_string(intercept) + " + " + std::to_string(slope) + " * lib";
    return m;
  }

  RtMap RtMap::fromTrafoXML(const std::string& filename, bool invert)
  {
    RtMap m;
    m.trafo_ = std::make_unique<OpenMS::TransformationDescription>();
    OpenMS::TransformationXMLFile().load(filename, *m.trafo_, true);
    if (invert) { m.trafo_->invert(); }
    m.description_ = "trafoXML " + filename + " (" + m.trafo_->getModelType() + (invert ? ", inverted)" : ")");
    return m;
  }

  RtMap RtMap::fromAnchors(const std::string& filename, const std::string& model)
  {
    std::ifstream in(filename);
    if (!in) { throw std::runtime_error("cannot open " + filename); }
    std::vector<std::pair<double, double>> points;
    std::string line;
    int used_col = -1;
    while (std::getline(in, line))
    {
      if (line.empty() || line[0] == '#') { continue; }
      if (used_col < 0 && line.rfind("library_rt", 0) == 0)
      {
        // our -out_calibration header: remember where `used` is, so rejected anchors are not refitted
        std::size_t col = 0;
        for (std::size_t b = 0; b <= line.size(); ++col)
        {
          const std::size_t e = std::min(line.find('\t', b), line.size());
          if (line.compare(b, e - b, "used") == 0) { used_col = static_cast<int>(col); }
          b = e + 1;
        }
        continue;
      }
      if (used_col >= 0)
      {
        std::size_t b = 0;
        for (int col = 0; col < used_col && b < line.size(); ++col) { b = line.find('\t', b) + 1; }
        if (b < line.size() && line[b] == '0') { continue; }
      }
      const auto sep = line.find_first_of("\t,; ");
      if (sep == std::string::npos) { continue; }
      double x = 0, y = 0;
      const std::string_view a(line.data(), sep), b(line.data() + sep + 1, line.size() - sep - 1);
      if (!parseDoublePrefix(a, x) || !parseDoublePrefix(b, y))
      {
        continue; // header or malformed line
      }
      points.emplace_back(x, y);
    }
    if (points.size() < 2) { throw std::runtime_error("RT anchors: fewer than 2 points in " + filename); }
    RtMap m = fromPoints(points, model);
    m.description_ = "anchors " + filename + " (" + std::to_string(points.size()) + " points, " + model + ")";
    return m;
  }

  RtMap RtMap::fromPoints(const std::vector<std::pair<double, double>>& points, const std::string& model,
                          double lowess_span)
  {
    if (points.size() < 2) { throw std::runtime_error("RT map: fewer than 2 points"); }
    RtMap m;
    m.trafo_ = std::make_unique<OpenMS::TransformationDescription>();
    m.trafo_->setDataPoints(points);
    OpenMS::Param p;
    if (model == "lowess")
    {
      p.setValue("span", lowess_span);
      p.setValue("auto_span", "false");
    }
    m.trafo_->fitModel(model, p);
    m.description_ = model + " fit to " + std::to_string(points.size()) + " points";
    return m;
  }

  double RtMap::operator()(double library_rt) const
  {
    if (trafo_) { return trafo_->apply(library_rt); }
    return intercept_ + slope_ * library_rt;
  }

  std::string RtMap::describe() const { return description_; }
}
