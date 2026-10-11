// ODIA-V3 trace extractor -- ordered parallel pipeline.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "Profile.h"

namespace ODIA
{
  /// Runs `make(i)` for i in [0, n) on `threads` OpenMP threads and hands each result to
  /// `commit(result)` strictly in index order (one call at a time).
  ///
  /// Results finished out of order are parked; a worker more than `ahead` units in front of the
  /// next commit waits, which bounds the parked results. The output sequence is therefore
  /// independent of the thread count. The first exception stops the pipeline and is rethrown.
  /// With ODIA_PROFILE and a `tag`: counters <tag>.make (parallel work per item), <tag>.commit (the serial,
  /// ordered stage) and <tag>.ahead_wait (workers blocked because they ran `ahead` items in front of the commit).
  template <typename Make, typename Commit>
  void orderedParallel(std::size_t n, int threads, std::size_t ahead, Make&& make, Commit&& commit, const char* tag = nullptr)
  {
    prof::Counter* c_make = tag && prof::enabled() ? &prof::counter(std::string(tag) + ".make") : nullptr;
    prof::Counter* c_commit = c_make ? &prof::counter(std::string(tag) + ".commit") : nullptr;
    prof::Counter* c_wait = c_make ? &prof::counter(std::string(tag) + ".ahead_wait") : nullptr;
    ahead = std::max<std::size_t>(ahead, 1);   // 0 would make the worker holding `next` wait for itself
    using Result = decltype(make(std::size_t{0}));
    std::mutex mtx;
    std::condition_variable cv;
    std::map<std::size_t, Result> parked;
    std::size_t next = 0;
    std::exception_ptr error;

#pragma omp parallel for schedule(dynamic, 1) num_threads(threads)
    for (std::ptrdiff_t ii = 0; ii < static_cast<std::ptrdiff_t>(n); ++ii)
    {
      const auto i = static_cast<std::size_t>(ii);
      {
        std::unique_lock lock(mtx);
        const std::uint64_t t_wait = c_wait ? prof::nowNs() : 0;
        cv.wait(lock, [&] { return i < next + ahead || error; });
        if (c_wait) { c_wait->add(prof::nowNs() - t_wait); }
        if (error) { continue; }
      }
      std::optional<Result> r;
      const std::uint64_t t_make = c_make ? prof::nowNs() : 0;
      try { r.emplace(make(i)); if (c_make) { c_make->add(prof::nowNs() - t_make); } }
      catch (...)
      {
        std::lock_guard lock(mtx);
        if (!error) { error = std::current_exception(); }
        cv.notify_all();
        continue;
      }
      std::unique_lock lock(mtx);
      parked.emplace(i, std::move(*r));
      while (!error && !parked.empty() && parked.begin()->first == next)
      {
        auto node = parked.extract(parked.begin());
        const std::uint64_t t_commit = c_commit ? prof::nowNs() : 0;
        try { commit(std::move(node.mapped())); if (c_commit) { c_commit->add(prof::nowNs() - t_commit); } }
        catch (...) { error = std::current_exception(); break; }
        ++next;
      }
      cv.notify_all();
    }
    if (error) { std::rethrow_exception(error); }
  }
}
