#include "RunModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

    /// Mean of the central half by rank (ties at the quartiles do not widen it).
    double trimmedMean(std::vector<double> v)
    {
      std::sort(v.begin(), v.end());
      const std::size_t a = v.size() / 4, b = std::max(a + 1, v.size() - v.size() / 4);
      double s = 0;
      for (std::size_t i = a; i < b; ++i) { s += v[i]; }
      return s / static_cast<double>(b - a);
    }
  }

  RunModel estimateRunModel(const std::vector<double>& rt, const std::vector<double>& residual, const std::vector<double>& fwhm,
                            std::size_t grid_points, double span, std::size_t kmin, std::size_t min_anchors)
  {
    if (rt.size() != residual.size() || rt.size() != fwhm.size()) { throw std::invalid_argument("estimateRunModel: lengths differ"); }
    if (grid_points < 2 || !(span > 0.0 && span <= 1.0)) { throw std::invalid_argument("estimateRunModel: grid_points >= 2 and 0 < span <= 1"); }
    RunModel m;
    std::vector<std::size_t> o;
    for (std::size_t i = 0; i < rt.size(); ++i) { if (std::isfinite(rt[i]) && std::isfinite(residual[i]) && std::isfinite(fwhm[i])) { o.push_back(i); } }
    const std::size_t n = o.size();
    if (n < std::max<std::size_t>(min_anchors, 5)) { return m; }
    std::stable_sort(o.begin(), o.end(), [&](std::size_t a, std::size_t b) { return rt[a] < rt[b]; });
    std::vector<double> t(n), r(n), f(n);
    for (std::size_t i = 0; i < n; ++i) { t[i] = rt[o[i]]; r[i] = residual[o[i]]; f[i] = fwhm[o[i]]; }
    const std::size_t k = std::min(n, std::max<std::size_t>({kmin, 5, static_cast<std::size_t>(std::lround(span * static_cast<double>(n)))}));
    m.anchors = n;
    m.neighbours = k;
    const double g0 = quantile(t, 0.05), g1 = quantile(t, 0.95);
    for (std::size_t j = 0; j < grid_points; ++j)
    {
      const double g = g0 + (g1 - g0) * static_cast<double>(j) / static_cast<double>(grid_points - 1);
      // the k anchors nearest in RT (in one dimension a contiguous run of the sorted anchors): grow [lo, hi) from g
      // towards the nearer neighbour; ties go to the earlier anchor
      std::size_t hi = static_cast<std::size_t>(std::lower_bound(t.begin(), t.end(), g) - t.begin()), lo = hi;
      while (hi - lo < k)
      {
        if (lo == 0) { ++hi; }
        else if (hi == n) { --lo; }
        else if (g - t[lo - 1] <= t[hi] - g) { --lo; }
        else { ++hi; }
      }
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
      m.support_lo.push_back(t[lo]);
      m.support_hi.push_back(t[lo + k - 1]);
    }
    return m;
  }

  double RunModel::at(const std::vector<double>& curve, double x) const
  {
    const bool ours = &curve == &drt_center || &curve == &drt_scale || &curve == &drt_tail99 || &curve == &fwhm || &curve == &support_lo || &curve == &support_hi;
    if (rt.empty() || !ours || curve.size() != rt.size()) { return std::numeric_limits<double>::quiet_NaN(); }
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
    list("support_lo", support_lo); list("support_hi", support_hi);
    o << "}";
    return o.str();
  }
}
