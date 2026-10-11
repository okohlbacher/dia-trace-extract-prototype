// ODIA-V3 trace extractor -- self-calibration of RT and m/z from the run grid (no external search).
#include "Calibration.h"

#include "Pipeline.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace ODIA
{
  namespace
  {
    constexpr double C13_C12 = 1.0033548378;

    /// Most intense centroid within [lo, hi] of cell [a, b): (intensity, m/z), or (0, 0).
    std::pair<float, MzFixed> maxPeak(const PeakArena& p, std::size_t a, std::size_t b, MzFixed lo, MzFixed hi)
    {
      const MzFixed* mz = p.mz.data();
      auto i = static_cast<std::size_t>(std::lower_bound(mz + a, mz + b, lo) - mz);
      std::pair<float, MzFixed> best{0.0f, 0};
      for (; i < b && mz[i] <= hi; ++i)
      {
        if (p.intensity[i] > best.first) { best = {p.intensity[i], mz[i]}; }
      }
      return best;
    }

    double pearson(const float* x, const float* y, std::size_t n)
    {
      double mx = 0, my = 0;
      for (std::size_t i = 0; i < n; ++i) { mx += x[i]; my += y[i]; }
      mx /= n; my /= n;
      double sxy = 0, sxx = 0, syy = 0;
      for (std::size_t i = 0; i < n; ++i)
      {
        const double dx = x[i] - mx, dy = y[i] - my;
        sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
      }
      return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : std::numeric_limits<double>::quiet_NaN();
    }

    /// Median and MAD-based sigma, unweighted (the intensities are ignored), plus 5 x the 70th percentile of
    /// the absolute residuals around the median.
    struct Centre { double median = 0, sigma = 0, accuracy = 0; };
    Centre robustCentre(std::vector<std::pair<double, double>> v)
    {
      Centre c;
      if (v.empty()) { return c; }
      std::vector<double> x;
      x.reserve(v.size());
      for (const auto& [val, w] : v) { x.push_back(val); }
      std::sort(x.begin(), x.end());
      const auto q = [](const std::vector<double>& s, double f) { return s[static_cast<std::size_t>(f * (s.size() - 1) + 0.5)]; };
      c.median = q(x, 0.5);
      for (auto& val : x) { val = std::fabs(val - c.median); }
      std::sort(x.begin(), x.end());
      c.sigma = 1.4826 * q(x, 0.5);
      c.accuracy = 5.0 * q(x, 0.7);
      return c;
    }

    /// Quantile of |value| (values already centred).
    double robustAbsQuantile(const std::vector<std::pair<double, double>>& v, double f)
    {
      std::vector<double> x;
      for (const auto& [val, w] : v) { x.push_back(std::fabs(val)); }
      if (x.empty()) { return 0.0; }
      std::sort(x.begin(), x.end());
      return x[static_cast<std::size_t>(f * (x.size() - 1) + 0.5)];
    }

    /// Weighted median and weighted MAD-based sigma.
    [[maybe_unused]] std::pair<double, double> weightedCentre(std::vector<std::pair<double, double>> v)
    {
      if (v.empty()) { return {0.0, 0.0}; }
      auto wmedian = [](std::vector<std::pair<double, double>>& x) {
        std::sort(x.begin(), x.end());
        double total = 0;
        for (const auto& [val, w] : x) { total += w; }
        double acc = 0;
        for (const auto& [val, w] : x) { acc += w; if (acc >= total / 2) { return val; } }
        return x.back().first;
      };
      const double m = wmedian(v);
      for (auto& [val, w] : v) { val = std::fabs(val - m); }
      return {m, 1.4826 * wmedian(v)};
    }

    double quantile(std::vector<double> v, double q)
    {
      if (v.empty()) { return 0.0; }
      std::sort(v.begin(), v.end());
      return v[static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * (v.size() - 1))];
    }

    struct PanelResult
    {
      bool accepted = false;
      Anchor anchor;
      std::vector<std::pair<double, double>> ms2_ppm;  ///< (ppm, intensity) at the apex
      std::pair<double, double> ms1_ppm{0, 0};         ///< (ppm, intensity) of M+0 at the nearest MS1 scan
    };

    std::size_t nearestWindow(const RunGrid& g, double mz)
    {
      std::size_t best = g.windows.size();
      double d = std::numeric_limits<double>::infinity();
      for (std::size_t w = 0; w < g.windows.size(); ++w)
      {
        const auto& W = g.windows[w];
        if (mz < W.lower || mz > W.upper) { continue; }
        if (std::fabs(mz - W.target) < d) { d = std::fabs(mz - W.target); best = w; }
      }
      return best;
    }

    struct Criteria
    {
      int min_fragments;
      double min_correlation, min_cosine;
    };

    /// Look for a co-eluting apex of panel precursor p within global cycles [c_lo, c_hi).
    PanelResult examine(const Library& lib, const RunGrid& g, const CalibrationOptions& opt, std::uint32_t p,
                        std::size_t c_lo, std::size_t c_hi, const Criteria& crit)
    {
      PanelResult out;
      const std::size_t w = nearestWindow(g, lib.precursor_mz[p]);
      if (w == g.windows.size()) { return out; }
      const auto& lane = g.windows[w];
      const std::size_t C = g.cycles();
      const std::uint32_t f0 = lib.fragment_begin[p];
      const std::size_t F = std::min<std::size_t>(lib.fragmentsOf(p), static_cast<std::size_t>(opt.top_fragments));
      if (F < static_cast<std::size_t>(crit.min_fragments) || c_hi <= c_lo) { return out; }

      // Whole-run traces of the top fragments (max centroid within the wide window).
      std::vector<float> tile(F * C, 0.0f);
      std::vector<std::pair<MzFixed, MzFixed>> bounds(F);
      for (std::size_t f = 0; f < F; ++f)
      {
        const double t = lib.fragment_mz[f0 + f], tol = t * opt.search_ppm * 1e-6;
        bounds[f] = {toFixed(t - tol), toFixed(t + tol)};
      }
      std::vector<float> sum(C, 0.0f);
      // Gather the range plus the co-elution margin so windows at its edges are complete.
      const std::size_t g_lo = c_lo > static_cast<std::size_t>(opt.half_window_cycles) ? c_lo - opt.half_window_cycles : 0;
      const std::size_t g_hi = std::min(C, c_hi + opt.half_window_cycles);
      for (std::size_t c = g_lo; c < g_hi; ++c)
      {
        const std::size_t a = lane.peaks.offset[c], b = lane.peaks.offset[c + 1];
        if (a == b) { continue; }
        for (std::size_t f = 0; f < F; ++f)
        {
          const float v = maxPeak(lane.peaks, a, b, bounds[f].first, bounds[f].second).first;
          tile[f * C + c] = v;
          sum[c] += v;
        }
      }

      // Candidate apexes: the largest local maxima of the summed trace, at least 2 windows apart.
      const int hw = opt.half_window_cycles;
      std::vector<std::size_t> maxima;
      for (std::size_t c = std::max<std::size_t>(c_lo, 1); c + 1 < C && c < c_hi; ++c)
      {
        if (sum[c] > 0 && sum[c] >= sum[c - 1] && sum[c] > sum[c + 1]) { maxima.push_back(c); }
      }
      std::sort(maxima.begin(), maxima.end(), [&](std::size_t a, std::size_t b) { return sum[a] > sum[b]; });
      std::vector<std::size_t> cand;
      for (auto c : maxima)
      {
        bool far = true;
        for (auto d : cand) { if (std::abs(static_cast<long>(c) - static_cast<long>(d)) < 2 * hw) { far = false; break; } }
        if (far) { cand.push_back(c); }
        if (static_cast<int>(cand.size()) >= opt.candidates) { break; }
      }

      double best_score = -1;
      for (auto c : cand)
      {
        if (c < static_cast<std::size_t>(hw) || c + hw >= C) { continue; }
        const std::size_t lo = c - hw, n = 2 * hw + 1;
        // fragments with signal at the apex
        std::vector<std::size_t> present;
        for (std::size_t f = 0; f < F; ++f) { if (tile[f * C + c] > 0) { present.push_back(f); } }
        if (present.size() < static_cast<std::size_t>(crit.min_fragments)) { continue; }
        double rsum = 0; int rn = 0;
        for (std::size_t i = 0; i < present.size(); ++i)
        {
          for (std::size_t j = i + 1; j < present.size(); ++j)
          {
            const double r = pearson(&tile[present[i] * C + lo], &tile[present[j] * C + lo], n);
            if (!std::isnan(r)) { rsum += r; ++rn; }
          }
        }
        if (rn == 0) { continue; }
        const double corr = rsum / rn;
        double dot = 0, nx = 0, ny = 0;
        for (std::size_t f = 0; f < F; ++f)
        {
          const double x = std::sqrt(static_cast<double>(tile[f * C + c]));
          const double y = std::sqrt(static_cast<double>(lib.fragment_intensity[f0 + f]));
          dot += x * y; nx += x * x; ny += y * y;
        }
        const double cosine = nx > 0 && ny > 0 ? dot / std::sqrt(nx * ny) : 0.0;
        const double score = corr * cosine;
        if (score > best_score && corr >= crit.min_correlation && cosine >= crit.min_cosine)
        {
          best_score = score;
          out.accepted = true;
          out.anchor = {p, lib.library_rt[p], g.cycle_rt[c], static_cast<std::uint32_t>(c),
                        static_cast<float>(corr), static_cast<float>(cosine), 0.0f, false};
        }
      }
      if (!out.accepted) { return out; }

      // Mass errors at the apex: observed m/z of the most intense centroid per fragment, for fragments with at
      // least 10% of the anchor's strongest apex signal (a weak "fragment" is often a random centroid inside the
      // wide search window, which would inflate the residual quantiles behind the measured accuracy).
      const std::size_t c = out.anchor.apex_cycle;
      const std::size_t a = lane.peaks.offset[c], b = lane.peaks.offset[c + 1];
      std::vector<std::pair<float, MzFixed>> apex(F);
      float strongest = 0;
      for (std::size_t f = 0; f < F; ++f)
      {
        apex[f] = maxPeak(lane.peaks, a, b, bounds[f].first, bounds[f].second);
        strongest = std::max(strongest, apex[f].first);
      }
      for (std::size_t f = 0; f < F; ++f)
      {
        const auto [v, m] = apex[f];
        if (v <= 0 || v < 0.1f * strongest) { continue; }
        const double t = lib.fragment_mz[f0 + f];
        out.ms2_ppm.push_back({(fromFixed(m) - t) / t * 1e6, v});
      }
      // MS1 monoisotope at the MS1 scan nearest to the apex.
      const auto& ms1 = g.ms1;
      if (!ms1.rt.empty())
      {
        auto j = static_cast<std::size_t>(std::lower_bound(ms1.rt.begin(), ms1.rt.end(), out.anchor.run_rt) - ms1.rt.begin());
        if (j == ms1.rt.size() || (j > 0 && out.anchor.run_rt - ms1.rt[j - 1] < ms1.rt[j] - out.anchor.run_rt)) { --j; }
        const double t = lib.precursor_mz[p], tol = t * opt.search_ppm * 1e-6;
        const auto [v, m] = maxPeak(ms1.peaks, ms1.peaks.offset[j], ms1.peaks.offset[j + 1], toFixed(t - tol), toFixed(t + tol));
        if (v > 0) { out.ms1_ppm = {(fromFixed(m) - t) / t * 1e6, v}; }
      }
      return out;
    }
  }

  namespace
  {
    /// Examine the panel in parallel; results are returned in panel order (deterministic).
    std::vector<PanelResult> examinePanel(const Library& lib, const RunGrid& g, const CalibrationOptions& opt,
                                          const std::vector<std::uint32_t>& panel, const Criteria& crit,
                                          const RtMap* prior, double window_s)
    {
      std::vector<PanelResult> all;
      constexpr std::size_t chunk = 256;
      const std::size_t units = (panel.size() + chunk - 1) / chunk;
      orderedParallel(
        units, opt.threads, 4 * static_cast<std::size_t>(opt.threads),
        [&](std::size_t u) {
          std::vector<PanelResult> r;
          for (std::size_t i = u * chunk; i < std::min(panel.size(), (u + 1) * chunk); ++i)
          {
            std::size_t lo = 0, hi = g.cycles();
            if (prior)
            {
              const double t = (*prior)(lib.library_rt[panel[i]]);
              lo = g.lowerCycle(t - window_s);
              hi = g.upperCycle(t + window_s);
            }
            PanelResult x = examine(lib, g, opt, panel[i], lo, hi, crit);
            if (x.accepted) { r.push_back(std::move(x)); }
          }
          return r;
        },
        [&](std::vector<PanelResult>&& r) { for (auto& x : r) { all.push_back(std::move(x)); } },
        prior ? "calib.stage2_panel" : "calib.stage1_panel");
      return all;
    }

    /// Robust initial curve: per library-RT quantile bin, the run RT with the most anchors within
    /// +- bandwidth (true anchors cluster; false ones spread over the run). Anchors far from the
    /// curve through the bin modes are dropped.
    std::vector<std::pair<double, double>> modeFilter(const std::vector<Anchor>& a, int bins, double bandwidth,
                                                      double keep_s)
    {
      std::vector<std::pair<double, double>> pts;
      for (const auto& x : a) { pts.emplace_back(x.library_rt, x.run_rt); }
      std::sort(pts.begin(), pts.end());
      std::vector<std::pair<double, double>> modes;   // (median library RT of bin, run-RT mode)
      for (int b = 0; b < bins; ++b)
      {
        const std::size_t lo = pts.size() * b / bins, hi = pts.size() * (b + 1) / bins;
        if (hi - lo < 5) { continue; }
        std::vector<double> y;
        for (std::size_t i = lo; i < hi; ++i) { y.push_back(pts[i].second); }
        std::sort(y.begin(), y.end());
        std::size_t best = 0, best_n = 0, k = 0;
        for (std::size_t i = 0; i < y.size(); ++i)
        {
          while (y[i] - y[k] > 2 * bandwidth) { ++k; }
          if (i - k + 1 > best_n) { best_n = i - k + 1; best = (i + k) / 2; }
        }
        modes.emplace_back(pts[(lo + hi) / 2].first, y[best]);
      }
      std::vector<std::pair<double, double>> kept;
      for (const auto& [x, y] : pts)
      {
        // linear interpolation through the bin modes (constant beyond the ends)
        auto it = std::lower_bound(modes.begin(), modes.end(), std::make_pair(x, -std::numeric_limits<double>::infinity()));
        double m;
        if (it == modes.begin()) { m = modes.front().second; }
        else if (it == modes.end()) { m = modes.back().second; }
        else
        {
          const auto& [x0, y0] = *(it - 1);
          const auto& [x1, y1] = *it;
          m = x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y0;
        }
        if (std::fabs(y - m) <= keep_s) { kept.emplace_back(x, y); }
      }
      return kept;
    }

    /// LOWESS (or other model) with iterative rejection; marks `used` and residuals on the anchors.
    RtMap robustFit(std::vector<Anchor>& anchors, const CalibrationOptions& opt, double& sigma)
    {
      RtMap map;
      for (int round = 0; ; ++round)
      {
        std::vector<std::pair<double, double>> pts;
        for (const auto& a : anchors) { if (a.used) { pts.emplace_back(a.library_rt, a.run_rt); } }
        if (pts.size() < 10) { throw std::runtime_error("calibration: too few anchors survive rejection"); }
        map = RtMap::fromPoints(pts, opt.model, opt.lowess_span);
        std::vector<double> absres;
        for (auto& a : anchors)
        {
          a.residual = static_cast<float>(a.run_rt - map(a.library_rt));
          if (a.used) { absres.push_back(std::fabs(a.residual)); }
        }
        sigma = 1.4826 * quantile(absres, 0.5);
        if (round == opt.rejection_rounds) { break; }
        const double cut = std::max(3.0 * sigma, opt.min_reject_s);
        bool changed = false;
        for (auto& a : anchors)
        {
          const bool keep = std::fabs(a.residual) <= cut;
          changed |= keep != a.used;
          a.used = keep;
        }
        if (!changed) { break; }
      }
      return map;
    }
  }

  CalibrationResult calibrate(const Library& lib, const RunGrid& grid, const CalibrationOptions& opt)
  {
    const StageTime t0 = stageClock();
    CalibrationResult res;

    // Panel: targets with enough fragments, stratified over library RT by an even stride.
    std::vector<std::uint32_t> eligible;
    for (std::uint32_t p = 0; p < lib.size(); ++p)
    {
      if (!lib.decoy[p] && lib.fragmentsOf(p) >= static_cast<std::uint32_t>(opt.min_fragments)) { eligible.push_back(p); }
    }
    std::stable_sort(eligible.begin(), eligible.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return lib.library_rt[a] < lib.library_rt[b]; });
    std::vector<std::uint32_t> panel;
    const std::size_t n = std::min(opt.panel, eligible.size());
    for (std::size_t i = 0; i < n; ++i) { panel.push_back(eligible[i * eligible.size() / std::max<std::size_t>(1, n)]); }
    res.panel = panel.size();

    // Stage 1: whole run, strict criteria; robust initial curve from run-RT modes, then a fit.
    const Criteria strict{opt.strict_min_fragments, opt.strict_min_correlation, opt.strict_min_cosine};
    auto stage1 = examinePanel(lib, grid, opt, panel, strict, nullptr, 0.0);
    res.stage1_anchors = stage1.size();
    if (stage1.size() < 50)
    {
      throw std::runtime_error("calibration: only " + std::to_string(stage1.size()) + " stage-1 anchors from a panel of " +
                               std::to_string(res.panel) + "; supply -rt_anchors instead");
    }
    std::vector<Anchor> a1;
    for (const auto& x : stage1) { a1.push_back(x.anchor); }
    const auto kept = modeFilter(a1, opt.mode_bins, opt.mode_bandwidth_s, 4 * opt.mode_bandwidth_s);
    for (auto& a : a1)
    {
      a.used = std::binary_search(kept.begin(), kept.end(), std::make_pair(a.library_rt, a.run_rt));
    }
    double sigma1 = 0;
    RtMap map1 = robustFit(a1, opt, sigma1);
    res.stage1_used = static_cast<std::size_t>(std::count_if(a1.begin(), a1.end(), [](const Anchor& a) { return a.used; }));

    // Stage 2: around the stage-1 curve, relaxed criteria; final fit and mass offsets.
    res.stage2_window_s = std::max(4.0 * sigma1, opt.min_window_s);
    const Criteria relaxed{opt.min_fragments, opt.min_correlation, opt.min_library_cosine};
    auto stage2 = examinePanel(lib, grid, opt, panel, relaxed, &map1, res.stage2_window_s);
    std::vector<std::pair<double, double>> ms2_ppm, ms1_ppm;
    for (auto& x : stage2)
    {
      x.anchor.used = true;
      res.anchors.push_back(x.anchor);
    }
    if (res.anchors.size() < 50) { throw std::runtime_error("calibration: too few stage-2 anchors"); }
    double sigma2 = 0;
    res.rt_map = robustFit(res.anchors, opt, sigma2);
    // Mass errors from anchors that fit the RT model. The accuracy (spread) is taken from the anchors that also
    // pass the strict co-elution criteria when there are enough of them: with a full predicted library some
    // relaxed anchors are false (a co-eluting peak matched to an absent peptide), and their mass errors spread
    // over the whole search window -- harmless for the median offset, not for the 70th percentile.
    std::vector<std::pair<double, double>> ms2_strict, ms1_strict;
    for (std::size_t i = 0; i < stage2.size(); ++i)
    {
      if (!res.anchors[i].used) { continue; }
      ms2_ppm.insert(ms2_ppm.end(), stage2[i].ms2_ppm.begin(), stage2[i].ms2_ppm.end());
      if (stage2[i].ms1_ppm.second > 0) { ms1_ppm.push_back(stage2[i].ms1_ppm); }
      if (res.anchors[i].correlation >= opt.strict_min_correlation && res.anchors[i].cosine >= opt.strict_min_cosine)
      {
        ms2_strict.insert(ms2_strict.end(), stage2[i].ms2_ppm.begin(), stage2[i].ms2_ppm.end());
        if (stage2[i].ms1_ppm.second > 0) { ms1_strict.push_back(stage2[i].ms1_ppm); }
      }
    }
    std::vector<double> absres;
    for (const auto& a : res.anchors) { if (a.used) { absres.push_back(std::fabs(a.residual)); ++res.used; } }
    res.rt_residual_p50 = quantile(absres, 0.50);
    res.rt_residual_p95 = quantile(absres, 0.95);
    res.rt_residual_p99 = quantile(absres, 0.99);
    const Centre c2 = robustCentre(ms2_ppm), c1 = robustCentre(ms1_ppm);
    res.ms2_offset_ppm = c2.median; res.ms2_sigma_ppm = c2.sigma; res.ms2_accuracy_ppm = c2.accuracy;
    res.ms1_offset_ppm = c1.median; res.ms1_sigma_ppm = c1.sigma; res.ms1_accuracy_ppm = c1.accuracy;
    constexpr std::size_t min_strict = 200;
    if (ms1_strict.size() >= min_strict)
    {
      // spread around the offset of all anchors
      for (auto& e : ms2_strict) { e.first -= res.ms2_offset_ppm; }
      for (auto& e : ms1_strict) { e.first -= res.ms1_offset_ppm; }
      res.ms2_accuracy_ppm = 5.0 * robustAbsQuantile(ms2_strict, 0.7);
      res.ms1_accuracy_ppm = 5.0 * robustAbsQuantile(ms1_strict, 0.7);
      res.strict_mass_anchors = ms1_strict.size();
    }
    res.ms2_mass_points = ms2_ppm.size();
    res.ms1_mass_points = ms1_ppm.size();
    res.time = stageClock() - t0;
    return res;
  }

  void writeAnchors(const std::string& filename, const Library& lib, const CalibrationResult& r)
  {
    std::ofstream out(filename);
    if (!out) { throw std::runtime_error("cannot write " + filename); }
    out << "library_rt\trun_rt\tprecursor_id\tapex_cycle\tcorrelation\tlibrary_cosine\tresidual\tused\n";
    out.precision(10);
    for (const auto& a : r.anchors)
    {
      out << a.library_rt << '\t' << a.run_rt << '\t' << lib.precursor_id[a.precursor] << '\t' << a.apex_cycle << '\t'
          << a.correlation << '\t' << a.cosine << '\t' << a.residual << '\t' << (a.used ? 1 : 0) << '\n';
    }
  }
}
