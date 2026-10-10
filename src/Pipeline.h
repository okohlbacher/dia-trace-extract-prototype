// ODIA-V3 trace extractor -- ordered parallel pipeline.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <map>
#include <mutex>
#include <optional>

namespace ODIA
{
  /// Runs `make(i)` for i in [0, n) on `threads` OpenMP threads and hands each result to
  /// `commit(result)` strictly in index order (one call at a time).
  ///
  /// Results finished out of order are parked; a worker more than `ahead` units in front of the
  /// next commit waits, which bounds the parked results. The output sequence is therefore
  /// independent of the thread count. The first exception stops the pipeline and is rethrown.
  template <typename Make, typename Commit>
  void orderedParallel(std::size_t n, int threads, std::size_t ahead, Make&& make, Commit&& commit)
  {
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
        cv.wait(lock, [&] { return i < next + ahead || error; });
        if (error) { continue; }
      }
      std::optional<Result> r;
      try { r.emplace(make(i)); }
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
        try { commit(std::move(node.mapped())); }
        catch (...) { error = std::current_exception(); break; }
        ++next;
      }
      cv.notify_all();
    }
    if (error) { std::rethrow_exception(error); }
  }
}
