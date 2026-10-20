// RunModel: the running estimators recover a known RT-error scale and peak width that change along the gradient.
#include "RunModel.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>

int main()
{
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> u(600.0, 1200.0);
  std::normal_distribution<double> z(0.0, 1.0);
  std::vector<double> rt, res, fw;
  // scale 10 s at 600 s rising to 30 s at 1200 s; FWHM 2 s rising to 4 s; bias +5 s
  for (int i = 0; i < 3000; ++i)
  {
    const double t = u(rng), x = (t - 600.0) / 600.0;
    rt.push_back(t); res.push_back(5.0 + (10.0 + 20.0 * x) * z(rng)); fw.push_back(2.0 + 2.0 * x + 0.2 * z(rng));
  }
  const ODIA::RunModel m = ODIA::estimateRunModel(rt, res, fw);
  int bad = 0;
  const auto check = [&](const char* what, double got, double want, double tol) {
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%s %-24s got %8.3f want %8.3f (+-%g)\n", ok ? "ok  " : "FAIL", what, got, want, tol);
    bad += !ok;
  };
  for (double t : {750.0, 900.0, 1050.0})
  {
    const double x = (t - 600.0) / 600.0;
    char name[64];
    std::snprintf(name, sizeof name, "scale at %.0f s", t); check(name, m.at(m.drt_scale, t), 10.0 + 20.0 * x, 0.15 * (10.0 + 20.0 * x) + 1.0);
    std::snprintf(name, sizeof name, "tail99 at %.0f s", t); check(name, m.at(m.drt_tail99, t), 2.576 * (10.0 + 20.0 * x), 0.25 * 2.576 * (10.0 + 20.0 * x) + 2.0);
    std::snprintf(name, sizeof name, "FWHM at %.0f s", t); check(name, m.at(m.fwhm, t), 2.0 + 2.0 * x, 0.25);
    std::snprintf(name, sizeof name, "bias at %.0f s", t); check(name, m.at(m.drt_center, t), 5.0, 0.2 * (10.0 + 20.0 * x));   // ~3 standard errors of a median over 600 anchors
  }
  check("anchors", static_cast<double>(m.anchors), 3000, 0);
  check("too few anchors: empty", static_cast<double>(ODIA::estimateRunModel({1, 2}, {0, 0}, {1, 1}).empty()), 1, 0);
  // nearest in RT, not by rank: 100 anchors at 0-99 s and 100 at 1000-1099 s; k = 20 around 120 s must take the
  // anchors at 80-99 s (all within 40 s), none from the far cluster
  {
    std::vector<double> t2, r2, f2;
    for (int i = 0; i < 100; ++i) { t2.push_back(i); r2.push_back(0.0); f2.push_back(1.0); t2.push_back(1000 + i); r2.push_back(100.0); f2.push_back(9.0); }
    const ODIA::RunModel g = ODIA::estimateRunModel(t2, r2, f2, 40, 0.1, 15, 50);
    std::size_t j = 0;
    while (j + 1 < g.rt.size() && g.rt[j] < 120) { ++j; }
    check("gap: support starts in the near cluster", g.support_lo[j - 1] < 100 ? 1.0 : 0.0, 1, 0);
    check("gap: bias from the near cluster", g.drt_center[j - 1], 0.0, 0.0);
  }
  // invalid input and the missing-curve fallback
  {
    bool threw = false;
    try { ODIA::estimateRunModel(rt, res, fw, 40, 0.0); } catch (const std::invalid_argument&) { threw = true; }
    check("span 0 rejected", threw ? 1.0 : 0.0, 1, 0);
    std::vector<double> rtn = rt; rtn[0] = std::nan("");
    check("non-finite anchor dropped", static_cast<double>(ODIA::estimateRunModel(rtn, res, fw).anchors), 2999, 0);
    const std::vector<double> other(m.rt.size(), 1.0);
    check("foreign curve gives NaN", std::isnan(m.at(other, 900.0)) ? 1.0 : 0.0, 1, 0);
  }
  // m/z errors: width 3 ppm for the weakest tenth falling to 1 ppm for the strongest (log-uniform intensities),
  // offset 0, plus 10% random centroids flat over a +-50 ppm window
  {
    std::uniform_real_distribution<double> li(3.0, 7.0), w(-50.0, 50.0), u01(0.0, 1.0);
    std::vector<double> t3, r3, win, in;
    for (int i = 0; i < 20000; ++i)
    {
      const double l = li(rng), x = (l - 3.0) / 4.0;
      double e = (3.0 - 2.0 * x) * z(rng);
      if (u01(rng) < 0.1) { e = w(rng); }
      t3.push_back(u(rng)); r3.push_back(e); win.push_back(e); in.push_back(std::pow(10.0, l));
    }
    const ODIA::MzErrors e = ODIA::estimateMzErrors("ms2", "broad", t3, r3, win, in, 50.0, 6.0);
    check("m/z bins", static_cast<double>(e.scale_by_intensity.size()), 10, 0);
    check("m/z scale, weakest tenth", e.scale_by_intensity.front(), 2.9, 0.3);   // bin centre x = 0.05
    check("m/z scale, strongest tenth", e.scale_by_intensity.back(), 1.1, 0.15);
    check("m/z background share", e.background, 0.1, 0.02);
    check("m/z count per bin", static_cast<double>(e.count_by_intensity[3]), 2000, 0);
    check("m/z centre along RT", e.center_rt[e.center_rt.size() / 2], 0.0, 0.1);
    check("m/z too few points: empty", static_cast<double>(ODIA::estimateMzErrors("ms1", "final", {1}, {0}, {0}, {1}, 5, 1).empty()), 1, 0);
    ODIA::RunModel only_mz;
    only_mz.mz.push_back(e);
    check("m/z only: model not empty", only_mz.empty() ? 0.0 : 1.0, 1, 0);
  }
  return bad ? 1 : 0;
}
