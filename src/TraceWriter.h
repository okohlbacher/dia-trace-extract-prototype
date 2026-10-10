// ODIA-V3 trace extractor -- trace export.
#pragma once

#include "Extractor.h"
#include "Library.h"
#include "RunGrid.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace arrow { class RecordBatch; class Schema; }

namespace ODIA
{
  /// Writes traces in two stages:
  ///  1. encode(block) -- const and thread-safe; the extraction workers call it, so building the
  ///     Arrow arrays and the TSV text runs in parallel;
  ///  2. push(encoded) -- in unit order; a writer thread appends to the files (Parquet buffers
  ///     row groups and compresses columns on its own threads).
  ///
  /// Outputs:
  ///  - Parquet traces (one row per trace = fragment or MS1 isotope): `cycle_begin` on the global
  ///    axis plus the intensity list. Encoding `float32` (exact) or `log8` (1 byte per point,
  ///    log-spaced over `log8_decades` below the per-trace maximum `intensity_scale`; 0 = zero).
  ///    `expand_rt` adds the RT list per row for convenience.
  ///  - Parquet axes: the global cycle axis (lane -1), exact acquisition RT of every window
  ///    spectrum (lane = window index), MS1 scan times (lane -2, cycle = scan number).
  ///  - TSV: long format, one row per point on the global axis.
  class TraceWriter
  {
  public:
    struct Config
    {
      std::string parquet, axes, tsv;
      std::string run_name;
      bool log8 = false;
      double log8_decades = 6.0;
      bool expand_rt = false;
      bool skip_empty = false;
      bool isotope_unit_spacing = false;
      std::int64_t rows_per_group = 1 << 17;
      std::vector<std::pair<std::string, std::string>> metadata;  ///< written into the Parquet footer
    };

    struct Encoded
    {
      std::shared_ptr<arrow::RecordBatch> batch;  ///< null without Parquet output
      std::string tsv;
      std::uint64_t rows = 0, points = 0;
    };

    TraceWriter(const Library& lib, const RunGrid& grid, Config cfg);
    ~TraceWriter();

    Encoded encode(const TraceBlock& block) const;
    /// Queue an encoded block (in unit order; blocks while the bounded queue is full).
    void push(Encoded&& e);
    /// Drain, close files; rethrows a writer-thread error.
    void close();

    std::uint64_t rows() const { return rows_; }
    std::uint64_t points() const { return points_; }
    double busySeconds() const { return busy_; }

  private:
    struct Files;
    void run_();
    void writeAxes_();

    const Library& lib_;
    const RunGrid& grid_;
    Config cfg_;
    std::shared_ptr<arrow::Schema> schema_;
    std::unique_ptr<Files> files_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Encoded> queue_;
    bool closing_ = false, closed_ = false;
    std::exception_ptr error_;
    std::thread thread_;
    std::uint64_t rows_ = 0, points_ = 0;
    double busy_ = 0;
  };
}
