// ODIA-V3 trace extractor -- central scheduling of library reading, extraction and writing.
#include "Scheduler.h"

#include <algorithm>
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
    if (std::getenv("ODIA_PROFILE") != nullptr)
    {
      monitor = std::thread([&] {
        Lock lock(mtx_);
        while (!stop)
        {
          cv_mon_.wait_for(lock, std::chrono::milliseconds(500));
          if (stop) { break; }
          std::fprintf(stderr, "[sched %6.1f s] active read %d extract %d encode %d write %d | outstanding %zu unissued %zu "
                       "loaded parts %zu (commit part %zu unit %zu) | encode queue %zu unwritten %zu | pool free %zu\n",
                       std::chrono::duration<double>(clk::now() - t0).count(), active_[0], active_[1], active_[2], active_[3],
                       outstanding_, unissued_, loaded_.size(), commit_part_, commit_unit_, encode_queue_.size(), unwritten_,
                       pool_.available());
        }
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
      cv_.wait(lock);
    }
    cv_.notify_all();
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
      writer_.append(best, item->encoded);
    }
    catch (...)
    {
      lock.lock();
      sink_busy_[best] = false;
      end_(SchedulerStats::Write, t0);
      fail_();
      return true;
    }
    lock.lock();
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
      item->encoded = writer_.encode(*item->block, item->part->lib);
      // Encoded: discard the traces now (blocks back to the pool) and release the library part.
      recycle_(std::move(item->block));
      item->part.reset();
    }
    catch (...)
    {
      lock.lock();
      --encoding_;
      end_(SchedulerStats::Encode, t0);
      fail_();
      return true;
    }
    lock.lock();
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
        part = newPart_();
        part->seq = reader_.read(src, part->lib);
        part->plan = extractor_.plan(part->lib);
      }
      catch (...)
      {
        lock.lock();
        source_busy_[src] = false;
        end_(SchedulerStats::Read, t0);
        fail_();
        return true;
      }
      lock.lock();
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
        block = newBlock_();
        block->arena.reset(&pool_, std::move(blocks));
        extractor_.extract(keep->lib, keep->plan, unit, *block);
      }
      catch (...)
      {
        lock.lock();
        end_(SchedulerStats::Extract, t0);
        fail_();
        return true;
      }
      lock.lock();
      end_(SchedulerStats::Extract, t0);
      keep->parked.emplace(unit, std::move(block));
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
          lock.lock();
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
        lock.lock();
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
        if (!lock.owns_lock()) { lock.lock(); }
        fail_();
      }
    }
    committing_ = false;
    cv_.notify_all();
  }
}
