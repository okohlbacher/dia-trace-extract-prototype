#pragma once
// One Parquet file written by several Parquet writers in parallel, without re-encoding: each writer writes into
// memory (PartStream); whenever a writer has completed row groups, their bytes are appended to the one output file
// (ParquetJoiner), concurrently with the extraction; at the end one footer lists the row groups of all parts, part by
// part, with every file offset translated to where its bytes landed.
#include <arrow/io/interfaces.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ODIA
{
  class ParquetJoiner
  {
  public:
    ParquetJoiner(const std::string& path, std::size_t parts);
    ~ParquetJoiner();
    /// Appends bytes [local, local + n) of part `part` (complete row groups) to the file.
    void append(std::size_t part, std::int64_t local, const std::uint8_t* data, std::int64_t n);
    /// The part's footer (FileMetaData, Thrift compact), once its writer has closed.
    void setFooter(std::size_t part, std::string footer);
    /// Writes the joined footer and closes the file (all parts must have their footer).
    void finish();
    double appendSeconds() const { return append_s_; }

  private:
    struct Segment { std::int64_t local, file, n; };
    std::string path_;
    std::FILE* f_ = nullptr;
    std::int64_t pos_ = 4;
    std::vector<std::vector<Segment>> segments_;
    std::vector<std::string> footers_;
    std::mutex m_;
    double append_s_ = 0;
  };

  /// An arrow OutputStream that buffers one Parquet part in memory and hands its completed row groups to a joiner
  /// on drain(); close() hands over the remaining row groups and the footer.
  class PartStream : public arrow::io::OutputStream
  {
  public:
    PartStream(ParquetJoiner& joiner, std::size_t part) : joiner_(joiner), part_(part) {}
    arrow::Status Close() override;
    bool closed() const override { return closed_; }
    arrow::Result<std::int64_t> Tell() const override { return pos_; }
    arrow::Status Write(const void* data, std::int64_t nbytes) override;
    /// Passes everything written so far (whole row groups, called after each record batch) to the joiner.
    void drain();

  private:
    ParquetJoiner& joiner_;
    std::size_t part_;
    std::vector<std::uint8_t> buf_;   ///< bytes not yet handed over, starting at local offset begin_
    std::int64_t begin_ = 0, pos_ = 0;
    bool closed_ = false;
  };
}
