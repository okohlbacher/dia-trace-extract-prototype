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
      if (!(rt == rt)) { ++plan.without_window; continue; }   // NaN RT: no window, and it would break the sort below
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
    // Deterministic order: window, then RT, then library index. Units never straddle a window (a unit touches
    // one lane of the grid) and hold RT-sorted precursors: their cell ranges then rise monotonically, so the
    // precursors active in a spectrum are a sliding, contiguous range (the merge join in extract() keeps only
    // their targets).
    std::stable_sort(assignments.begin(), assignments.end(), [](const Assignment& a, const Assignment& b) {
      if (a.window != b.window) { return a.window < b.window; }
      if (a.rt_center != b.rt_center) { return a.rt_center < b.rt_center; }
      return a.precursor < b.precursor;
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

  namespace
  {
    /// A target m/z window of one trace of a unit (fragment or MS1 isotope), for the merge join.
    struct Target
    {
      MzFixed lo, hi;
      std::uint32_t rec, row;
    };
    /// Where a record's traces live on the join's axis: cells [begin, end), values at
    /// base + row * stride + (cell - begin).
    struct Span
    {
      std::uint32_t begin = 0, end = 0;
      float* base = nullptr;
      std::uint32_t stride = 0;
    };

    /// Merge join of a unit's targets with the cells [c0, c1) of one peak arena.
    ///
    /// `targets` are grouped by record (record r owns [seg[r], seg[r+1]), sorted by lo within the group);
    /// records are in RT order, so their spans [begin, end) rise monotonically and the records active in a cell
    /// form a sliding range. `act` holds the active targets sorted by lo: a record's targets are merged in when
    /// its span opens and dropped when it closes. Each cell's peaks are then walked once, forward, together
    /// with the active targets; the bucket index jumps over m/z gaps. Same result as a lower_bound per target
    /// and cell (first peak >= lo, then every peak <= hi aggregated by max or sum).
    template <bool Sum>
    std::uint64_t join(const PeakArena& P, const std::vector<Target>& targets, const std::vector<std::uint32_t>& seg,
                       const std::vector<Span>& spans, std::size_t c0, std::size_t c1,
                       std::vector<Target>& act, std::vector<Target>& tmp)
    {
      const MzFixed* mz = P.mz;
      const float* in = P.intensity;
      const auto by_lo = [](const Target& a, const Target& b) {
        return a.lo != b.lo ? a.lo < b.lo : (a.rec != b.rec ? a.rec < b.rec : a.row < b.row);
      };
      const std::size_t n_rec = spans.size();
      std::size_t r_in = 0, r_out = 0;
      std::uint64_t visited = 0;
      act.clear();
      for (std::size_t c = c0; c < c1; ++c)
      {
        // records whose span opens at or before c
        while (r_in < n_rec && spans[r_in].begin <= c)
        {
          if (spans[r_in].end > c && seg[r_in + 1] > seg[r_in])
          {
            tmp.resize(act.size() + (seg[r_in + 1] - seg[r_in]));
            std::merge(act.begin(), act.end(), targets.begin() + seg[r_in], targets.begin() + seg[r_in + 1], tmp.begin(), by_lo);
            act.swap(tmp);
          }
          ++r_in;
        }
        // records whose span has closed
        bool closed = false;
        while (r_out < r_in && spans[r_out].end <= c) { ++r_out; closed = true; }
        if (closed)
        {
          act.erase(std::remove_if(act.begin(), act.end(), [&](const Target& t) { return spans[t.rec].end <= c; }), act.end());
        }
        if (act.empty()) { continue; }
        const std::size_t a = P.cellBegin(c), b = P.cellEnd(c);
        if (a == b) { continue; }
        std::size_t p = a;
        for (const Target& t : act)
        {
          ++visited;
          if (p < b && mz[p] < t.lo)
          {
            const std::size_t q = P.seek(c, t.lo);
            if (q > p) { p = q; }
            while (p < b && mz[p] < t.lo) { ++p; }
          }
          float v = 0.0f;
          for (std::size_t i = p; i < b && mz[i] <= t.hi; ++i)
          {
            if constexpr (Sum) { v += in[i]; } else { v = std::max(v, in[i]); }
          }
          const Span& sp = spans[t.rec];
          sp.base[static_cast<std::size_t>(t.row) * sp.stride + (c - sp.begin)] = v;
        }
      }
      return visited;
    }
  }

  void Extractor::extract(const Library& lib, const Plan& plan, std::size_t unit, TraceBlock& out) const
  {
    const Unit& u = plan.units[unit];
    out.unit = unit;
    out.records.clear();
    out.lookups = 0;
    const auto corrected = [](double mz, double offset, double slope, double ref) { return mz * (1.0 + (offset + slope * (mz - ref)) * 1e-6); };
    const auto ms2_corr = [&](double mz) { return corrected(mz, opt_.ms2_ppm_offset, opt_.ms2_ppm_slope, opt_.ms2_mz_ref); };
    const auto ms1_corr = [&](double mz) { return corrected(mz, opt_.ms1_ppm_offset, opt_.ms1_ppm_slope, opt_.ms1_mz_ref); };
    const int n_iso = opt_.extract_ms1 ? opt_.ms1_isotopes + 1 : 0;
    const auto& ms1 = grid_.ms1;
    const auto& lane = grid_.windows[plan.assignments[u.begin].window];

    // Reused per worker: targets and spans of the unit (a few 10 kB), MS1 interpolation scratch.
    thread_local std::vector<Target> t2, t1, act, tmp;
    thread_local std::vector<std::uint32_t> g2, g1;   // per record: first target (grouped by record)
    thread_local std::vector<Span> s2, s1;
    thread_local std::vector<Bounds> iso;
    thread_local std::vector<float> ms1_at;   // [ms1 spectrum][isotope], interpolated mode
    t2.clear(); t1.clear(); s2.clear(); s1.clear(); g2.clear(); g1.clear();
    std::size_t c_min = std::numeric_limits<std::size_t>::max(), c_max = 0;
    std::size_t m_min = std::numeric_limits<std::size_t>::max(), m_max = 0;

    // 1. Records, their value slots and the targets of the unit.
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
      if (lib.fragmentsOf(p) > 65535) { throw std::runtime_error("precursor " + std::string(lib.precursor_id[p]) + " has more than 65535 fragments"); }
      r.fragments = static_cast<std::uint16_t>(lib.fragmentsOf(p));
      r.isotopes = static_cast<std::uint8_t>(n_iso);
      const auto rec = static_cast<std::uint32_t>(out.records.size());
      s2.push_back({});
      s1.push_back({});
      g2.push_back(static_cast<std::uint32_t>(t2.size()));
      g1.push_back(static_cast<std::uint32_t>(t1.size()));
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

      s2[rec] = {static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi), v, static_cast<std::uint32_t>(T)};
      c_min = std::min(c_min, lo); c_max = std::max(c_max, hi);
      for (std::uint32_t f = lib.fragment_begin[p], row = 0; f < lib.fragment_begin[p + 1]; ++f, ++row)
      {
        const Bounds bd = bounds(ms2_corr(lib.fragment_mz[f]), opt_.ms2_ppm);
        t2.push_back({bd.lo, bd.hi, rec, row});
      }
      std::sort(t2.begin() + g2.back(), t2.end(), [](const Target& x, const Target& y) {
        return x.lo != y.lo ? x.lo < y.lo : x.row < y.row;
      });

      if (n_iso > 0 && !ms1.rt.empty())
      {
        const double z = opt_.isotope_unit_spacing ? 1.0 : std::max<int>(1, lib.charge[p]);
        float* v1 = v + static_cast<std::size_t>(r.fragments) * T;
        if (opt_.ms1_scan_axis)
        {
          if (M > 0)
          {
            s1[rec] = {static_cast<std::uint32_t>(m0), static_cast<std::uint32_t>(m1), v1, static_cast<std::uint32_t>(M)};
            m_min = std::min(m_min, m0); m_max = std::max(m_max, m1);
            for (int i = 0; i < n_iso; ++i)
            {
              const Bounds bd = bounds(ms1_corr(lib.precursor_mz[p] + i * C13_C12 / z), opt_.ms1_ppm);
              t1.push_back({bd.lo, bd.hi, rec, static_cast<std::uint32_t>(i)});
            }
            std::sort(t1.begin() + g1.back(), t1.end(), [](const Target& x, const Target& y) {
              return x.lo != y.lo ? x.lo < y.lo : x.row < y.row;
            });
          }
        }
        else
        {
          // Interpolated mode: the MS1 scans bracketing the cycle range, linearly interpolated onto the
          // global axis (MS1 is not acquired once per cycle).
          const double t0 = grid_.cycle_rt[lo], t1v = grid_.cycle_rt[hi - 1];
          auto j0 = static_cast<std::size_t>(std::upper_bound(ms1.rt.begin(), ms1.rt.end(), t0) - ms1.rt.begin());
          j0 = j0 > 0 ? j0 - 1 : 0;
          auto j1 = static_cast<std::size_t>(std::lower_bound(ms1.rt.begin(), ms1.rt.end(), t1v) - ms1.rt.begin());
          j1 = std::min(j1, ms1.rt.size() - 1);
          iso.clear();
          for (int i = 0; i < n_iso; ++i)
          {
            iso.push_back(bounds(ms1_corr(lib.precursor_mz[p] + i * C13_C12 / z), opt_.ms1_ppm));
          }
          const std::size_t J = j1 - j0 + 1;
          ms1_at.assign(J * n_iso, 0.0f);
          for (std::size_t j = j0; j <= j1; ++j)
          {
            const std::size_t a = ms1.peaks.cellBegin(j), b = ms1.peaks.cellEnd(j);
            std::size_t pos = a;
            for (int i = 0; i < n_iso; ++i)
            {
              ms1_at[(j - j0) * n_iso + i] = aggregate_(ms1.peaks.mz, ms1.peaks.intensity, a, b,
                                                        iso[i].lo, iso[i].hi, pos);
            }
            out.lookups += n_iso;
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
      }
      out.records.push_back(r);
    }

    // 2. Merge joins: MS2 over this window's cells, MS1 over its scans.
    g2.push_back(static_cast<std::uint32_t>(t2.size()));
    g1.push_back(static_cast<std::uint32_t>(t1.size()));
    if (!t2.empty())
    {
      out.lookups += opt_.aggregate_sum ? join<true>(lane.peaks, t2, g2, s2, c_min, c_max, act, tmp)
                                        : join<false>(lane.peaks, t2, g2, s2, c_min, c_max, act, tmp);
    }
    if (!t1.empty())
    {
      out.lookups += opt_.aggregate_sum ? join<true>(ms1.peaks, t1, g1, s1, m_min, m_max, act, tmp)
                                        : join<false>(ms1.peaks, t1, g1, s1, m_min, m_max, act, tmp);
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
