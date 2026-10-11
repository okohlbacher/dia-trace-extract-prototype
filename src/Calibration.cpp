// ODIA-V3 trace extractor -- self-calibration of RT and m/z from the run grid (no external search).
#include "Calibration.h"

#include "Pipeline.h"
#include "Profile.h"

#include <algorithm>
#include <cstdlib>
#include <omp.h>
#include <cstring>
#include <new>
#include <memory>
#include <functional>
#include <sys/mman.h>
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
      const MzFixed* mz = p.mz;
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

    /// Offset and extraction tolerance from the apex mass errors (ppm, relative to the uncorrected targets) found in
    /// the search window. Unweighted (the intensities are ignored: strong centroids drift upward on Astral, which
    /// biased a weighted median). Offset = median; sigma = 1.4826 x the median absolute deviation (robust to the
    /// random centroids of false anchors); accuracy = `k` sigma. Both scale with the instrument's error width; the
    /// multiple k is the only fixed constant (6: see docs/analysis/trace_quality.md, 5 reproduces DIA-NN's rule).
    struct MassModel
    {
      double offset = 0, sigma = 0, accuracy = 0;
      std::size_t n = 0;
    };
    MassModel massModel(const std::vector<std::pair<double, double>>& v, double k)
    {
      MassModel m;
      if (v.empty()) { return m; }
      std::vector<double> x;
      x.reserve(v.size());
      for (const auto& [val, w] : v) { x.push_back(val); }
      std::sort(x.begin(), x.end());
      const auto q = [](const std::vector<double>& s, double f) { return s[static_cast<std::size_t>(f * (s.size() - 1) + 0.5)]; };
      m.offset = q(x, 0.5);
      m.n = x.size();
      for (auto& val : x) { val = std::fabs(val - m.offset); }
      std::sort(x.begin(), x.end());
      m.sigma = 1.4826 * q(x, 0.5);
      m.accuracy = std::max(k * m.sigma, 0.1);   // floor: the fixed-point m/z grid (1e-5 Th) resolves ~0.01 ppm at 1000 Th
      return m;
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
      std::vector<std::pair<double, double>> ms2_ppm;  ///< (ppm, intensity) of the fragments over the co-elution window
      std::vector<std::pair<double, double>> ms1_ppm;  ///< (ppm, intensity) of M+0 at the MS1 scans of that window
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

    /// Where to look for the centroids: the targets shifted by `offset` ppm, +- `width` ppm.
    struct MassSearch
    {
      double ms2_offset = 0, ms2_width = 0, ms1_offset = 0, ms1_width = 0;
    };

    /// Fragment m/z bounds of panel precursor p (the wide search window).
    std::vector<std::pair<MzFixed, MzFixed>> fragmentBounds(const Library& lib, const MassSearch& ms, std::uint32_t p, std::size_t F)
    {
      std::vector<std::pair<MzFixed, MzFixed>> bounds(F);
      const std::uint32_t f0 = lib.fragment_begin[p];
      for (std::size_t f = 0; f < F; ++f)
      {
        const double t = lib.fragment_mz[f0 + f] * (1.0 + ms.ms2_offset * 1e-6), tol = t * ms.ms2_width * 1e-6;
        bounds[f] = {toFixed(t - tol), toFixed(t + tol)};
      }
      return bounds;
    }

    /// Score the candidate apexes of panel precursor p from its fragment traces `tile` (F x C, row-major) and their
    /// per-cycle sum (summed in fragment order); apex mass errors and the MS1 error of an accepted anchor.
    PanelResult scoreTile(const Library& lib, const RunGrid& g, const CalibrationOptions& opt, std::uint32_t p,
                          const WindowLane& lane, std::size_t F, const std::vector<std::pair<MzFixed, MzFixed>>& bounds,
                          const float* tile, const float* sum, std::size_t c_lo, std::size_t c_hi, const Criteria& crit,
                          const MassSearch& ms)
    {
      PanelResult out;
      const std::size_t C = g.cycles();
      const std::uint32_t f0 = lib.fragment_begin[p];
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

      // Mass errors at the apex: observed m/z of the most intense centroid per fragment (random centroids inside the
      // search window are the background the robust statistics of massModel tolerate; calibration:min_relative_intensity
      // can additionally drop weak fragments).
      const std::size_t c = out.anchor.apex_cycle;
      const std::size_t a = lane.peaks.cellBegin(c), b = lane.peaks.cellEnd(c);
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
        if (v <= 0 || v < static_cast<float>(opt.min_relative_intensity) * strongest) { continue; }
        const double t = lib.fragment_mz[f0 + f];
        out.ms2_ppm.push_back({(fromFixed(m) - t) / t * 1e6, v});
      }
      // MS1 monoisotope at the MS1 scan nearest to the apex.
      const auto& ms1 = g.ms1;
      if (!ms1.rt.empty())
      {
        auto j = static_cast<std::size_t>(std::lower_bound(ms1.rt.begin(), ms1.rt.end(), out.anchor.run_rt) - ms1.rt.begin());
        if (j == ms1.rt.size() || (j > 0 && out.anchor.run_rt - ms1.rt[j - 1] < ms1.rt[j] - out.anchor.run_rt)) { --j; }
        const double t = lib.precursor_mz[p], tc = t * (1.0 + ms.ms1_offset * 1e-6), tol = tc * ms.ms1_width * 1e-6;
        const auto [v, m] = maxPeak(ms1.peaks, ms1.peaks.cellBegin(j), ms1.peaks.cellEnd(j), toFixed(tc - tol), toFixed(tc + tol));
        if (v > 0) { out.ms1_ppm.push_back({(fromFixed(m) - t) / t * 1e6, v}); }
      }
      return out;
    }

    /// Look for a co-eluting apex of panel precursor p within global cycles [c_lo, c_hi).
    PanelResult examine(const Library& lib, const RunGrid& g, const CalibrationOptions& opt, std::uint32_t p,
                        std::size_t c_lo, std::size_t c_hi, const Criteria& crit, const MassSearch& ms)
    {
      PanelResult out;
      const std::size_t w = nearestWindow(g, lib.precursor_mz[p]);
      if (w == g.windows.size()) { return out; }
      const auto& lane = g.windows[w];
      const std::size_t C = g.cycles();
      const std::size_t F = std::min<std::size_t>(lib.fragmentsOf(p), static_cast<std::size_t>(opt.top_fragments));
      if (F < static_cast<std::size_t>(crit.min_fragments) || c_hi <= c_lo) { return out; }

      // Whole-run traces of the top fragments (max centroid within the wide window).
      std::vector<float> tile(F * C, 0.0f);
      const auto bounds = fragmentBounds(lib, ms, p, F);
      std::vector<float> sum(C, 0.0f);
      // Gather the range plus the co-elution margin so windows at its edges are complete.
      const std::size_t g_lo = c_lo > static_cast<std::size_t>(opt.half_window_cycles) ? c_lo - opt.half_window_cycles : 0;
      const std::size_t g_hi = std::min(C, c_hi + opt.half_window_cycles);
      for (std::size_t c = g_lo; c < g_hi; ++c)
      {
        const std::size_t a = lane.peaks.cellBegin(c), b = lane.peaks.cellEnd(c);
        if (a == b) { continue; }
        for (std::size_t f = 0; f < F; ++f)
        {
          const float v = maxPeak(lane.peaks, a, b, bounds[f].first, bounds[f].second).first;
          tile[f * C + c] = v;
          sum[c] += v;
        }
      }

      return scoreTile(lib, g, opt, p, lane, F, bounds, tile.data(), sum.data(), c_lo, c_hi, crit, ms);
    }
  }

  namespace
  {
    /// Stage 1 (whole run): one task per isolation window. The fragment bounds of all panel precursors in the window
    /// are sorted by lower edge, so in every spectrum one forward pointer serves all of them (a merge join instead of a
    /// binary search per fragment and spectrum). Same traces as examine() (same maximum, same tie rule, sums in
    /// fragment order), hence the same anchors; results in panel order.
    std::vector<PanelResult> examineWholeRun(const Library& lib, const RunGrid& g, const CalibrationOptions& opt,
                                             const std::vector<std::uint32_t>& panel, const Criteria& crit, const MassSearch& ms)
    {
      static prof::Counter& c_win = prof::counter("calib.stage1_window");
      const std::size_t C = g.cycles(), W = g.windows.size();
      std::vector<std::vector<std::size_t>> by_window(W);   // panel positions per window
      for (std::size_t i = 0; i < panel.size(); ++i)
      {
        const std::size_t w = nearestWindow(g, lib.precursor_mz[panel[i]]);
        const std::size_t F = std::min<std::size_t>(lib.fragmentsOf(panel[i]), static_cast<std::size_t>(opt.top_fragments));
        if (w < W && F >= static_cast<std::size_t>(crit.min_fragments) && C > 0) { by_window[w].push_back(i); }
      }
      // Largest windows first (load balance); results are put back into panel order below.
      std::vector<std::size_t> order(W);
      std::iota(order.begin(), order.end(), 0);
      std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return by_window[a].size() > by_window[b].size(); });
      std::vector<std::vector<std::pair<std::size_t, PanelResult>>> found(W);
      const int threads = std::max(1, std::min(opt.threads, 32));   // a moderate pool: per-window work is memory-bound
      // One reusable trace buffer per thread, mapped directly rather than malloc'd: freeing a multi-MB malloc'd block
      // raises glibc's dynamic mmap threshold, after which the extraction's large buffers stay in the arenas (+0.5 GiB
      // peak RSS measured); a fresh mapping per window instead pays ~4.7 GB of zero-page faults. Unmapped on return.
      struct TileBuffer
      {
        void* p = nullptr;
        std::size_t bytes = 0;
        float* get(std::size_t n)
        {
          const std::size_t need = std::max<std::size_t>(n, 1) * sizeof(float);
          if (need > bytes)
          {
            if (p) { ::munmap(p, bytes); }
            bytes = std::max(need, 2 * bytes);
            p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) { p = nullptr; bytes = 0; throw std::bad_alloc(); }
          }
          std::memset(p, 0, n * sizeof(float));
          return static_cast<float*>(p);
        }
        ~TileBuffer() { if (p) { ::munmap(p, bytes); } }
      };
      std::vector<TileBuffer> buffers(static_cast<std::size_t>(threads));
#pragma omp parallel for schedule(dynamic, 1) num_threads(threads)
      for (std::ptrdiff_t k = 0; k < static_cast<std::ptrdiff_t>(W); ++k)
      {
        const std::size_t w = order[static_cast<std::size_t>(k)];
        const auto& members = by_window[w];
        if (members.empty()) { continue; }
        prof::Scope sc(c_win);
        const auto& lane = g.windows[w];
        struct Bound { MzFixed lo, hi; std::uint32_t row; };   // row = trace row in `tiles`
        std::vector<Bound> bs;
        std::vector<std::size_t> first_row(members.size() + 1, 0), Fs(members.size());
        std::vector<std::vector<std::pair<MzFixed, MzFixed>>> bounds(members.size());
        for (std::size_t m = 0; m < members.size(); ++m)
        {
          const std::uint32_t p = panel[members[m]];
          Fs[m] = std::min<std::size_t>(lib.fragmentsOf(p), static_cast<std::size_t>(opt.top_fragments));
          bounds[m] = fragmentBounds(lib, ms, p, Fs[m]);
          first_row[m + 1] = first_row[m] + Fs[m];
          for (std::size_t f = 0; f < Fs[m]; ++f)
          {
            bs.push_back({bounds[m][f].first, bounds[m][f].second, static_cast<std::uint32_t>(first_row[m] + f)});
          }
        }
        std::sort(bs.begin(), bs.end(), [](const Bound& a, const Bound& b) { return a.lo < b.lo || (a.lo == b.lo && a.row < b.row); });
        float* tiles = buffers[static_cast<std::size_t>(omp_get_thread_num())].get(first_row.back() * C);
        const MzFixed* mz = lane.peaks.mz;
        const float* in = lane.peaks.intensity;
        for (std::size_t c = 0; c < C; ++c)
        {
          const std::size_t a = lane.peaks.cellBegin(c), b = lane.peaks.cellEnd(c);
          if (a == b) { continue; }
          std::size_t i = a;
          for (const Bound& bd : bs)
          {
            while (i < b && mz[i] < bd.lo) { ++i; }   // = lower_bound(lo): lo is non-decreasing over bs
            if (i == b) { break; }                     // all later bounds start at or above the last peak
            float best = 0.0f;
            for (std::size_t j = i; j < b && mz[j] <= bd.hi; ++j) { if (in[j] > best) { best = in[j]; } }
            tiles[bd.row * C + c] = best;
          }
        }
        std::vector<float> sum(C);
        for (std::size_t m = 0; m < members.size(); ++m)
        {
          const float* tile = tiles + first_row[m] * C;
          std::fill(sum.begin(), sum.end(), 0.0f);
          for (std::size_t c = 0; c < C; ++c)
          {
            if (lane.peaks.cellBegin(c) == lane.peaks.cellEnd(c)) { continue; }
            for (std::size_t f = 0; f < Fs[m]; ++f) { sum[c] += tile[f * C + c]; }
          }
          PanelResult x = scoreTile(lib, g, opt, panel[members[m]], lane, Fs[m], bounds[m], tile, sum.data(), 0, C, crit, ms);
          if (x.accepted) { found[w].emplace_back(members[m], std::move(x)); }
        }
      }
      std::vector<std::pair<std::size_t, PanelResult>> flat;
      for (auto& v : found) { for (auto& x : v) { flat.push_back(std::move(x)); } }
      std::sort(flat.begin(), flat.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      std::vector<PanelResult> all;
      all.reserve(flat.size());
      for (auto& x : flat) { all.push_back(std::move(x.second)); }
      return all;
    }

    /// Examine the panel in parallel; results are returned in panel order (deterministic).
    std::vector<PanelResult> examinePanel(const Library& lib, const RunGrid& g, const CalibrationOptions& opt,
                                          const std::vector<std::uint32_t>& panel, const Criteria& crit, const MassSearch& ms,
                                          const RtMap* prior, double window_s)
    {
      std::vector<PanelResult> all;
      if (!prior) { return examineWholeRun(lib, g, opt, panel, crit, ms); }
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
            PanelResult x = examine(lib, g, opt, panel[i], lo, hi, crit, ms);
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
      if (std::count_if(anchors.begin(), anchors.end(), [](const Anchor& a) { return a.used; }) < 10)
      {
        throw std::runtime_error("calibration: fewer than 10 usable anchors; supply -rt_anchors instead");
      }
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
    const std::size_t want = opt.panel > 0 ? opt.panel : static_cast<std::size_t>(std::llround(opt.panel_fraction * static_cast<double>(eligible.size())));
    const std::size_t n = std::min(want, eligible.size());
    for (std::size_t i = 0; i < n; ++i) { panel.push_back(eligible[i * eligible.size() / std::max<std::size_t>(1, n)]); }
    res.panel = panel.size();

    // Stage 1: whole run, strict criteria; robust initial curve from run-RT modes, then a fit.
    const Criteria strict{opt.strict_min_fragments, opt.strict_min_correlation, opt.strict_min_cosine};
    MassSearch search{0.0, opt.search_ppm, 0.0, opt.search_ppm};
    auto stage1 = examinePanel(lib, grid, opt, panel, strict, search, nullptr, 0.0);
    res.stage1_anchors = stage1.size();
    if (stage1.size() < 50)
    {
      throw std::runtime_error("calibration: only " + std::to_string(stage1.size()) + " stage-1 anchors from a panel of " +
                               std::to_string(res.panel) + "; supply -rt_anchors instead");
    }
    std::vector<Anchor> a1;
    for (const auto& x : stage1) { a1.push_back(x.anchor); }
    // Bins need a handful of anchors each; with few anchors (small panel or library) the mode filter is skipped.
    const int bins = std::max(1, std::min(opt.mode_bins, static_cast<int>(a1.size() / 10)));
    const auto kept = modeFilter(a1, bins, opt.mode_bandwidth_s, 4 * opt.mode_bandwidth_s);
    const bool filter = kept.size() >= 20;
    for (auto& a : a1)
    {
      a.used = !filter || std::binary_search(kept.begin(), kept.end(), std::make_pair(a.library_rt, a.run_rt));
    }
    double sigma1 = 0;
    RtMap map1 = robustFit(a1, opt, sigma1);
    res.stage1_used = static_cast<std::size_t>(std::count_if(a1.begin(), a1.end(), [](const Anchor& a) { return a.used; }));

    // Mass errors of the anchors used in a fit; the mass model (offset, background, tolerance) per level.
    // Mass errors of the anchors used in a fit. With a full predicted library many relaxed anchors are false (a
    // co-eluting peak matched to an absent peptide) and their centroids are random: a background too dense for
    // the tail of the true matches to be measured. So the mass model takes the anchors that pass the strict
    // co-elution criteria when there are at least 200 of them (a sample-size floor), else all used anchors.
    std::size_t mass_anchors = 0;
    const auto errors = [&](const std::vector<PanelResult>& results, const std::vector<Anchor>& anchors,
                            std::vector<std::pair<double, double>>& ms2, std::vector<std::pair<double, double>>& ms1) {
      const auto strict_ok = [&](const Anchor& a) {
        return a.correlation >= opt.strict_min_correlation && a.cosine >= opt.strict_min_cosine;
      };
      std::size_t n_strict = 0;
      for (const auto& a : anchors) { n_strict += a.used && strict_ok(a); }
      const bool strict_only = n_strict >= 200;
      ms2.clear(); ms1.clear(); mass_anchors = 0;
      for (std::size_t i = 0; i < results.size(); ++i)
      {
        if (!anchors[i].used || (strict_only && !strict_ok(anchors[i]))) { continue; }
        ++mass_anchors;
        ms2.insert(ms2.end(), results[i].ms2_ppm.begin(), results[i].ms2_ppm.end());
        ms1.insert(ms1.end(), results[i].ms1_ppm.begin(), results[i].ms1_ppm.end());
      }
    };
    // The next pass searches +- factor x the measured accuracy around the measured offset.
    const auto narrowed = [&](const MassModel& m, double previous_width) {
      return m.n > 0 ? std::min(opt.search_ppm, opt.search_narrow_factor * m.accuracy) : previous_width;
    };
    std::vector<std::pair<double, double>> ms2_ppm, ms1_ppm;
    errors(stage1, a1, ms2_ppm, ms1_ppm);
    MassModel m2 = massModel(ms2_ppm, opt.accuracy_sigma);
    MassModel m1 = massModel(ms1_ppm, opt.accuracy_sigma);

    // Stage 2: around the stage-1 curve, relaxed criteria, the mass search narrowed around the stage-1 offsets;
    // repeated while the measured window still changes (at most search_rounds passes). Final fit and offsets.
    res.stage2_window_s = std::max(4.0 * sigma1, opt.min_window_s);
    const Criteria relaxed{opt.min_fragments, opt.min_correlation, opt.min_library_cosine};
    std::vector<PanelResult> stage2;
    for (int round = 0; round < std::max(1, opt.search_rounds); ++round)
    {
      const MassSearch next{m2.offset, narrowed(m2, search.ms2_width), m1.offset, narrowed(m1, search.ms1_width)};
      if (round > 0 && std::fabs(next.ms2_width - search.ms2_width) < 0.25 * search.ms2_width
          && std::fabs(next.ms1_width - search.ms1_width) < 0.25 * search.ms1_width) { break; }
      search = next;
      ++res.search_rounds;
      stage2 = examinePanel(lib, grid, opt, panel, relaxed, search, &map1, res.stage2_window_s);
      res.anchors.clear();
      for (auto& x : stage2)
      {
        x.anchor.used = true;
        res.anchors.push_back(x.anchor);
      }
      if (res.anchors.size() < 50) { throw std::runtime_error("calibration: too few stage-2 anchors"); }
      double sigma2 = 0;
      res.rt_map = robustFit(res.anchors, opt, sigma2);
      errors(stage2, res.anchors, ms2_ppm, ms1_ppm);
      m2 = massModel(ms2_ppm, opt.accuracy_sigma);
      m1 = massModel(ms1_ppm, opt.accuracy_sigma);
    }
    if (const char* dump = std::getenv("ODIA_DUMP_MASS_ERRORS"))
    {
      // Diagnostic: the apex mass errors behind the mass model (ppm, intensity), one line per centroid.
      std::ofstream o(dump);
      o << "level\tppm\tintensity\n";
      for (const auto& [e, w] : ms2_ppm) { o << "2\t" << e << '\t' << w << '\n'; }
      for (const auto& [e, w] : ms1_ppm) { o << "1\t" << e << '\t' << w << '\n'; }
    }
    std::vector<double> absres;
    res.used = 0;
    for (const auto& a : res.anchors) { if (a.used) { absres.push_back(std::fabs(a.residual)); ++res.used; } }
    res.rt_residual_p50 = quantile(absres, 0.50);
    res.rt_residual_p95 = quantile(absres, 0.95);
    res.rt_residual_p99 = quantile(absres, 0.99);
    res.ms2_offset_ppm = m2.offset; res.ms2_sigma_ppm = m2.sigma; res.ms2_accuracy_ppm = m2.accuracy;
    res.ms1_offset_ppm = m1.offset; res.ms1_sigma_ppm = m1.sigma; res.ms1_accuracy_ppm = m1.accuracy;
    res.ms2_search_ppm = search.ms2_width; res.ms1_search_ppm = search.ms1_width;
    res.strict_mass_anchors = mass_anchors;
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
