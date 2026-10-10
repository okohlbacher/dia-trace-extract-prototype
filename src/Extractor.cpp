// ODIA-V3 trace extractor -- gathers traces from the run grid.
#include "Extractor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace ODIA
{
  namespace
  {
    constexpr double C13_C12 = 1.0033548378; // OpenMS Constants::C13C12_MASSDIFF_U

    struct Bounds { MzFixed lo, hi; };
    Bounds bounds(double target, double ppm)
    {
      const double tol = target * ppm * 1e-6;
      return {toFixed(std::max(0.0, target - tol)), toFixed(target + tol)};
    }
  }

  Extractor::Extractor(const Library& lib, const RtMap& rt_map, const RunGrid& grid, const ExtractOptions& opt)
    : lib_(lib), grid_(grid), opt_(opt)
  {
    stats_.precursors = lib.size();
    const auto& win = grid.windows;
    for (std::uint32_t p = 0; p < lib.size(); ++p)
    {
      const double mz = lib.precursor_mz[p];
      const double rt = rt_map(lib.library_rt[p]);
      std::int64_t best = -1;
      double best_dist = std::numeric_limits<double>::infinity();
      for (std::size_t w = 0; w < win.size(); ++w)
      {
        if (mz < win[w].lower || mz > win[w].upper) { continue; }
        if (opt_.assign_all_windows) { assignments_.push_back({p, static_cast<std::uint32_t>(w), rt}); continue; }
        const double d = std::fabs(mz - win[w].target);
        if (d < best_dist) { best_dist = d; best = static_cast<std::int64_t>(w); }
      }
      if (!opt_.assign_all_windows)
      {
        if (best < 0) { ++stats_.without_window; continue; }
        assignments_.push_back({p, static_cast<std::uint32_t>(best), rt});
      }
    }
    if (opt_.assign_all_windows)
    {
      std::vector<bool> any(lib.size(), false);
      for (const auto& a : assignments_) { any[a.precursor] = true; }
      stats_.without_window = static_cast<std::uint64_t>(std::count(any.begin(), any.end(), false));
    }
    // Deterministic order: window, then library index. Units never straddle a window, so a unit
    // touches exactly one lane of the grid (cache locality, one owner).
    std::stable_sort(assignments_.begin(), assignments_.end(), [](const Assignment& a, const Assignment& b) {
      return a.window != b.window ? a.window < b.window : a.precursor < b.precursor;
    });
    stats_.assignments = assignments_.size();
    const std::size_t step = std::max<std::size_t>(1, opt_.unit_precursors);
    for (std::size_t b = 0; b < assignments_.size();)
    {
      std::size_t e = b;
      while (e < assignments_.size() && e - b < step && assignments_[e].window == assignments_[b].window) { ++e; }
      units_.push_back({b, e});
      b = e;
    }
    stats_.units = units_.size();
  }

  float Extractor::aggregate_(const MzFixed* mz, const float* in, std::size_t a, std::size_t b, MzFixed lo,
                              MzFixed hi, std::size_t& pos) const
  {
    // Fragments are visited in ascending m/z per cell, so the search resumes where the last one began.
    pos = static_cast<std::size_t>(std::lower_bound(mz + std::max(pos, a), mz + b, lo) - mz);
    float v = 0.0f;
    if (opt_.aggregate_sum)
    {
      for (std::size_t i = pos; i < b && mz[i] <= hi; ++i) { v += in[i]; }
    }
    else
    {
      for (std::size_t i = pos; i < b && mz[i] <= hi; ++i) { v = std::max(v, in[i]); }
    }
    return v;
  }

  TraceBlock Extractor::extract(std::size_t unit) const
  {
    const Unit& u = units_[unit];
    TraceBlock out;
    out.unit = unit;
    std::uint64_t& lookups = out.lookups;
    const bool whole = opt_.rt_half_window <= 0.0;
    const std::size_t n_cycles = grid_.cycles();
    const double ms2_scale = 1.0 + opt_.ms2_ppm_offset * 1e-6;
    const double ms1_scale = 1.0 + opt_.ms1_ppm_offset * 1e-6;
    const int n_iso = opt_.extract_ms1 ? opt_.ms1_isotopes + 1 : 0;
    const auto& ms1 = grid_.ms1;

    std::vector<std::pair<Bounds, std::uint32_t>> frag;   // (bounds, row) sorted by m/z
    std::vector<Bounds> iso;
    std::vector<float> ms1_at;                            // [ms1 spectrum][isotope]

    for (std::size_t k = u.begin; k < u.end; ++k)
    {
      const Assignment& as = assignments_[k];
      const std::uint32_t p = as.precursor;
      const std::size_t lo = whole ? 0 : grid_.lowerCycle(as.rt_center - opt_.rt_half_window);
      const std::size_t hi = whole ? n_cycles : grid_.upperCycle(as.rt_center + opt_.rt_half_window);
      TraceRecord r;
      r.precursor = p;
      r.window = static_cast<std::int32_t>(as.window);
      r.rt_center = static_cast<float>(as.rt_center);
      r.cycle_begin = static_cast<std::uint32_t>(lo);
      r.cycles = static_cast<std::uint32_t>(hi > lo ? hi - lo : 0);
      r.fragments = static_cast<std::uint16_t>(lib_.fragmentsOf(p));
      r.isotopes = static_cast<std::uint8_t>(n_iso);
      r.value_offset = out.values.size();
      if (r.cycles == 0)
      {
        out.records.push_back(r); // RT window outside the acquisition: an empty record keeps accounting exact
        continue;
      }
      const std::size_t T = r.cycles;
      out.values.resize(out.values.size() + (r.fragments + r.isotopes) * T, 0.0f);
      float* v = out.values.data() + r.value_offset;

      // --- MS2: one tile, F x T, gathered cell by cell from this window's lane.
      frag.clear();
      for (std::uint32_t f = lib_.fragment_begin[p], row = 0; f < lib_.fragment_begin[p + 1]; ++f, ++row)
      {
        frag.push_back({bounds(lib_.fragment_mz[f] * ms2_scale, opt_.ms2_ppm), row});
      }
      std::sort(frag.begin(), frag.end(), [](const auto& a, const auto& b) { return a.first.lo < b.first.lo; });
      const auto& lane = grid_.windows[as.window];
      const MzFixed* mz = lane.peaks.mz.data();
      const float* in = lane.peaks.intensity.data();
      for (std::size_t c = lo; c < hi; ++c)
      {
        const std::size_t a = lane.peaks.offset[c], b = lane.peaks.offset[c + 1];
        if (a == b) { continue; }
        std::size_t pos = a;
        for (const auto& [bd, row] : frag) { v[row * T + (c - lo)] = aggregate_(mz, in, a, b, bd.lo, bd.hi, pos); }
        lookups += frag.size();
      }

      // --- MS1: isotopes at the MS1 scans bracketing the cycle range, interpolated linearly
      // onto the global axis (MS1 is not acquired once per cycle).
      if (n_iso > 0 && !ms1.rt.empty())
      {
        const double t0 = grid_.cycle_rt[lo], t1 = grid_.cycle_rt[hi - 1];
        auto j0 = static_cast<std::size_t>(std::upper_bound(ms1.rt.begin(), ms1.rt.end(), t0) - ms1.rt.begin());
        j0 = j0 > 0 ? j0 - 1 : 0;
        auto j1 = static_cast<std::size_t>(std::lower_bound(ms1.rt.begin(), ms1.rt.end(), t1) - ms1.rt.begin());
        j1 = std::min(j1, ms1.rt.size() - 1);
        const double z = opt_.isotope_unit_spacing ? 1.0 : std::max<int>(1, lib_.charge[p]);
        iso.clear();
        for (int i = 0; i < n_iso; ++i)
        {
          iso.push_back(bounds((lib_.precursor_mz[p] + i * C13_C12 / z) * ms1_scale, opt_.ms1_ppm));
        }
        const std::size_t J = j1 - j0 + 1;
        ms1_at.assign(J * n_iso, 0.0f);
        for (std::size_t j = j0; j <= j1; ++j)
        {
          const std::size_t a = ms1.peaks.offset[j], b = ms1.peaks.offset[j + 1];
          std::size_t pos = a;
          for (int i = 0; i < n_iso; ++i)
          {
            ms1_at[(j - j0) * n_iso + i] = aggregate_(ms1.peaks.mz.data(), ms1.peaks.intensity.data(), a, b,
                                                      iso[i].lo, iso[i].hi, pos);
          }
          lookups += n_iso;
        }
        std::size_t j = j0;
        for (std::size_t c = lo; c < hi; ++c)
        {
          const double t = grid_.cycle_rt[c];
          while (j + 1 <= j1 && ms1.rt[j + 1] <= t) { ++j; }
          const std::size_t ja = j, jb = std::min(j + 1, j1);
          double w = 0.0;
          if (jb != ja && ms1.rt[jb] > ms1.rt[ja]) { w = std::clamp((t - ms1.rt[ja]) / (ms1.rt[jb] - ms1.rt[ja]), 0.0, 1.0); }
          for (int i = 0; i < n_iso; ++i)
          {
            const float ya = ms1_at[(ja - j0) * n_iso + i], yb = ms1_at[(jb - j0) * n_iso + i];
            v[(r.fragments + i) * T + (c - lo)] = static_cast<float>(ya + w * (yb - ya));
          }
        }
      }
      out.records.push_back(r);
    }
    return out;
  }

  void Extractor::account(const std::vector<TraceRecord>& records, std::uint64_t lookups)
  {
    stats_.peak_lookups += lookups;
    for (const auto& r : records)
    {
      stats_.traces += r.fragments + r.isotopes;
      stats_.ms2_points += static_cast<std::uint64_t>(r.fragments) * r.cycles;
      stats_.ms1_points += static_cast<std::uint64_t>(r.isotopes) * r.cycles;
      stats_.empty_range += r.cycles == 0;
    }
  }
}
