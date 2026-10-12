// ODIA-V3 trace extractor -- trace export.
#include "TraceWriter.h"
#include "ArrowPool.h"
#include "ArrowCheck.h"

#include <algorithm>
#include <cctype>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/writer.h>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <chrono>
#include <exception>
#include <thread>
#include <cmath>
#include <stdexcept>

namespace ODIA
{
  namespace
  {
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
                     ->memory_pool(arrowPool())
                     ->build();
      // Column encoding/compression stays on the calling worker: the scheduler parallelises across sinks.
      // store_schema(): without it the schema's key-value metadata (odia.*) is not written to the footer.
      auto arrow_props = parquet::ArrowWriterProperties::Builder().set_use_threads(false)->store_schema()->build();
      return check(parquet::arrow::FileWriter::Open(schema, arrowPool(), out, props, arrow_props));
    }
  }

  /// One Parquet part file.
  struct TraceWriter::Shard
  {
    std::shared_ptr<arrow::io::FileOutputStream> out;
    std::unique_ptr<parquet::arrow::FileWriter> writer;
    double busy = 0;
  };

  TraceWriter::TraceWriter(const RunGrid& grid, Config cfg)
    : grid_(grid), cfg_(std::move(cfg))
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
    md->Append("odia.axis", cfg_.ms1_scan_axis
      ? "MS2 rows: global DIA cycle axis, point i at axes[lane=-1, cycle=cycle_begin+i].rt; MS1 rows: MS1 scans, "
        "point i at axes[lane=-2, cycle=cycle_begin+i].rt (raw values)"
      : "global DIA cycle axis; point i of a row is at axes[lane=-1, cycle=cycle_begin+i].rt (MS1 interpolated)");
    md->Append("odia.ms1_axis", cfg_.ms1_scan_axis ? "ms1_scans" : "cycles");
    md->Append("odia.encoding", cfg_.log8 ? "log8" : "float32");
    md->Append("odia.array_layout", "intensity/rt are binary blobs: little-endian float32[n] (intensity_log8: uint8[n]), "
                                    "n = point count; decode e.g. numpy.frombuffer(b, '<f4')");
    md->Append("odia.log8_decades", std::to_string(cfg_.log8_decades));
    md->Append("odia.ms1_isotope_spacing", cfg_.isotope_unit_spacing ? "unit" : "charge");
    for (const auto& [k, v] : cfg_.metadata) { md->Append("odia." + k, v); }
    schema_ = arrow::schema(f, md);

    if (!cfg_.parquet.empty())
    {
      const int n = std::max(1, cfg_.shards);
      std::vector<std::string> paths;
      if (n == 1) { paths.push_back(cfg_.parquet); }
      else
      {
        namespace fs = std::filesystem;
        if (fs::exists(cfg_.parquet) && !fs::is_directory(cfg_.parquet)) { fs::remove(cfg_.parquet); }
        fs::create_directories(cfg_.parquet);
        // Stale parts of an earlier run (a reader would take them for ours): only files named like our own
        // shards, part-NNN.parquet, are removed; anything else in the directory is left alone.
        for (const auto& e : fs::directory_iterator(cfg_.parquet))
        {
          const std::string f = e.path().filename().string();
          const bool ours = f.size() == 16 && f.starts_with("part-") && f.ends_with(".parquet")
                            && std::all_of(f.begin() + 5, f.begin() + 8, [](unsigned char c) { return std::isdigit(c) != 0; });
          if (ours && e.is_regular_file()) { fs::remove(e.path()); }
        }
        for (int i = 0; i < n; ++i)
        {
          char name[32];
          std::snprintf(name, sizeof name, "part-%03d.parquet", i);
          paths.push_back((fs::path(cfg_.parquet) / name).string());
        }
      }
      for (const auto& path : paths)
      {
        auto sh = std::make_unique<Shard>();
        // Rows buffered per part until its row group is flushed: a fixed total over all parts (more parts =
        // smaller row groups), so the writer's memory does not grow with the part count.
        const std::int64_t rows = std::max<std::int64_t>(16384, cfg_.rows_per_group * 8 / static_cast<std::int64_t>(paths.size()));
        sh->writer = openWriter(path, *schema_, rows, sh->out);
        shards_.push_back(std::move(sh));
      }
    }
    if (!cfg_.axes.empty()) { writeAxes_(); }
    if (!cfg_.tsv.empty())
    {
      tsv_ = std::fopen(cfg_.tsv.c_str(), "wb");
      if (!tsv_) { throw std::runtime_error("cannot write " + cfg_.tsv); }
      std::fputs("run\tprecursor_id\tmodified_sequence\tprecursor_charge\tdecoy\tms_level\tannotation\t"
                 "target_mz\tcycle\trt\tintensity\n", tsv_);
    }
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

  void TraceWriter::sinksOf(std::uint64_t unit, std::vector<std::size_t>& out) const
  {
    out.clear();
    if (!shards_.empty()) { out.push_back(static_cast<std::size_t>(unit % shards_.size())); }
    if (tsv_) { out.push_back(shards_.size()); }
  }

  double TraceWriter::busySeconds() const
  {
    double b = tsv_busy_;
    for (const auto& sh : shards_) { b = std::max(b, sh->busy); }
    return b;
  }

  TraceWriter::Encoded TraceWriter::encode(const TraceBlock& block, const Library& lib) const
  {
    const bool pq = !shards_.empty(), tsv = tsv_ != nullptr;
    Encoded e;
    arrow::MemoryPool* pool = arrowPool();
    arrow::StringBuilder run(pool), id(pool), seq(pool), ann(pool), ftype(pool);
    arrow::UInt8Builder charge(pool), decoy(pool), level(pool), ford(pool), fz(pool), floss(pool);
    arrow::DoubleBuilder pmz(pool), tmz(pool);
    arrow::Int32Builder window(pool);
    arrow::Int8Builder iso(pool);
    arrow::FloatBuilder libint(pool), rtc(pool), scale(pool);
    arrow::UInt32Builder cycle_begin(pool);
    // Point arrays as binary blobs: one Parquet value per trace instead of per point (nested
    // lists cost repetition/definition levels for every point on the serial write path).
    arrow::BinaryBuilder inten_f(pool), inten_q(pool), rtl(pool);
    std::vector<std::uint8_t> qbuf;
    std::vector<float> rtbuf;
    const double decades = cfg_.log8_decades;
    if (pq)
    {
      // Size every builder once from the block (upper bounds: skipped empty traces only leave slack), so
      // appends never reallocate: no grow-copy-free cycles through the pool, no fresh pages mid-encode.
      std::int64_t rows = 0, pts = 0, id_bytes = 0, seq_bytes = 0;
      for (const auto& r : block.records)
      {
        const std::int64_t n = static_cast<std::int64_t>(r.fragments) + r.isotopes;
        rows += n;
        pts += static_cast<std::int64_t>(r.fragments) * r.cycles + static_cast<std::int64_t>(r.isotopes) * r.ms1_count;
        id_bytes += n * static_cast<std::int64_t>(lib.precursor_id[r.precursor].size());
        seq_bytes += n * static_cast<std::int64_t>(lib.modified_sequence[r.precursor].size());
      }
      for (arrow::ArrayBuilder* b : std::initializer_list<arrow::ArrayBuilder*>{&run, &id, &seq, &ann, &ftype, &charge, &decoy, &level,
                                                                               &ford, &fz, &floss, &pmz, &tmz, &window, &iso, &libint,
                                                                               &rtc, &scale, &cycle_begin})
      {
        check(b->Reserve(rows));
      }
      check(run.ReserveData(rows * static_cast<std::int64_t>(cfg_.run_name.size())));
      check(id.ReserveData(id_bytes));
      check(seq.ReserveData(seq_bytes));
      check(ann.ReserveData(rows * 8));
      check(ftype.ReserveData(rows));
      if (cfg_.log8) { check(inten_q.Reserve(rows)); check(inten_q.ReserveData(pts)); }
      else { check(inten_f.Reserve(rows)); check(inten_f.ReserveData(pts * static_cast<std::int64_t>(sizeof(float)))); }
      if (cfg_.expand_rt) { check(rtl.Reserve(rows)); check(rtl.ReserveData(pts * static_cast<std::int64_t>(sizeof(float)))); }
    }

    char num[64];

    for (const auto& r : block.records)
    {
      const std::uint32_t p = r.precursor;
      const std::size_t n_traces = static_cast<std::size_t>(r.fragments) + r.isotopes;
      for (std::size_t k = 0; k < n_traces; ++k)
      {
        const bool ms2 = k < r.fragments;
        const std::size_t f = ms2 ? lib.fragment_begin[p] + k : 0;
        const int isotope = ms2 ? -1 : static_cast<int>(k - r.fragments);
        // MS2 on the cycle axis; MS1 on its own range (MS1 scans, or the cycle range when interpolated).
        const std::size_t T = ms2 ? r.cycles : r.ms1_count;
        const std::uint32_t begin = ms2 ? r.cycle_begin : r.ms1_begin;
        const float* v = ms2 ? r.values + k * r.cycles
                             : r.values + static_cast<std::size_t>(r.fragments) * r.cycles + static_cast<std::size_t>(isotope) * r.ms1_count;
        const auto time_of = [&](std::size_t i) {
          return (!ms2 && cfg_.ms1_scan_axis) ? grid_.ms1.rt[begin + i] : grid_.cycle_rt[begin + i];
        };
        float vmax = 0.0f;
        for (std::size_t i = 0; i < T; ++i) { vmax = std::max(vmax, v[i]); }
        if (cfg_.skip_empty && vmax <= 0.0f) { continue; }
        const double z = cfg_.isotope_unit_spacing ? 1.0 : std::max<int>(1, lib.charge[p]);
        const double target = ms2 ? lib.fragment_mz[f] : lib.precursor_mz[p] + isotope * C13_C12 / z;
        const std::string a = ms2 ? lib.annotation(f) : "M+" + std::to_string(isotope);
        ++e.rows;
        e.points += T;
        if (pq)
        {
          check(run.Append(cfg_.run_name));
          check(id.Append(lib.precursor_id[p]));
          check(seq.Append(lib.modified_sequence[p]));
          check(charge.Append(lib.charge[p]));
          check(decoy.Append(lib.decoy[p]));
          check(pmz.Append(lib.precursor_mz[p]));
          check(window.Append(r.window));
          check(level.Append(ms2 ? 2 : 1));
          check(ann.Append(a));
          if (ms2)
          {
            check(ftype.Append(std::string(1, lib.fragment_type[f])));
            check(ford.Append(lib.fragment_ordinal[f]));
            check(fz.Append(lib.fragment_charge[f]));
            check(floss.Append(lib.fragment_loss[f]));
            check(iso.AppendNull());
            check(libint.Append(lib.fragment_intensity[f]));
          }
          else
          {
            check(ftype.AppendNull()); check(ford.AppendNull()); check(fz.AppendNull()); check(floss.AppendNull());
            check(iso.Append(static_cast<std::int8_t>(isotope)));
            check(libint.AppendNull());
          }
          check(tmz.Append(target));
          check(rtc.Append(r.rt_center));
          check(cycle_begin.Append(begin));
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
            for (std::size_t i = 0; i < T; ++i) { rtbuf[i] = static_cast<float>(time_of(i)); }
            check(rtl.Append(reinterpret_cast<const std::uint8_t*>(rtbuf.data()), static_cast<std::int32_t>(T * sizeof(float))));
          }
        }
        if (tsv)
        {
          auto& b = e.tsv;
          for (std::size_t i = 0; i < T; ++i)
          {
            b += cfg_.run_name; b += '\t'; b += lib.precursor_id[p]; b += '\t';
            b += lib.modified_sequence[p]; b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, static_cast<int>(lib.charge[p])).ptr); b += '\t';
            b += lib.decoy[p] ? '1' : '0'; b += '\t';
            b += ms2 ? '2' : '1'; b += '\t';
            b += a; b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, target).ptr); b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, begin + i).ptr); b += '\t';
            b.append(num, std::to_chars(num, num + sizeof num, time_of(i)).ptr); b += '\t';
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

  void TraceWriter::append(std::size_t sink, const Encoded& e)
  {
    const auto t0 = std::chrono::steady_clock::now();
    const bool pq = sink < shards_.size();
    if (pq)
    {
      if (e.batch) { check(shards_[sink]->writer->WriteRecordBatch(*e.batch)); }
      shards_[sink]->busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    else
    {
      if (!e.tsv.empty() && std::fwrite(e.tsv.data(), 1, e.tsv.size(), tsv_) != e.tsv.size()) { throw std::runtime_error("TSV write failed"); }
      tsv_busy_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    // Rows and points are counted once per unit: on its Parquet part, or on the TSV without Parquet.
    if (pq || shards_.empty()) { rows_ += e.rows; points_ += e.points; }
  }

  void TraceWriter::close()
  {
    if (closed_) { return; }
    closed_ = true;
    // Each shard flushes its last buffered row group and footer: independent, so in parallel.
    std::vector<std::exception_ptr> errors(shards_.size());
    std::vector<std::thread> closers;
    closers.reserve(shards_.size());
    for (std::size_t i = 0; i < shards_.size(); ++i)
    {
      closers.emplace_back([this, i, &errors] {
        try
        {
          check(shards_[i]->writer->Close());
          check(shards_[i]->out->Close());
        }
        catch (...) { errors[i] = std::current_exception(); }
      });
    }
    for (auto& t : closers) { t.join(); }
    for (auto& e : errors) { if (e) { std::rethrow_exception(e); } }
    if (tsv_) { const bool ok = std::fclose(tsv_) == 0; tsv_ = nullptr; if (!ok) { throw std::runtime_error("TSV close failed"); } }
  }
}
