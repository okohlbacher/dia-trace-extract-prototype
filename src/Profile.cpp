// ODIA-V3 trace extractor -- lightweight profiling counters (ODIA_PROFILE=1).
#include "Profile.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <sys/resource.h>

namespace ODIA::prof
{
  namespace
  {
    std::mutex& registryMutex() { static std::mutex m; return m; }
    std::map<std::string, std::unique_ptr<Counter>>& registry() { static std::map<std::string, std::unique_ptr<Counter>> r; return r; }
  }

  bool enabled()
  {
    static const bool on = std::getenv("ODIA_PROFILE") != nullptr;
    return on;
  }

  Counter& counter(const std::string& name)
  {
    std::lock_guard g(registryMutex());
    auto& slot = registry()[name];
    if (!slot) { slot = std::make_unique<Counter>(); }
    return *slot;
  }

  namespace
  {
    std::map<std::string, std::unique_ptr<TaskStats>>& taskRegistry() { static std::map<std::string, std::unique_ptr<TaskStats>> r; return r; }
    struct Usage { std::uint64_t cpu, flt, vcs, ivcs; };
    Usage threadUsage()
    {
      rusage ru{};
      ::getrusage(RUSAGE_THREAD, &ru);
      const auto us = [](const timeval& t) { return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(t.tv_usec) * 1000ull; };
      return {us(ru.ru_utime) + us(ru.ru_stime), static_cast<std::uint64_t>(ru.ru_minflt), static_cast<std::uint64_t>(ru.ru_nvcsw),
              static_cast<std::uint64_t>(ru.ru_nivcsw)};
    }
  }

  TaskStats& taskStats(const std::string& name)
  {
    std::lock_guard g(registryMutex());
    auto& slot = taskRegistry()[name];
    if (!slot) { slot = std::make_unique<TaskStats>(); }
    return *slot;
  }

  TaskProbe::TaskProbe(TaskStats& s) : s_(enabled() ? &s : nullptr)
  {
    if (!s_) { return; }
    const Usage u = threadUsage();
    w0_ = nowNs(); cpu0_ = u.cpu; flt0_ = u.flt; vcs0_ = u.vcs; ivcs0_ = u.ivcs;
  }

  TaskProbe::~TaskProbe()
  {
    if (!s_) { return; }
    const Usage u = threadUsage();
    s_->n.fetch_add(1, std::memory_order_relaxed);
    s_->wall_ns.fetch_add(nowNs() - w0_, std::memory_order_relaxed);
    s_->cpu_ns.fetch_add(u.cpu - cpu0_, std::memory_order_relaxed);
    s_->minflt.fetch_add(u.flt - flt0_, std::memory_order_relaxed);
    s_->nvcsw.fetch_add(u.vcs - vcs0_, std::memory_order_relaxed);
    s_->nivcsw.fetch_add(u.ivcs - ivcs0_, std::memory_order_relaxed);
  }

  void report(std::FILE* out)
  {
    std::lock_guard g(registryMutex());
    std::fprintf(out, "[task] %-20s %10s %10s %10s %8s %12s %12s %12s\n", "class", "tasks", "wall_s", "cpu_s", "cpu/wall",
                 "minflt", "vol_cs", "invol_cs");
    for (const auto& [name, t] : taskRegistry())
    {
      const double w = t->wall_ns.load() * 1e-9, c = t->cpu_ns.load() * 1e-9;
      std::fprintf(out, "[task] %-20s %10llu %10.3f %10.3f %8.3f %12llu %12llu %12llu\n", name.c_str(),
                   static_cast<unsigned long long>(t->n.load()), w, c, w > 0 ? c / w : 0.0,
                   static_cast<unsigned long long>(t->minflt.load()), static_cast<unsigned long long>(t->nvcsw.load()),
                   static_cast<unsigned long long>(t->nivcsw.load()));
    }
    std::fprintf(out, "[profile] %-44s %12s %12s %12s\n", "counter", "total_s", "count", "mean_ms");
    for (const auto& [name, c] : registry())
    {
      const double s = c->ns.load() * 1e-9;
      const auto n = c->n.load();
      std::fprintf(out, "[profile] %-44s %12.3f %12llu %12.4f\n", name.c_str(), s, static_cast<unsigned long long>(n),
                   n ? s * 1e3 / static_cast<double>(n) : 0.0);
    }
  }
}
