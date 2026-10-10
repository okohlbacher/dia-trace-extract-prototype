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

  Extractor::Extractor(const RtMap& rt_map, const RunGrid& grid, const ExtractOptions& opt)
    : rt_map_(rt_map), grid_(grid), opt_(opt)
  {
  }

  std::pair<std::size_t, std::size_t> Extractor::ms1Range_(double rt_center, std::size_t lo, std::size_t hi) const
  {
    if (!opt_.ms1_scan_axis) { return {lo, hi}; }
    const auto& t = grid_.ms1.rt;
    if (hi <= lo || t.empty()) { return {0, 0}; }
    if (opt_.rt_half_window <= 0.0) { return {0, t.size()}; }
    const auto b = std::lower_bound(t.begin(), t.end(), rt_center - opt_.rt_half_window) - t.begin();
    const auto e = std::upper_bound(t.begin(), t.end(), rt_center + opt_.rt_half_window) - t.begin();
    return {static_cast<std::size_t>(b), static_cast<std::size_t>(std::max(b, e))};
  }

  Extractor::Plan Extractor::plan(const Library& lib) const
  {
    Plan plan;
    auto& assignments = plan.assignments;
    plan.precursors = lib.size();
    const auto& win = grid_.windows;
    for (std::uint32_t p = 0; p < lib.size(); ++p)
    {
      const double mz = lib.precursor_mz[p];
      const double rt = rt_map_(lib.library_rt[p]);
      std::int64_t best = -1;
      double best_dist = std::numeric_limits<double>::infinity();
      bool any = false;
      for (std::size_t w = 0; w < win.size(); ++w)
      {
        if (mz < win[w].lower || mz > win[w].upper) { continue; }
        if (opt_.assign_all_windows) { assignments.push_back({p, static_cast<std::uint32_t>(w), rt}); any = true; continue; }
        const double d = std::fabs(mz - win[w].target);
        if (d < best_dist) { best_dist = d; best = static_cast<std::int64_t>(w); }
      }
      if (opt_.assign_all_windows) { plan.without_window += !any; continue; }
      if (best < 0) { ++plan.without_window; continue; }
      assignments.push_back({p, static_cast<std::uint32_t>(best), rt});
    }
    // Deterministic order: window, then library index. Units never straddle a window, so a unit
    // touches exactly one lane of the grid (cache locality, one owner).
    std::stable_sort(assignments.begin(), assignments.end(), [](const Assignment& a, const Assignment& b) {
      return a.window != b.window ? a.window < b.window : a.precursor < b.precursor;
    });
    const std::size_t step = std::max<std::size_t>(1, opt_.unit_precursors);
    for (std::size_t b = 0; b < assignments.size();)
    {
      std::size_t e = b;
      while (e < assignments.size() && e - b < step && assignments[e].window == assignments[b].window) { ++e; }
      Unit unit{b, e, 0};
      if (opt_.block_floats > 0)
      {
        thread_local std::vector<std::size_t> runs;
        runs.clear();
        const std::size_t n_iso = opt_.extract_ms1 ? static_cast<std::size_t>(opt_.ms1_isotopes) + 1 : 0;
        for (std::size_t k = b; k < e; ++k)
        {
          const auto [lo, hi] = cycleRange_(assignments[k].rt_center);
          const auto [m0, m1] = ms1Range_(assignments[k].rt_center, lo, hi);
          runs.push_back(hi > lo ? lib.fragmentsOf(assignments[k].precursor) * (hi - lo) + n_iso * (m1 - m0) : 0);
        }
        unit.blocks = TraceArena::blocksFor(runs, opt_.block_floats);
      }
      plan.units.push_back(unit);
      b = e;
    }
    return plan;
  }

  void Extractor::account(const Plan& plan)
  {
    stats_.precursors += plan.precursors;
    stats_.without_window += plan.without_window;
    stats_.assignments += plan.assignments.size();
    stats_.units += plan.units.size();
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

  std::pair<std::size_t, std::size_t> Extractor::cycleRange_(double rt_center) const
  {
    if (opt_.rt_half_window <= 0.0) { return {0, grid_.cycles()}; }
    return {grid_.lowerCycle(rt_center - opt_.rt_half_window), grid_.upperCycle(rt_center + opt_.rt_half_window)};
  }

  void Extractor::extract(const Library& lib, const Plan& plan, std::size_t unit, TraceBlock& out) const
  {
    const Unit& u = plan.units[unit];
    out.unit = unit;
    out.records.clear();
    out.lookups = 0;
    std::uint64_t& lookups = out.lookups;
    const double ms2_scale = 1.0 + opt_.ms2_ppm_offset * 1e-6;
    const double ms1_scale = 1.0 + opt_.ms1_ppm_offset * 1e-6;
    const int n_iso = opt_.extract_ms1 ? opt_.ms1_isotopes + 1 : 0;
    const auto& ms1 = grid_.ms1;

    thread_local std::vector<std::pair<Bounds, std::uint32_t>> frag;   // (bounds, row) sorted by m/z
    thread_local std::vector<Bounds> iso;
    thread_local std::vector<float> ms1_at;                            // [ms1 spectrum][isotope]

    for (std::size_t k = u.begin; k < u.end; ++k)
    {
      const Assignment& as = plan.assignments[k];
      const std::uint32_t p = as.precursor;
      const auto [lo, hi] = cycleRange_(as.rt_center);
      TraceRecord r;
      r.precursor = p;
      r.window = static_cast<std::int32_t>(as.window);
      r.rt_center = static_cast<float>(as.rt_center);
      r.cycle_begin = static_cast<std::uint32_t>(lo);
      r.cycles = static_cast<std::uint32_t>(hi > lo ? hi - lo : 0);
      r.fragments = static_cast<std::uint16_t>(lib.fragmentsOf(p));
      r.isotopes = static_cast<std::uint8_t>(n_iso);
      if (r.cycles == 0)
      {
        out.records.push_back(r); // RT window outside the acquisition: an empty record keeps accounting exact
        continue;
      }
      const std::size_t T = r.cycles;
      const auto [m0, m1] = ms1Range_(as.rt_center, lo, hi);
      r.ms1_begin = static_cast<std::uint32_t>(m0);
      r.ms1_count = static_cast<std::uint32_t>(m1 - m0);
      const std::size_t M = r.ms1_count;
      float* v = r.values = out.arena.alloc(r.fragments * T + r.isotopes * M);

      // --- MS2: one tile, F x T, gathered cell by cell from this window's lane.
      frag.clear();
      for (std::uint32_t f = lib.fragment_begin[p], row = 0; f < lib.fragment_begin[p + 1]; ++f, ++row)
      {
        frag.push_back({bounds(lib.fragment_mz[f] * ms2_scale, opt_.ms2_ppm), row});
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

      // --- MS1 isotopes. Scan axis: the raw value at every MS1 scan in the window. Otherwise: the MS1 scans
      // bracketing the cycle range, interpolated linearly onto the global axis (MS1 is not acquired once per
      // cycle).
      float* v1 = v + static_cast<std::size_t>(r.fragments) * T;
      if (n_iso > 0 && !ms1.rt.empty() && M > 0 && opt_.ms1_scan_axis)
      {
        const double z = opt_.isotope_unit_spacing ? 1.0 : std::max<int>(1, lib.charge[p]);
        iso.clear();
        for (int i = 0; i < n_iso; ++i)
        {
          iso.push_back(bounds((lib.precursor_mz[p] + i * C13_C12 / z) * ms1_scale, opt_.ms1_ppm));
        }
        for (std::size_t j = m0; j < m1; ++j)
        {
          const std::size_t a = ms1.peaks.offset[j], b = ms1.peaks.offset[j + 1];
          std::size_t pos = a;
          for (int i = 0; i < n_iso; ++i)
          {
            v1[i * M + (j - m0)] = aggregate_(ms1.peaks.mz.data(), ms1.peaks.intensity.data(), a, b, iso[i].lo, iso[i].hi, pos);
          }
          lookups += n_iso;
        }
      }
      else if (n_iso > 0 && !ms1.rt.empty())
      {
        const double t0 = grid_.cycle_rt[lo], t1 = grid_.cycle_rt[hi - 1];
        auto j0 = static_cast<std::size_t>(std::upper_bound(ms1.rt.begin(), ms1.rt.end(), t0) - ms1.rt.begin());
        j0 = j0 > 0 ? j0 - 1 : 0;
        auto j1 = static_cast<std::size_t>(std::lower_bound(ms1.rt.begin(), ms1.rt.end(), t1) - ms1.rt.begin());
        j1 = std::min(j1, ms1.rt.size() - 1);
        const double z = opt_.isotope_unit_spacing ? 1.0 : std::max<int>(1, lib.charge[p]);
        iso.clear();
        for (int i = 0; i < n_iso; ++i)
        {
          iso.push_back(bounds((lib.precursor_mz[p] + i * C13_C12 / z) * ms1_scale, opt_.ms1_ppm));
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
            v1[i * M + (c - lo)] = static_cast<float>(ya + w * (yb - ya));
          }
        }
      }
      out.records.push_back(r);
    }
  }

  void Extractor::account(const std::vector<TraceRecord>& records, std::uint64_t lookups)
  {
    stats_.peak_lookups += lookups;
    for (const auto& r : records)
    {
      stats_.traces += r.fragments + r.isotopes;
      stats_.ms2_points += static_cast<std::uint64_t>(r.fragments) * r.cycles;
      stats_.ms1_points += static_cast<std::uint64_t>(r.isotopes) * r.ms1_count;
      stats_.empty_range += r.cycles == 0;
    }
  }
}
