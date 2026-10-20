#include "RunModel.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace ODIA
{
  namespace
  {
    double median(std::vector<double> v)
    {
      const std::size_t m = v.size() / 2;
      std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
      if (v.size() % 2) { return v[m]; }
      const double hi = v[m];
      return 0.5 * (hi + *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m)));
    }

    double quantile(std::vector<double> v, double q)
    {
      std::sort(v.begin(), v.end());
      const double pos = q * static_cast<double>(v.size() - 1);
      const auto i = static_cast<std::size_t>(pos);
      return i + 1 < v.size() ? v[i] + (pos - static_cast<double>(i)) * (v[i + 1] - v[i]) : v.back();
    }

    double trimmedMean(std::vector<double> v)
    {
      std::sort(v.begin(), v.end());
      const double lo = quantile(v, 0.25), hi = quantile(v, 0.75);
      double s = 0;
      std::size_t n = 0;
      for (double x : v) { if (x >= lo && x <= hi) { s += x; ++n; } }
      return n ? s / static_cast<double>(n) : median(v);
    }
  }

  RunModel estimateRunModel(const std::vector<double>& rt, const std::vector<double>& residual, const std::vector<double>& fwhm,
                            std::size_t grid_points, double span, std::size_t kmin, std::size_t min_anchors)
  {
    if (rt.size() != residual.size() || rt.size() != fwhm.size()) { throw std::invalid_argument("estimateRunModel: lengths differ"); }
    RunModel m;
    const std::size_t n = rt.size();
    if (n < std::max<std::size_t>(min_anchors, 2) || grid_points < 2) { return m; }
    std::vector<std::size_t> o(n);
    std::iota(o.begin(), o.end(), 0);
    std::stable_sort(o.begin(), o.end(), [&](std::size_t a, std::size_t b) { return rt[a] < rt[b]; });
    std::vector<double> t(n), r(n), f(n);
    for (std::size_t i = 0; i < n; ++i) { t[i] = rt[o[i]]; r[i] = residual[o[i]]; f[i] = fwhm[o[i]]; }
    const std::size_t k = std::min(n, std::max(kmin, static_cast<std::size_t>(std::lround(span * static_cast<double>(n)))));
    m.anchors = n;
    m.neighbours = k;
    const double g0 = quantile(t, 0.05), g1 = quantile(t, 0.95);
    for (std::size_t j = 0; j < grid_points; ++j)
    {
      const double g = g0 + (g1 - g0) * static_cast<double>(j) / static_cast<double>(grid_points - 1);
      // the k anchors nearest in RT: a window of k consecutive anchors, centred on g where the data allow
      const std::size_t i = static_cast<std::size_t>(std::lower_bound(t.begin(), t.end(), g) - t.begin());
      const std::size_t lo = std::min(i > k / 2 ? i - k / 2 : 0, n - k);
      const std::vector<double> rr(r.begin() + static_cast<std::ptrdiff_t>(lo), r.begin() + static_cast<std::ptrdiff_t>(lo + k));
      const std::vector<double> ff(f.begin() + static_cast<std::ptrdiff_t>(lo), f.begin() + static_cast<std::ptrdiff_t>(lo + k));
      const double c = median(rr);
      std::vector<double> dev(k);
      for (std::size_t q = 0; q < k; ++q) { dev[q] = std::fabs(rr[q] - c); }
      m.rt.push_back(g);
      m.drt_center.push_back(c);
      m.drt_scale.push_back(1.4826 * median(dev));
      m.drt_tail99.push_back(quantile(dev, 0.99));
      m.fwhm.push_back(trimmedMean(ff));
    }
    return m;
  }

  double RunModel::at(const std::vector<double>& curve, double x) const
  {
    if (rt.empty() || curve.size() != rt.size()) { return 0.0; }
    if (x <= rt.front()) { return curve.front(); }
    if (x >= rt.back()) { return curve.back(); }
    const std::size_t i = static_cast<std::size_t>(std::upper_bound(rt.begin(), rt.end(), x) - rt.begin());
    const double w = (x - rt[i - 1]) / (rt[i] - rt[i - 1]);
    return curve[i - 1] + w * (curve[i] - curve[i - 1]);
  }

  std::string RunModel::json() const
  {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    const auto list = [&](const char* name, const std::vector<double>& v) {
      o << ", \"" << name << "\": [";
      for (std::size_t i = 0; i < v.size(); ++i) { o << (i ? ", " : "") << v[i]; }
      o << "]";
    };
    o << "{\"anchors\": " << anchors << ", \"neighbours\": " << neighbours;
    list("rt", rt); list("drt_center", drt_center); list("drt_scale", drt_scale); list("drt_tail99", drt_tail99); list("fwhm", fwhm);
    o << "}";
    return o.str();
  }
}
