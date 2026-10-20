// RunModel: the running estimators recover a known RT-error scale and peak width that change along the gradient.
#include "RunModel.h"

#include <cmath>
#include <cstdio>
#include <random>

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
  return bad ? 1 : 0;
}
