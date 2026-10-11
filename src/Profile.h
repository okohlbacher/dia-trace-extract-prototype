// ODIA-V3 trace extractor -- lightweight profiling counters (ODIA_PROFILE=1).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ODIA::prof
{
  /// True when the environment sets ODIA_PROFILE (read once).
  bool enabled();

  inline std::uint64_t nowNs()
  {
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  /// A named accumulator: total nanoseconds and event count (atomic; cheap enough per task, not per peak).
  struct Counter
  {
    std::atomic<std::uint64_t> ns{0}, n{0};
    void add(std::uint64_t d, std::uint64_t k = 1)
    {
      ns.fetch_add(d, std::memory_order_relaxed);
      n.fetch_add(k, std::memory_order_relaxed);
    }
  };

  /// The counter registered under `name` (created on first use; keep the reference in a function-local static).
  Counter& counter(const std::string& name);

  /// Times a scope into a counter when profiling is enabled.
  class Scope
  {
  public:
    explicit Scope(Counter& c) : c_(enabled() ? &c : nullptr), t0_(c_ ? nowNs() : 0) {}
    ~Scope() { if (c_) { c_->add(nowNs() - t0_); } }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  private:
    Counter* c_;
    std::uint64_t t0_;
  };

  /// All counters, sorted by name: total seconds, count, mean milliseconds.
  void report(std::FILE* out);
}
