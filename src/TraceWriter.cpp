// ODIA-V3 trace extractor -- trace export.
#include "TraceWriter.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/writer.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace ODIA
{
  namespace
  {
    void check(const arrow::Status& s)
    {
      if (!s.ok()) { throw std::runtime_error("Arrow: " + s.ToString()); }
    }
    template <typename T> T check(arrow::Result<T> r)
    {
      if (!r.ok()) { throw std::runtime_error("Arrow: " + r.status().ToString()); }
      return std::move(r).ValueUnsafe();
    }
    template <typename B> std::shared_ptr<arrow::Array> finish(B& b)
    {
      std::shared_ptr<arrow::Array> a;
      check(b.Finish(&a));
      return a;
    }

    constexpr double C13_C12 = 1.0033548378;

    std::unique_ptr<parquet::arrow::FileWriter> openWriter(const std::string& file, const arrow::Schema& schema,
                                                           std::int64_t rows_per_group,
                                                           std::shared_ptr<arrow::io::FileOutputStream>& out)
    {
      out = check(arrow::io::FileOutputStream::Open(file));
      auto props = parquet::WriterProperties::Builder()
                     .compression(parquet::Compression::ZSTD)
                     ->compression_level(3)
                     ->enable_dictionary()
                     ->disable_dictionary("intensity")
                     ->disable_dictionary("intensity_log8")
                     ->disable_dictionary("rt")
                     ->max_row_group_length(rows_per_group)
                     ->build();
      auto arrow_props = parquet::ArrowWriterProperties::Builder().set_use_threads(true)->build();
      return check(parquet::arrow::FileWriter::Open(schema, arrow::default_memory_pool(), out, props, arrow_props));
    }
  }

  struct TraceWriter::Files
  {
    std::shared_ptr<arrow::io::FileOutputStream> out;
    std::unique_ptr<parquet::arrow::FileWriter> writer;
    std::FILE* tsv = nullptr;
  };

  TraceWriter::TraceWriter(const Library& lib, const RunGrid& grid, Config cfg)
    : lib_(lib), grid_(grid), cfg_(std::move(cfg)), files_(std::make_unique<Files>())
  {
    arrow::FieldVector f = {
      arrow::field("run", arrow::utf8()),
      arrow::field("precursor_id", arrow::utf8()),
      arrow::field("modified_sequence", arrow::utf8()),
      arrow::field("precursor_charge", arrow::uint8()),
      arrow::field("decoy", arrow::uint8()),
      arrow::field("precursor_mz", arrow::float64()),
      arrow::field("window", arrow::int32()),
      arrow::field("ms_level", arrow::uint8()),
      arrow::field("annotation", arrow::utf8()),
      arrow::field("fragment_type", arrow::utf8()),
      arrow::field("fragment_ordinal", arrow::uint8()),
      arrow::field("fragment_charge", arrow::uint8()),
      arrow::field("fragment_loss", arrow::uint8()),
      arrow::field("isotope", arrow::int8()),
      arrow::field("target_mz", arrow::float64()),
      arrow::field("library_intensity", arrow::float32()),
      arrow::field("rt_center", arrow::float32()),
      arrow::field("cycle_begin", arrow::uint32()),
    };
    if (cfg_.log8)
    {
      f.push_back(arrow::field("intensity_scale", arrow::float32()));
      f.push_back(arrow::field("intensity_log8", arrow::binary()));
    }
    else
    {
      f.push_back(arrow::field("intensity", arrow::binary()));
    }
    if (cfg_.expand_rt) { f.push_back(arrow::field("rt", arrow::binary())); }
    auto md = std::make_shared<arrow::KeyValueMetadata>();
    md->Append("odia.format", "odia-traces-v1");
    md->Append("odia.axis", "global DIA cycle axis; point i of a row is at axes[lane=-1, cycle=cycle_begin+i].rt");
    md->Append("odia.encoding", cfg_.log8 ? "log8" : "float32");
    md->Append("odia.array_layout", "intensity/rt are binary blobs: little-endian float32[n] (intensity_log8: uint8[n]), "
                                    "n = point count; decode e.g. numpy.frombuffer(b, '<f4')");
    md->Append("odia.log8_decades", std::to_string(cfg_.log8_decades));
    md->Append("odia.ms1_isotope_spacing", cfg_.isotope_unit_spacing ? "unit" : "charge");
    for (const auto& [k, v] : cfg_.metadata) { md->Append("odia." + k, v); }
    schema_ = arrow::schema(f, md);

    if (!cfg_.parquet.empty()) { files_->writer = openWriter(cfg_.parquet, *schema_, cfg_.rows_per_group, files_->out); }
    if (!cfg_.axes.empty()) { writeAxes_(); }
    if (!cfg_.tsv.empty())
    {
      files_->tsv = std::fopen(cfg_.tsv.c_str(), "wb");
      if (!files_->tsv) { throw std::runtime_error("cannot write " + cfg_.tsv); }
      std::fputs("run\tprecursor_id\tmodified_sequence\tprecursor_charge\tdecoy\tms_level\tannotation\t"
                 "target_mz\tcycle\trt\tintensity\n", files_->tsv);
    }
    thread_ = std::thread([this] { run_(); });
  }

  TraceWriter::~TraceWriter()
  {
    try { close(); } catch (...) {}
  }

  void TraceWriter::writeAxes_()
  {
    auto schema = arrow::schema({
      arrow::field("lane", arrow::int32()),
      arrow::field("cycle", arrow::uint32()),
      arrow::field("rt", arrow::float64()),
      arrow::field("spectrum", arrow::int64()),
      arrow::field("window_target", arrow::float64()),
      arrow::field("window_lower", arrow::float64()),
      arrow::field("window_upper", arrow::float64()),
    });
    arrow::Int32Builder lane;
    arrow::UInt32Builder cycle;
    arrow::DoubleBuilder rt, wt, wl, wu;
    arrow::Int64Builder spec;
    auto row = [&](int l, std::size_t c, double t, std::int64_t s, double a, double b, double d) {
      check(lane.Append(l)); check(cycle.Append(static_cast<std::uint32_t>(c))); check(rt.Append(t));
      if (s < 0) { check(spec.AppendNull()); } else { check(spec.Append(s)); }
      if (l < 0) { check(wt.AppendNull()); check(wl.AppendNull()); check(wu.AppendNull()); }
      else { check(wt.Append(a)); check(wl.Append(b)); check(wu.Append(d)); }
    };
    for (std::size_t c = 0; c < grid_.cycles(); ++c) { row(-1, c, grid_.cycle_rt[c], -1, 0, 0, 0); }
    for (std::size_t w = 0; w < grid_.windows.size(); ++w)
    {
      const auto& L = grid_.windows[w];
      for (std::size_t c = 0; c < L.acquired_rt.size(); ++c)
      {
        if (std::isnan(L.acquired_rt[c])) { continue; }
        row(static_cast<int>(w), c, L.acquired_rt[c], L.spectrum[c], L.target, L.lower, L.upper);
      }
    }
    for (std::size_t j = 0; j < grid_.ms1.rt.size(); ++j) { row(-2, j, grid_.ms1.rt[j], grid_.ms1.spectrum[j], 0, 0, 0); }
    const std::int64_t n = lane.length(); // before finish(): it resets the builder
    auto batch = arrow::RecordBatch::Make(schema, n,
      {finish(lane), finish(cycle), finish(rt), finish(spec), finish(wt), finish(wl), finish(wu)});
    std::shared_ptr<arrow::io::FileOutputStream> out;
    auto w = openWriter(cfg_.axes, *schema, 1 << 20, out);
    check(w->WriteRecordBatch(*batch));
    check(w->Close());
    check(out->Close());
  }

  TraceWriter::Encoded TraceWriter::encode(const TraceBlock& block) const
  {
    Encoded e;
    const bool pq = !cfg_.parquet.empty(), tsv = !cfg_.tsv.empty();
    arrow::StringBuilder run, id, seq, ann, ftype;
    arrow::UInt8Builder charge, decoy, level, ford, fz, floss;
    arrow::DoubleBuilder pmz, tmz;
    arrow::Int32Builder window;
    arrow::Int8Builder iso;
    arrow::FloatBuilder libint, rtc, scale;
    arrow::UInt32Builder cycle_begin;
    // Point arrays as binary blobs: one Parquet value per trace instead of per point (nested
    // lists cost repetition/definition levels for every point on the serial write path).
    arrow::BinaryBuilder inten_f, inten_q, rtl;
    std::vector<std::uint8_t> qbuf;
    std::vector<float> rtbuf;
    const double decades = cfg_.log8_decades;
    char num[64];

    for (const auto& r : block.records)
    {
      const std::uint32_t p = r.precursor;
      const std::size_t T = r.cycles;
      const std::size_t n_traces = static_cast<std::size_t>(r.fragments) + r.isotopes;
      for (std::size_t k = 0; k < n_traces; ++k)
      {
        const bool ms2 = k < r.fragments;
        const std::size_t f = ms2 ? lib_.fragment_begin[p] + k : 0;
        const int isotope = ms2 ? -1 : static_cast<int>(k - r.fragments);
        const float* v = block.values.data() + r.value_offset + k * T;
        float vmax = 0.0f;
        for (std::size_t i = 0; i < T; ++i) { vmax = std::max(vmax, v[i]); }
        if (cfg_.skip_empty && vmax <= 0.0f) { continue; }
        const double z = cfg_.isotope_unit_spacing ? 1.0 : std::max<int>(1, lib_.charge[p]);
        const double target = ms2 ? lib_.fragment_mz[f] : lib_.precursor_mz[p] + isotope * C13_C12 / z;
        const std::string a = ms2 ? lib_.annotation(f) : "M+" + std::to_string(isotope);
        ++e.rows;
        e.points += T;
        if (pq)
        {
          check(run.Append(cfg_.run_name));
          check(id.Append(lib_.precursor_id[p]));
          check(seq.Append(lib_.modified_sequence[p]));
          check(charge.Append(lib_.charge[p]));
          check(decoy.Append(lib_.decoy[p]));
          check(pmz.Append(lib_.precursor_mz[p]));
          check(window.Append(r.window));
          check(level.Append(ms2 ? 2 : 1));
          check(ann.Append(a));
          if (ms2)
          {
            check(ftype.Append(std::string(1, lib_.fragment_type[f])));
            check(ford.Append(lib_.fragment_ordinal[f]));
            check(fz.Append(lib_.fragment_charge[f]));
            check(floss.Append(lib_.fragment_loss[f]));
            check(iso.AppendNull());
            check(libint.Append(lib_.fragment_intensity[f]));
          }
          else
          {
            check(ftype.AppendNull()); check(ford.AppendNull()); check(fz.AppendNull()); check(floss.AppendNull());
            check(iso.Append(static_cast<std::int8_t>(isotope)));
            check(libint.AppendNull());
          }
          check(tmz.Append(target));
          check(rtc.Append(r.rt_center));
          check(cycle_begin.Append(r.cycle_begin));
          if (cfg_.log8)
          {
            check(scale.Append(vmax));
            qbuf.resize(T);
            for (std::size_t i = 0; i < T; ++i)
            {
              std::uint8_t q = 0;
              if (v[i] > 0.0f && vmax > 0.0f)
              {
                // 1..255 spans [vmax * 10^-decades, vmax] log-uniformly; 0 is reserved for zero.
                const double x = 1.0 + std::log10(static_cast<double>(v[i]) / vmax) / decades;
                q = static_cast<std::uint8_t>(std::clamp(1.0 + std::round(x * 254.0), 1.0, 255.0));
              }
              qbuf[i] = q;
            }
            check(inten_q.Append(qbuf.data(), static_cast<std::int32_t>(T)));
          }
          else
          {
            check(inten_f.Append(reinterpret_cast<const std::uint8_t*>(v), static_cast<std::int32_t>(T * sizeof(float))));
          }
          if (cfg_.expand_rt)
          {
            rtbuf.resize(T);
            for (std::size_t i = 0; i < T; ++i) { rtbuf[i] = static_cast<float>(grid_.cycle_rt[r.cycle_begin + i]); }
            check(rtl.Append(reinterpret_cast<const std::uint8_t*>(rtbuf.data()), static_cast<std::int32_t>(T * sizeof(float))));
          }
        }
        if (tsv)
        {
          auto& b = e.tsv;
          for (std::size_t i = 0; i < T; ++i)
          {
            b += cfg_.run_name; b += '\t'; b += lib_.precursor_id[p]; b += '\t';
            b += lib_.modified_sequence[p]; b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, static_cast<int>(lib_.charge[p])).ptr); b += '\t';
            b += lib_.decoy[p] ? '1' : '0'; b += '\t';
            b += ms2 ? '2' : '1'; b += '\t';
            b += a; b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, target).ptr); b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, r.cycle_begin + i).ptr); b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, grid_.cycle_rt[r.cycle_begin + i]).ptr); b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, v[i]).ptr); b += '\n';
          }
        }
      }
    }
    if (pq && e.rows > 0)
    {
      std::vector<std::shared_ptr<arrow::Array>> cols = {
        finish(run), finish(id), finish(seq), finish(charge), finish(decoy), finish(pmz), finish(window),
        finish(level), finish(ann), finish(ftype), finish(ford), finish(fz), finish(floss), finish(iso),
        finish(tmz), finish(libint), finish(rtc), finish(cycle_begin)};
      if (cfg_.log8) { cols.push_back(finish(scale)); cols.push_back(finish(inten_q)); }
      else { cols.push_back(finish(inten_f)); }
      if (cfg_.expand_rt) { cols.push_back(finish(rtl)); }
      e.batch = arrow::RecordBatch::Make(schema_, static_cast<std::int64_t>(e.rows), cols);
    }
    return e;
  }

  void TraceWriter::push(Encoded&& e)
  {
    std::unique_lock lock(mtx_);
    cv_.wait(lock, [&] { return queue_.size() < 32 || error_; });
    if (error_) { std::rethrow_exception(error_); }
    queue_.push_back(std::move(e));
    cv_.notify_all();
  }

  void TraceWriter::run_()
  {
    try
    {
      while (true)
      {
        Encoded e;
        {
          std::unique_lock lock(mtx_);
          cv_.wait(lock, [&] { return !queue_.empty() || closing_; });
          if (queue_.empty()) { break; }
          e = std::move(queue_.front());
          queue_.pop_front();
          cv_.notify_all();
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (e.batch) { check(files_->writer->WriteRecordBatch(*e.batch)); }
        if (files_->tsv && !e.tsv.empty()) { std::fwrite(e.tsv.data(), 1, e.tsv.size(), files_->tsv); }
        rows_ += e.rows;
        points_ += e.points;
        busy_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      }
      if (files_->writer)
      {
        check(files_->writer->Close());
        check(files_->out->Close());
      }
      if (files_->tsv) { std::fclose(files_->tsv); files_->tsv = nullptr; }
    }
    catch (...)
    {
      std::lock_guard lock(mtx_);
      error_ = std::current_exception();
      cv_.notify_all();
    }
  }

  void TraceWriter::close()
  {
    {
      std::lock_guard lock(mtx_);
      if (closed_) { return; }
      closing_ = closed_ = true;
      cv_.notify_all();
    }
    if (thread_.joinable()) { thread_.join(); }
    if (error_) { std::rethrow_exception(error_); }
  }
}
