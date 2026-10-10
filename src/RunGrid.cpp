// ODIA-V3 trace extractor -- compact grid representation of a DIA run on ONE global RT axis.
#include "RunGrid.h"

#include <OpenMS/FORMAT/DATAACCESS/MSDataTransformingConsumer.h>
#include <OpenMS/FORMAT/FileHandler.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/KERNEL/MSExperiment.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <sys/resource.h>
#include <thread>

#include <omp.h>

namespace ODIA
{
  StageTime stageClock()
  {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    const double cpu = u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return {wall, cpu};
  }
  StageTime operator-(const StageTime& a, const StageTime& b) { return {a.wall - b.wall, a.cpu - b.cpu}; }

  namespace
  {
    bool isRaw(const std::string& f)
    {
      if (f.size() < 4) { return false; }
      std::string ext = f.substr(f.size() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
      return ext == ".raw";
    }

    /// A decoded spectrum on its way from the parser to the router.
    struct Decoded
    {
      std::uint32_t index = 0;
      double rt = 0;
      int ms_level = 0;
      double target = 0, lower = 0, upper = 0;
      std::vector<MzFixed> mz;
      std::vector<float> intensity;
      bool was_unsorted = false;
    };

    Decoded decode(OpenMS::MSSpectrum& s, std::uint32_t index)
    {
      Decoded d;
      d.index = index;
      d.rt = s.getRT();
      d.ms_level = static_cast<int>(s.getMSLevel());
      if (d.ms_level >= 2 && !s.getPrecursors().empty())
      {
        const auto& p = s.getPrecursors()[0];
        d.target = p.getMZ();
        d.lower = p.getMZ() - p.getIsolationWindowLowerOffset();
        d.upper = p.getMZ() + p.getIsolationWindowUpperOffset();
      }
      if (!s.isSorted()) { s.sortByPosition(); d.was_unsorted = true; }
      d.mz.resize(s.size());
      d.intensity.resize(s.size());
      for (std::size_t i = 0; i < s.size(); ++i)
      {
        d.mz[i] = toFixed(s[i].getMZ());
        d.intensity[i] = s[i].getIntensity();
      }
      return d;
    }

    void append(PeakArena& a, const Decoded& d)
    {
      a.mz.insert(a.mz.end(), d.mz.begin(), d.mz.end());
      a.intensity.insert(a.intensity.end(), d.intensity.begin(), d.intensity.end());
      a.offset.push_back(a.mz.size());
    }

    /// Routes decoded spectra onto the grid. Single owner: runs on one thread.
    class Router
    {
    public:
      explicit Router(RunGrid& g) : g_(g) {}

      void route(Decoded&& d)
      {
        ++g_.stats.spectra;
        if (d.was_unsorted) { ++g_.stats.unsorted_spectra; }
        if (d.ms_level == 1)
        {
          ++g_.stats.ms1_spectra;
          g_.stats.ms1_peaks += d.mz.size();
          append(g_.ms1.peaks, d);
          g_.ms1.rt.push_back(d.rt);
          g_.ms1.spectrum.push_back(d.index);
          return;
        }
        if (d.ms_level != 2) { ++g_.stats.other_spectra; return; }
        ++g_.stats.ms2_spectra;
        g_.stats.ms2_peaks += d.mz.size();

        const std::size_t w = windowId_(d);
        if (w < seen_.size() && seen_[w])
        {
          // The window repeats: the cycle is complete.
          closeCycle_();
        }
        if (seen_.size() <= w) { seen_.resize(w + 1, false); }
        seen_[w] = true;
        cycle_rt_sum_ += d.rt;
        ++cycle_n_;
        pending_.push_back({w, d.index, d.rt, std::move(d.mz), std::move(d.intensity)});
      }

      void finish()
      {
        if (cycle_n_ > 0) { closeCycle_(); }
        // Order windows by isolation target; pad every lane to the full cycle count.
        std::vector<std::size_t> order(lanes_.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
          return lanes_[a].target < lanes_[b].target;
        });
        const std::size_t n = g_.cycle_rt.size();
        for (auto i : order)
        {
          auto& lane = lanes_[i];
          padTo_(lane, n);
          g_.windows.push_back(std::move(lane));
        }
      }

    private:
      struct Pending
      {
        std::size_t window;
        std::uint32_t index;
        double rt;
        std::vector<MzFixed> mz;
        std::vector<float> intensity;
      };

      std::size_t windowId_(const Decoded& d)
      {
        // Same window: isolation target within 1e-6 Th and same edges (OpenMS SwathFileConsumer rule).
        auto it = ids_.lower_bound(d.target - 1e-6);
        for (; it != ids_.end() && it->first <= d.target + 1e-6; ++it)
        {
          const auto& l = lanes_[it->second];
          if (std::fabs(l.lower - d.lower) < 1e-6 && std::fabs(l.upper - d.upper) < 1e-6) { return it->second; }
        }
        WindowLane lane;
        lane.target = d.target;
        lane.lower = d.lower;
        lane.upper = d.upper;
        lanes_.push_back(std::move(lane));
        ids_.emplace(d.target, lanes_.size() - 1);
        return lanes_.size() - 1;
      }

      void padTo_(WindowLane& lane, std::size_t cycles)
      {
        while (lane.acquired_rt.size() < cycles)
        {
          lane.peaks.offset.push_back(lane.peaks.mz.size());
          lane.acquired_rt.push_back(std::numeric_limits<double>::quiet_NaN());
          lane.spectrum.push_back(std::numeric_limits<std::uint32_t>::max());
          ++g_.stats.missing_cells;
        }
      }

      void closeCycle_()
      {
        const std::size_t c = g_.cycle_rt.size();
        g_.cycle_rt.push_back(cycle_rt_sum_ / static_cast<double>(cycle_n_));
        for (auto& p : pending_)
        {
          auto& lane = lanes_[p.window];
          padTo_(lane, c); // cycles this window skipped
          if (lane.acquired_rt.size() == c + 1)
          {
            // A second spectrum of the window inside one cycle cannot happen by construction
            // (a repeat closes the cycle); kept as a guard.
            ++g_.stats.duplicate_cells;
            continue;
          }
          lane.peaks.mz.insert(lane.peaks.mz.end(), p.mz.begin(), p.mz.end());
          lane.peaks.intensity.insert(lane.peaks.intensity.end(), p.intensity.begin(), p.intensity.end());
          lane.peaks.offset.push_back(lane.peaks.mz.size());
          lane.acquired_rt.push_back(p.rt);
          lane.spectrum.push_back(p.index);
        }
        pending_.clear();
        std::fill(seen_.begin(), seen_.end(), false);
        cycle_rt_sum_ = 0;
        cycle_n_ = 0;
      }

      RunGrid& g_;
      std::vector<WindowLane> lanes_;
      std::multimap<double, std::size_t> ids_;
      std::vector<bool> seen_;
      std::vector<Pending> pending_;
      double cycle_rt_sum_ = 0;
      std::size_t cycle_n_ = 0;
    };

    struct StopParsing {};
  }

  std::size_t RunGrid::bytes() const
  {
    auto arena = [](const PeakArena& a) {
      return a.offset.capacity() * 8 + a.mz.capacity() * 4 + a.intensity.capacity() * 4;
    };
    std::size_t b = arena(ms1.peaks) + ms1.rt.capacity() * 8 + ms1.spectrum.capacity() * 4 + cycle_rt.capacity() * 8;
    for (const auto& w : windows) { b += arena(w.peaks) + w.acquired_rt.capacity() * 8 + w.spectrum.capacity() * 4; }
    return b;
  }

  std::size_t RunGrid::lowerCycle(double t) const
  {
    return static_cast<std::size_t>(std::lower_bound(cycle_rt.begin(), cycle_rt.end(), t) - cycle_rt.begin());
  }

  std::size_t RunGrid::upperCycle(double t) const
  {
    return static_cast<std::size_t>(std::upper_bound(cycle_rt.begin(), cycle_rt.end(), t) - cycle_rt.begin());
  }

  RunGrid RunGrid::load(const std::string& filename, int decode_threads)
  {
    const StageTime t0 = stageClock();
    RunGrid g;
    Router router(g);

    if (isRaw(filename))
    {
      omp_set_num_threads(std::max(1, decode_threads));
      OpenMS::MSExperiment exp;
      OpenMS::FileHandler().loadExperiment(filename, exp, {OpenMS::FileTypes::RAW});
      exp.sortSpectra(false);
      std::uint32_t index = 0;
      for (auto& s : exp.getSpectra()) { router.route(decode(s, index++)); }
    }
    else
    {
      // Producer/consumer: the serial XML parse (the ingest bottleneck) runs on its own thread;
      // routing onto the grid runs on this one. A bounded queue of batches bounds memory.
      constexpr std::size_t batch_size = 1024, capacity = 4;
      std::mutex mtx;
      std::condition_variable cv;
      std::deque<std::vector<Decoded>> queue;
      bool done = false, abort = false;
      std::exception_ptr error;

      std::thread producer([&] {
        // A new thread starts with the default OpenMP thread count (all cores): bound it.
        omp_set_num_threads(std::max(1, decode_threads));
        try
        {
          OpenMS::MzMLFile file;
          file.getOptions().setSkipXMLChecks(true);
          OpenMS::MSDataTransformingConsumer consumer;
          std::vector<Decoded> batch;
          std::uint32_t index = 0;
          auto push = [&] {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return queue.size() < capacity || abort; });
            if (abort) { throw StopParsing{}; }
            queue.push_back(std::move(batch));
            batch.clear();
            cv.notify_all();
          };
          consumer.setSpectraProcessingFunc([&](OpenMS::MSSpectrum& s) {
            batch.push_back(decode(s, index++));
            if (batch.size() == batch_size) { push(); }
          });
          file.transform(filename, &consumer, true, true);
          if (!batch.empty()) { push(); }
        }
        catch (const StopParsing&) {}
        catch (...) { std::lock_guard lock(mtx); error = std::current_exception(); }
        std::lock_guard lock(mtx);
        done = true;
        cv.notify_all();
      });

      try
      {
        while (true)
        {
          std::vector<Decoded> batch;
          {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return !queue.empty() || done; });
            if (queue.empty()) { break; }
            batch = std::move(queue.front());
            queue.pop_front();
            cv.notify_all();
          }
          for (auto& d : batch) { router.route(std::move(d)); }
        }
      }
      catch (...)
      {
        { std::lock_guard lock(mtx); abort = true; cv.notify_all(); }
        producer.join();
        throw;
      }
      producer.join();
      if (error) { std::rethrow_exception(error); }
    }
    router.finish();

    for (auto& w : g.windows)
    {
      w.peaks.mz.shrink_to_fit();
      w.peaks.intensity.shrink_to_fit();
    }
    g.ms1.peaks.mz.shrink_to_fit();
    g.ms1.peaks.intensity.shrink_to_fit();
    if (g.windows.empty()) { throw std::runtime_error("no MS2 isolation windows in " + filename); }
    g.stats.ingest = stageClock() - t0;
    return g;
  }
}
