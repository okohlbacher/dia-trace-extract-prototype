// ODIA-V3 trace extractor -- trace export.
#pragma once

#include "Extractor.h"
#include "Library.h"
#include "RunGrid.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace arrow { class RecordBatch; class Schema; }

namespace ODIA
{
  /// Writes traces to output *sinks*: the Parquet part files (`part-NNN.parquet`, or one file) and the
  /// long TSV. The writer owns no threads: the caller (the extraction scheduler) writes each sink
  /// serially from any worker thread, and different sinks concurrently. A write encodes the block
  /// (Arrow arrays or TSV text) and appends it; Parquet compresses each row group on the calling thread.
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
      /// Parquet part files (unit u goes to part u % shards). 1 = a single file at `parquet`; > 1 =
      /// `parquet` is a directory of part-NNN.parquet files (one dataset). Fixed, not derived from the
      /// thread count, so the output does not depend on -threads.
      int shards = 8;
      std::vector<std::pair<std::string, std::string>> metadata;  ///< written into the Parquet footer
    };

    TraceWriter(const RunGrid& grid, Config cfg);
    ~TraceWriter();

    /// Sinks: Parquet parts 0 .. parquetSinks()-1, then the TSV (if configured).
    std::size_t sinks() const { return shards_.size() + (tsv_ ? 1 : 0); }
    std::size_t parquetSinks() const { return shards_.size(); }
    bool hasTsv() const { return tsv_ != nullptr; }
    /// Sinks a unit (commit sequence number) goes to.
    void sinksOf(std::uint64_t unit, std::vector<std::size_t>& out) const;
    /// Encode `block` (extracted from library part `lib`) and append it to `sink`. One call at a time
    /// per sink, in unit order; different sinks may be written concurrently.
    void write(std::size_t sink, const TraceBlock& block, const Library& lib);
    /// Close the files.
    void close();

    std::uint64_t rows() const { return rows_; }
    std::uint64_t points() const { return points_; }
    double busySeconds() const;   ///< max over sinks of the time spent writing
    std::size_t shardCount() const { return shards_.size(); }

  private:
    struct Shard;
    void writeAxes_();

    const RunGrid& grid_;
    Config cfg_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::FILE* tsv_ = nullptr;
    double tsv_busy_ = 0;
    bool closed_ = false;
    std::atomic<std::uint64_t> rows_{0}, points_{0};
  };
}
