#include "RunModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>

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

    /// Start of the k points of sorted `t` nearest to g (in one dimension a contiguous run): [lo, hi) grown from g
    /// towards the nearer neighbour; ties go to the earlier point.
    std::size_t nearest(const std::vector<double>& t, double g, std::size_t k)
    {
      const std::size_t n = t.size();
      std::size_t hi = static_cast<std::size_t>(std::lower_bound(t.begin(), t.end(), g) - t.begin()), lo = hi;
      while (hi - lo < k)
      {
        if (lo == 0) { ++hi; }
        else if (hi == n) { --lo; }
        else if (g - t[lo - 1] <= t[hi] - g) { --lo; }
        else { ++hi; }
      }
      return lo;
    }

    /// (median, core scale): 1.4826 x MAD, recomputed twice over the points within 10 scales of the median.
    std::pair<double, double> coreScale(const std::vector<double>& v)
    {
      double c = median(v), s = 0;
      std::vector<double> core = v;
      for (int pass = 0; pass < 3; ++pass)
      {
        std::vector<double> dev;
        for (double x : core) { dev.push_back(std::fabs(x - c)); }
        s = 1.4826 * median(dev);
        if (pass == 2 || s <= 0) { break; }
        core.clear();
        for (double x : v) { if (std::fabs(x - c) <= 10 * s) { core.push_back(x); } }
        c = median(core);
      }
      return {c, s};
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
      const std::size_t lo = nearest(t, g, k);   // the k anchors nearest in RT
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

  MzErrors estimateMzErrors(const std::string& level, const std::string& search, const std::vector<double>& rt,
                            const std::vector<double>& residual, const std::vector<double>& window, const std::vector<double>& intensity,
                            double search_ppm, double tolerance_ppm, std::size_t grid_points, double span, std::size_t kmin, std::size_t min_points)
  {
    if (rt.size() != residual.size() || rt.size() != window.size() || rt.size() != intensity.size()) { throw std::invalid_argument("estimateMzErrors: lengths differ"); }
    if (grid_points < 2 || !(span > 0.0 && span <= 1.0)) { throw std::invalid_argument("estimateMzErrors: grid_points >= 2 and 0 < span <= 1"); }
    MzErrors e;
    e.level = level; e.search = search; e.search_ppm = search_ppm; e.tolerance_ppm = tolerance_ppm;
    std::vector<std::size_t> o;
    for (std::size_t i = 0; i < rt.size(); ++i)
    {
      if (std::isfinite(rt[i]) && std::isfinite(residual[i]) && std::isfinite(window[i]) && std::isfinite(intensity[i])) { o.push_back(i); }
    }
    const std::size_t n = o.size();
    if (n < std::max<std::size_t>(min_points, 10)) { return e; }
    e.points = n;
    std::vector<double> r;
    std::size_t beyond = 0, outer = 0;
    for (std::size_t i : o)
    {
      r.push_back(residual[i]);
      beyond += std::fabs(residual[i]) > tolerance_ppm;
      outer += std::fabs(window[i]) > 0.5 * search_ppm;
    }
    std::tie(e.center, e.scale) = coreScale(r);
    e.beyond_tolerance = static_cast<double>(beyond) / static_cast<double>(n);
    e.background = std::min(1.0, 2.0 * static_cast<double>(outer) / static_cast<double>(n));
    // along RT
    std::stable_sort(o.begin(), o.end(), [&](std::size_t a, std::size_t b) { return rt[a] < rt[b]; });
    std::vector<double> t(n), rs(n);
    for (std::size_t i = 0; i < n; ++i) { t[i] = rt[o[i]]; rs[i] = residual[o[i]]; }
    const std::size_t k = std::min(n, std::max<std::size_t>({kmin, 10, static_cast<std::size_t>(std::lround(span * static_cast<double>(n)))}));
    e.neighbours = k;
    const double g0 = quantile(t, 0.05), g1 = quantile(t, 0.95);
    for (std::size_t j = 0; j < grid_points; ++j)
    {
      const double g = g0 + (g1 - g0) * static_cast<double>(j) / static_cast<double>(grid_points - 1);
      const std::size_t lo = nearest(t, g, k);
      const auto [c, sc] = coreScale(std::vector<double>(rs.begin() + static_cast<std::ptrdiff_t>(lo), rs.begin() + static_cast<std::ptrdiff_t>(lo + k)));
      e.rt.push_back(g); e.center_rt.push_back(c); e.scale_rt.push_back(sc);
    }
    // by intensity: equal-count bins by rank, at least kmin points each, at most ten
    std::stable_sort(o.begin(), o.end(), [&](std::size_t a, std::size_t b) { return intensity[a] < intensity[b]; });
    const std::size_t bins = std::min<std::size_t>(10, n / std::max<std::size_t>(kmin, 1));
    for (std::size_t b = 0; b < bins; ++b)
    {
      const std::size_t a0 = b * n / bins, a1 = (b + 1) * n / bins;
      std::vector<double> v;
      for (std::size_t i = a0; i < a1; ++i) { v.push_back(residual[o[i]]); }
      const auto [c, sc] = coreScale(v);
      e.intensity_lo.push_back(intensity[o[a0]]); e.intensity_hi.push_back(intensity[o[a1 - 1]]);
      e.center_by_intensity.push_back(c); e.scale_by_intensity.push_back(sc); e.count_by_intensity.push_back(a1 - a0);
    }
    return e;
  }

  std::string MzErrors::json() const
  {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    const auto list = [&](const char* name, const auto& v) {
      o << ", \"" << name << "\": [";
      for (std::size_t i = 0; i < v.size(); ++i) { o << (i ? ", " : "") << v[i]; }
      o << "]";
    };
    o << "{\"level\": \"" << level << "\", \"search\": \"" << search << "\", \"search_ppm\": " << search_ppm << ", \"tolerance_ppm\": "
      << tolerance_ppm << ", \"points\": " << points << ", \"neighbours\": " << neighbours << ", \"center\": " << center
      << ", \"scale\": " << scale << ", \"beyond_tolerance\": " << beyond_tolerance << ", \"background\": " << background;
    list("rt", rt); list("center_rt", center_rt); list("scale_rt", scale_rt);
    o.unsetf(std::ios::fixed);
    o.precision(4);
    list("intensity_lo", intensity_lo); list("intensity_hi", intensity_hi);
    o.setf(std::ios::fixed);
    o.precision(3);
    list("center_by_intensity", center_by_intensity); list("scale_by_intensity", scale_by_intensity); list("count_by_intensity", count_by_intensity);
    o << "}";
    return o.str();
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
    o << ", \"mz\": [";
    for (std::size_t i = 0; i < mz.size(); ++i) { o << (i ? ", " : "") << mz[i].json(); }
    o << "]}";
    return o.str();
  }
}
