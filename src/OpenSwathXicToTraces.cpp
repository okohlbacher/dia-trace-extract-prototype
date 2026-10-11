// ODIA-V3 -- convert an OpenSWATH .xic (OpenSwathWorkflow -out_chrom x.xic) into the comparison layout:
// one row per trace with precursor_id (= modified sequence + charge), ms_level, annotation and the
// point arrays as little-endian float32 blobs (rt in seconds). Reads through OpenMS's XICParquetFile,
// which decodes the numpress/zlib arrays.
#include <OpenMS/FORMAT/XICParquetFile.h>

#include "ArrowCheck.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
  using ODIA::check;

  /// OpenSWATH writes "b2^1" and "Precursor_i0"; the comparison layout uses "b2" and "M+0".
  std::string normaliseAnnotation(const std::string& a, std::int64_t ms_level)
  {
    if (ms_level == 1)
    {
      const auto p = a.rfind("_i");
      return "M+" + (p == std::string::npos ? std::string("0") : a.substr(p + 2));
    }
    if (a.size() > 2 && a.compare(a.size() - 2, 2, "^1") == 0) { return a.substr(0, a.size() - 2); }
    return a;
  }
}

int main(int argc, char** argv)
{
  if (argc != 3)
  {
    std::cerr << "usage: OpenSwathXicToTraces in.xic out.parquet\n";
    return 1;
  }
  try
  {
    std::vector<OpenMS::XICParquetFile::XICChromatogram> chroms;
    OpenMS::XICParquetFile(argv[1]).load(chroms);

    arrow::StringBuilder pid, ann;
    arrow::UInt8Builder level;
    arrow::BinaryBuilder rt, inten;
    std::vector<float> buf;
    auto blob = [&](arrow::BinaryBuilder& b, const std::vector<double>& v) {
      buf.assign(v.begin(), v.end());
      check(b.Append(reinterpret_cast<const std::uint8_t*>(buf.data()), static_cast<std::int32_t>(buf.size() * sizeof(float))));
    };
    for (const auto& c : chroms)
    {
      check(pid.Append(c.modified_sequence + std::to_string(c.precursor_charge)));
      check(level.Append(static_cast<std::uint8_t>(c.ms_level)));
      check(ann.Append(normaliseAnnotation(c.annotation, c.ms_level)));
      blob(rt, c.rt);
      blob(inten, c.intensity);
    }
    auto schema = arrow::schema({arrow::field("precursor_id", arrow::utf8()), arrow::field("ms_level", arrow::uint8()),
                                 arrow::field("annotation", arrow::utf8()), arrow::field("rt", arrow::binary()),
                                 arrow::field("intensity", arrow::binary())});
    std::vector<std::shared_ptr<arrow::Array>> cols(5);
    check(pid.Finish(&cols[0])); check(level.Finish(&cols[1])); check(ann.Finish(&cols[2]));
    check(rt.Finish(&cols[3])); check(inten.Finish(&cols[4]));
    auto table = arrow::Table::Make(schema, cols);
    auto out = check(arrow::io::FileOutputStream::Open(argv[2]));
    check(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 1 << 17));
    check(out->Close());
    std::cout << chroms.size() << " chromatograms -> " << argv[2] << "\n";
  }
  catch (const std::exception& e)
  {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
