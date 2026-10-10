// ODIA-V3 trace extractor -- central scheduling of library reading, extraction and writing.
#pragma once

#include "BufferPool.h"
#include "Extractor.h"
#include "Library.h"
#include "TraceWriter.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ODIA
{
  struct SchedulerOptions
  {
    int threads = 1;
    std::size_t ahead = 64;        ///< units extracted but not yet committed (results parked or running)
    std::size_t parts_ahead = 4;   ///< library parts loaded ahead of the part being committed
    std::size_t max_unwritten = 256;  ///< committed units not yet written (encoded batches waiting for sinks)
  };

  struct SchedulerStats
  {
    enum Class { Read = 0, Extract = 1, Encode = 2, Write = 3 };
    static constexpr int classes = 4;
    static constexpr const char* names[classes] = {"read+plan", "extract", "encode", "write"};
    double wall = 0;
    double busy[classes] = {};          ///< thread-seconds per task class
    double max_threads[classes] = {};   ///< most workers on one class at the same time
    std::uint64_t tasks[classes] = {};
    std::uint64_t heap_units = 0;        ///< units that ran without (enough) pool blocks
    std::size_t peak_parts = 0;          ///< library parts resident at once
    std::size_t part_objects = 0;        ///< library part objects allocated (then recycled)
  };

  /// One pool of workers, centrally scheduled. Four task classes share it and the share each gets
  /// follows the pressure in the pipeline, re-decided at every task:
  ///   1. write: append the next encoded unit to a sink (Parquet part / TSV) nobody is writing; sinks
  ///      are serial, so at most `sinks` workers;
  ///   2. encode: turn the oldest committed unit into an Arrow batch / TSV text (any number of workers);
  ///      its trace blocks go back to the pool right after, so encoding relieves memory pressure;
  ///   3. read + plan a library part, while fewer than 2 x threads units wait to be started;
  ///   4. extract the earliest unit not yet started, if its trace blocks can be reserved from the pool
  ///      and fewer than `ahead` results are uncommitted (the unit next in commit order always runs,
  ///      on the heap if the pool is empty, so the pipeline cannot stall);
  ///   5. read ahead (within `parts_ahead`), else wait.
  /// Results commit strictly in (part, unit) order (callbacks `on_part`, `on_unit`), then queue for
  /// encoding and for their sinks; a library part object returns to a recycling pool once its last
  /// unit is encoded. The output order is fixed by the library and options, never by the thread count.
  class ExtractionScheduler
  {
  public:
    ExtractionScheduler(LibraryReader& reader, const Extractor& extractor, TraceWriter& writer, BufferPool& pool,
                        const SchedulerOptions& opt);
    ~ExtractionScheduler();

    std::function<void(const Library&, const Extractor::Plan&)> on_part;   ///< ordered
    std::function<void(const TraceBlock&)> on_unit;                         ///< ordered

    void run();
    const SchedulerStats& stats() const { return stats_; }

  private:
    struct Part
    {
      Library lib;
      Extractor::Plan plan;
      std::size_t seq = 0, issued = 0;
      std::map<std::size_t, std::unique_ptr<TraceBlock>> parked;
    };
    struct Item
    {
      std::shared_ptr<Part> part;   // the library part stays alive until the unit is encoded
      std::unique_ptr<TraceBlock> block;
      TraceWriter::Encoded encoded;
      bool ready = false;           // encoded
      std::size_t sinks_left = 0;   // sinks still to write it
    };
    using Lock = std::unique_lock<std::mutex>;

    void worker_();
    bool tryWrite_(Lock& lock);
    bool tryEncode_(Lock& lock);
    bool tryRead_(Lock& lock, bool low_water_only);
    bool tryExtract_(Lock& lock);
    void tryCommit_(Lock& lock);
    void fail_();
    void begin_(int cls);   // with the lock held
    void end_(int cls, std::chrono::steady_clock::time_point t0);
    bool finished_() const;
    std::shared_ptr<Part> newPart_();
    std::unique_ptr<TraceBlock> newBlock_();
    void recycle_(std::unique_ptr<TraceBlock> b);

    LibraryReader& reader_;
    const Extractor& extractor_;
    TraceWriter& writer_;
    BufferPool& pool_;
    SchedulerOptions opt_;
    SchedulerStats stats_;

    std::mutex mtx_;
    std::condition_variable cv_, cv_mon_;
    std::map<std::size_t, std::shared_ptr<Part>> loaded_;
    std::vector<bool> source_busy_, sink_busy_;
    std::vector<std::deque<std::shared_ptr<Item>>> sink_queue_;
    std::deque<std::shared_ptr<Item>> encode_queue_;
    std::size_t encoding_ = 0;
    std::size_t unwritten_ = 0;   // committed units not yet written to every sink (bounded: back-pressure)
    std::size_t commit_part_ = 0, commit_unit_ = 0, outstanding_ = 0, unissued_ = 0;
    std::uint64_t unit_seq_ = 0;
    bool committing_ = false, begun_ = false;
    int active_[SchedulerStats::classes] = {};
    std::exception_ptr error_;
    std::vector<std::size_t> sinks_scratch_;

    // recycling pools (own mutex: deleters may run while the scheduler lock is held)
    std::mutex recycle_mtx_;
    std::vector<std::unique_ptr<Part>> free_parts_;
    std::vector<std::unique_ptr<TraceBlock>> free_blocks_;
  };
}
