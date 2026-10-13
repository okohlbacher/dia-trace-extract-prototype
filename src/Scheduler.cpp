// ODIA-V3 trace extractor -- central scheduling of library reading, extraction and writing.
#include "Scheduler.h"
#include "Profile.h"

#include <algorithm>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace ODIA
{
  using clk = std::chrono::steady_clock;

  ExtractionScheduler::ExtractionScheduler(LibraryReader& reader, const Extractor& extractor, TraceWriter& writer,
                                           BufferPool& pool, const SchedulerOptions& opt)
    : reader_(reader), extractor_(extractor), writer_(writer), pool_(pool), opt_(opt),
      source_busy_(reader.sources(), false), sink_busy_(writer.sinks(), false), sink_queue_(writer.sinks())
  {
    opt_.threads = std::max(1, opt_.threads);
    opt_.ahead = std::max<std::size_t>(1, opt_.ahead);
    opt_.parts_ahead = std::max<std::size_t>(1, opt_.parts_ahead);
  }

  ExtractionScheduler::~ExtractionScheduler() = default;

  void ExtractionScheduler::run()
  {
    const auto t0 = clk::now();
    std::vector<std::thread> pool;
    for (int t = 0; t < opt_.threads; ++t) { pool.emplace_back([this] { worker_(); }); }
    // ODIA_PROFILE=1: sample the scheduler state twice a second (stderr).
    std::thread monitor;
    bool stop = false;
    const char* timeline_path = std::getenv("ODIA_PROFILE_TIMELINE");
    if (std::getenv("ODIA_PROFILE") != nullptr)
    {
      monitor = std::thread([&, timeline_path] {
        // ODIA_PROFILE_TIMELINE=file: every 100 ms one TSV row (per-class active workers, idle workers, queues,
        // process CPU in the interval); stderr gets a line every 0.5 s.
        std::FILE* tl = timeline_path ? std::fopen(timeline_path, "w") : nullptr;
        if (tl)
        {
          std::fprintf(tl, "t\tread\textract\tencode\twrite\tidle\toutstanding\tunissued\tloaded_parts\tunwritten\t"
                           "encode_queue\tpool_free\tcommit_part\tprocess_cpu\trunning_threads\tmhz_mean\tmhz_min\n");
        }
        const auto cpu_now = [] {
          timespec ts{};
          ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
          return ts.tv_sec + ts.tv_nsec * 1e-9;
        };
        double cpu_last = cpu_now(), t_last = 0;
        int ticks = 0;
        Lock lock(mtx_);
        while (!stop)
        {
          cv_mon_.wait_for(lock, std::chrono::milliseconds(tl ? 100 : 500));
          if (stop) { break; }
          const double t = std::chrono::duration<double>(clk::now() - t0).count(), cpu = cpu_now();
          if (tl)
          {
            // Our threads that are running (state R) and the current frequency of the cores they run on.
            int running = 0;
            double mhz_sum = 0, mhz_min = 1e12;
            int mhz_n = 0;
#ifdef __linux__
            if (DIR* d = ::opendir("/proc/self/task"))
            {
              while (dirent* e = ::readdir(d))
              {
                if (e->d_name[0] == '.') { continue; }
                char path[320];
                std::snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
                std::FILE* f = std::fopen(path, "r");
                if (!f) { continue; }
                char buf[1024];
                const std::size_t len = std::fread(buf, 1, sizeof buf - 1, f);
                std::fclose(f);
                buf[len] = 0;
                const char* q = std::strrchr(buf, ')');
                if (!q || q[2] != 'R') { continue; }
                ++running;
                // field 39 (processor): count fields after ')'
                int field = 2;
                const char* r = q + 2;
                while (*r && field < 39) { if (*r == ' ') { ++field; } ++r; }
                const int cpu_id = std::atoi(r);
                char fp[96];
                std::snprintf(fp, sizeof fp, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu_id);
                if (std::FILE* ff = std::fopen(fp, "r"))
                {
                  long khz = 0;
                  if (std::fscanf(ff, "%ld", &khz) == 1) { mhz_sum += khz / 1000.0; mhz_min = std::min(mhz_min, khz / 1000.0); ++mhz_n; }
                  std::fclose(ff);
                }
              }
              ::closedir(d);
            }
#endif
            std::fprintf(tl, "%.2f\t%d\t%d\t%d\t%d\t%d\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%.2f\t%d\t%.0f\t%.0f\n", t, active_[0], active_[1],
                         active_[2], active_[3], waiting_, outstanding_, unissued_, loaded_.size(), unwritten_,
                         encode_queue_.size(), pool_.available(), commit_part_, (cpu - cpu_last) / std::max(1e-9, t - t_last),
                         running, mhz_n ? mhz_sum / mhz_n : 0.0, mhz_n ? mhz_min : 0.0);
          }
          cpu_last = cpu; t_last = t;
          if (tl && ++ticks % 5 != 0) { continue; }
          std::fprintf(stderr, "[sched %6.1f s] active read %d extract %d encode %d write %d idle %d | outstanding %zu unissued %zu "
                       "loaded parts %zu (commit part %zu unit %zu) | encode queue %zu unwritten %zu | pool free %zu\n",
                       t, active_[0], active_[1], active_[2], active_[3], waiting_,
                       outstanding_, unissued_, loaded_.size(), commit_part_, commit_unit_, encode_queue_.size(), unwritten_,
                       pool_.available());
        }
        if (tl) { std::fclose(tl); }
      });
    }
    for (auto& t : pool) { t.join(); }
    if (monitor.joinable())
    {
      { Lock lock(mtx_); stop = true; }
      cv_mon_.notify_all();
      monitor.join();
    }
    stats_.wall = std::chrono::duration<double>(clk::now() - t0).count();
    if (error_) { std::rethrow_exception(error_); }
  }

  bool ExtractionScheduler::finished_() const
  {
    if (commit_part_ < reader_.parts() || !encode_queue_.empty() || encoding_ > 0) { return false; }
    for (std::size_t s = 0; s < sink_queue_.size(); ++s)
    {
      if (sink_busy_[s] || !sink_queue_[s].empty()) { return false; }
    }
    return true;
  }

  void ExtractionScheduler::worker_()
  {
    Lock lock(mtx_);
    while (!error_ && !finished_())
    {
      if (tryWrite_(lock)) { continue; }
      if (tryEncode_(lock)) { continue; }
      if (tryRead_(lock, true)) { continue; }
      if (tryExtract_(lock)) { continue; }
      if (tryRead_(lock, false)) { continue; }
      if (prof::enabled())
      {
        const char* why = whyIdle_();
        ++waiting_;
        const std::uint64_t t0 = prof::nowNs();
        cv_.wait(lock);
        prof::counter(std::string("sched.idle.") + why).add(prof::nowNs() - t0);
        --waiting_;
      }
      else { cv_.wait(lock); }
    }
    cv_.notify_all();
  }

  void ExtractionScheduler::relock_(Lock& lock)
  {
    if (!prof::enabled()) { lock.lock(); return; }
    static prof::Counter& c = prof::counter("sched.lock_acquire");
    const std::uint64_t t0 = prof::nowNs();
    lock.lock();
    c.add(prof::nowNs() - t0);
  }

  const char* ExtractionScheduler::whyIdle_() const
  {
    bool queued = false, queued_free = false;
    for (std::size_t s = 0; s < sink_queue_.size(); ++s)
    {
      if (sink_queue_[s].empty()) { continue; }
      queued = true;
      if (!sink_busy_[s]) { queued_free = true; }
    }
    if (unissued_ > 0)
    {
      if (unwritten_ >= opt_.max_unwritten) { return queued && !queued_free ? "unwritten_cap(sinks_busy)" : "unwritten_cap(awaiting_encode)"; }
      if (outstanding_ >= opt_.ahead) { return "ordered_window_full"; }
      if (loaded_.empty() || loaded_.begin()->first != commit_part_) { return "next_part_not_loaded"; }
      return "buffer_pool_empty";
    }
    if (commit_part_ >= reader_.parts()) { return queued ? "drain(writes_serial)" : "drain(encode)"; }
    for (bool b : source_busy_) { if (b) { return "awaiting_library_read"; } }
    if (queued && !queued_free) { return "sinks_busy"; }
    return "read_ahead_limit";
  }

  void ExtractionScheduler::fail_()
  {
    if (!error_) { error_ = std::current_exception(); }
    cv_.notify_all();
  }

  void ExtractionScheduler::begin_(int cls)
  {
    ++active_[cls];
    ++stats_.tasks[cls];
    stats_.max_threads[cls] = std::max<double>(stats_.max_threads[cls], active_[cls]);
  }

  void ExtractionScheduler::end_(int cls, clk::time_point t0)
  {
    --active_[cls];
    stats_.busy[cls] += std::chrono::duration<double>(clk::now() - t0).count();
  }

  std::shared_ptr<ExtractionScheduler::Part> ExtractionScheduler::newPart_()
  {
    std::unique_ptr<Part> p;
    {
      std::lock_guard g(recycle_mtx_);
      if (!free_parts_.empty()) { p = std::move(free_parts_.back()); free_parts_.pop_back(); }
      else { ++stats_.part_objects; }
    }
    if (!p) { p = std::make_unique<Part>(); }
    p->issued = 0;
    p->parked.clear();
    // Recycled on release: the vectors keep their capacity for the next part.
    return std::shared_ptr<Part>(p.release(), [this](Part* q) {
      std::lock_guard g(recycle_mtx_);
      free_parts_.emplace_back(q);
    });
  }

  std::unique_ptr<TraceBlock> ExtractionScheduler::newBlock_()
  {
    std::lock_guard g(recycle_mtx_);
    if (free_blocks_.empty()) { return std::make_unique<TraceBlock>(); }
    auto b = std::move(free_blocks_.back());
    free_blocks_.pop_back();
    return b;
  }

  void ExtractionScheduler::recycle_(std::unique_ptr<TraceBlock> b)
  {
    b->arena.release();   // trace blocks back to the pool
    std::lock_guard g(recycle_mtx_);
    free_blocks_.push_back(std::move(b));   // keeps the records' capacity
  }

  bool ExtractionScheduler::tryWrite_(Lock& lock)
  {
    // The idle sink with the longest queue whose next unit is encoded.
    std::size_t best = sink_queue_.size(), longest = 0;
    for (std::size_t s = 0; s < sink_queue_.size(); ++s)
    {
      if (sink_busy_[s] || sink_queue_[s].empty() || !sink_queue_[s].front()->ready) { continue; }
      if (sink_queue_[s].size() > longest) { best = s; longest = sink_queue_[s].size(); }
    }
    if (best == sink_queue_.size()) { return false; }
    std::shared_ptr<Item> item = std::move(sink_queue_[best].front());
    sink_queue_[best].pop_front();
    sink_busy_[best] = true;
    begin_(SchedulerStats::Write);
    const auto t0 = clk::now();
    lock.unlock();
    try
    {
      static prof::TaskStats& ts = prof::taskStats("write");
      prof::TaskProbe probe(ts);
      writer_.append(best, item->encoded);
    }
    catch (...)
    {
      relock_(lock);
      sink_busy_[best] = false;
      end_(SchedulerStats::Write, t0);
      fail_();
      return true;
    }
    relock_(lock);
    if (--item->sinks_left == 0) { --unwritten_; }
    sink_busy_[best] = false;
    end_(SchedulerStats::Write, t0);
    cv_.notify_all();
    return true;
  }

  bool ExtractionScheduler::tryEncode_(Lock& lock)
  {
    if (encode_queue_.empty()) { return false; }
    std::shared_ptr<Item> item = std::move(encode_queue_.front());
    encode_queue_.pop_front();
    ++encoding_;
    begin_(SchedulerStats::Encode);
    const auto t0 = clk::now();
    lock.unlock();
    try
    {
      static prof::TaskStats& ts = prof::taskStats("encode");
      prof::TaskProbe probe(ts);
      item->encoded = writer_.encode(*item->block, item->part->lib);
      // Encoded: discard the traces now (blocks back to the pool) and release the library part.
      recycle_(std::move(item->block));
      item->part.reset();
    }
    catch (...)
    {
      relock_(lock);
      --encoding_;
      end_(SchedulerStats::Encode, t0);
      fail_();
      return true;
    }
    relock_(lock);
    --encoding_;
    end_(SchedulerStats::Encode, t0);
    item->ready = true;
    cv_.notify_all();
    return true;
  }

  bool ExtractionScheduler::tryRead_(Lock& lock, bool low_water_only)
  {
    if (low_water_only && unissued_ >= 2 * static_cast<std::size_t>(opt_.threads)) { return false; }
    for (std::size_t src = 0; src < source_busy_.size(); ++src)
    {
      if (source_busy_[src]) { continue; }
      const std::size_t seq = reader_.nextPart(src);
      if (seq >= reader_.parts() || seq >= commit_part_ + opt_.parts_ahead) { continue; }
      source_busy_[src] = true;
      begin_(SchedulerStats::Read);
      const auto t0 = clk::now();
      lock.unlock();
      std::shared_ptr<Part> part;
      try
      {
        static prof::TaskStats& ts = prof::taskStats("read+plan");
        prof::TaskProbe probe(ts);
        part = newPart_();
        part->seq = reader_.read(src, part->lib);
        part->plan = extractor_.plan(part->lib);
      }
      catch (...)
      {
        relock_(lock);
        source_busy_[src] = false;
        end_(SchedulerStats::Read, t0);
        fail_();
        return true;
      }
      relock_(lock);
      source_busy_[src] = false;
      end_(SchedulerStats::Read, t0);
      unissued_ += part->plan.units.size();
      loaded_.emplace(part->seq, std::move(part));
      stats_.peak_parts = std::max(stats_.peak_parts, loaded_.size());
      tryCommit_(lock);   // a part without units commits at once
      cv_.notify_all();
      return true;
    }
    return false;
  }

  bool ExtractionScheduler::tryExtract_(Lock& lock)
  {
    // Units are issued strictly in commit order: never past a part that is not loaded yet. (Otherwise
    // results of later parts can fill the `ahead` window while the part next in commit order waits,
    // and only the one unit next to commit may run: measured, 18 s of one busy worker.)
    std::size_t expected = commit_part_;
    for (auto& [seq, part] : loaded_)
    {
      if (seq != expected) { return false; }
      if (part->issued >= part->plan.units.size()) { ++expected; continue; }
      const std::size_t unit = part->issued;
      const bool next_commit = seq == commit_part_ && unit == commit_unit_;
      if (outstanding_ >= opt_.ahead && !next_commit) { return false; }
      if (unwritten_ >= opt_.max_unwritten) { return false; }   // the sinks are behind: write first
      std::vector<float*> blocks;
      if (!pool_.tryAcquire(part->plan.units[unit].blocks, blocks))
      {
        if (!next_commit) { return false; }   // memory pressure: let the writers free blocks first
        ++stats_.heap_units;
      }
      ++part->issued;
      --unissued_;
      ++outstanding_;
      std::shared_ptr<Part> keep = part;
      begin_(SchedulerStats::Extract);
      const auto t0 = clk::now();
      lock.unlock();
      std::unique_ptr<TraceBlock> block;
      try
      {
        static prof::TaskStats& ts = prof::taskStats("extract");
        prof::TaskProbe probe(ts);
        block = newBlock_();
        block->arena.reset(&pool_, std::move(blocks));
        extractor_.extract(keep->lib, keep->plan, unit, *block);
      }
      catch (...)
      {
        relock_(lock);
        end_(SchedulerStats::Extract, t0);
        fail_();
        return true;
      }
      relock_(lock);
      end_(SchedulerStats::Extract, t0);
      keep->parked.emplace(unit, std::move(block));
      // tryCommit_ may erase nodes of loaded_, the map this loop iterates: nothing after it touches the loop's
      // iterator, the function returns at once.
      tryCommit_(lock);
      cv_.notify_all();
      return true;
    }
    return false;
  }

  void ExtractionScheduler::tryCommit_(Lock& lock)
  {
    if (committing_) { return; }
    committing_ = true;
    while (!error_)
    {
      auto it = loaded_.find(commit_part_);
      if (it == loaded_.end()) { break; }
      std::shared_ptr<Part> part = it->second;
      try
      {
        if (!begun_)
        {
          begun_ = true;
          lock.unlock();
          if (on_part) { on_part(part->lib, part->plan); }
          relock_(lock);
        }
        if (commit_unit_ == part->plan.units.size())
        {
          loaded_.erase(it);
          ++commit_part_;
          commit_unit_ = 0;
          begun_ = false;
          continue;
        }
        auto p = part->parked.find(commit_unit_);
        if (p == part->parked.end()) { break; }
        auto node = part->parked.extract(p);
        lock.unlock();
        if (on_unit) { on_unit(*node.mapped()); }
        relock_(lock);
        --outstanding_;
        ++commit_unit_;
        auto item = std::make_shared<Item>();
        item->part = part;
        item->block = std::move(node.mapped());
        writer_.sinksOf(unit_seq_++, sinks_scratch_);
        item->sinks_left = sinks_scratch_.size();
        for (const std::size_t s : sinks_scratch_) { sink_queue_[s].push_back(item); }
        if (item->sinks_left > 0) { ++unwritten_; }
        encode_queue_.push_back(std::move(item));
      }
      catch (...)
      {
        if (!lock.owns_lock()) { relock_(lock); }
        fail_();
      }
    }
    committing_ = false;
    cv_.notify_all();
  }
}
