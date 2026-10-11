// ODIA-V3 trace extractor -- lightweight profiling counters (ODIA_PROFILE=1).
#include "Profile.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

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

  void report(std::FILE* out)
  {
    std::lock_guard g(registryMutex());
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
