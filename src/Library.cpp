// ODIA-V3 trace extractor -- library readers.
#include "Library.h"

#include <OpenMS/ANALYSIS/OPENSWATH/TransitionPQPFile.h>
#include <OpenMS/ANALYSIS/OPENSWATH/TransitionTSVFile.h>
#include <OpenMS/ANALYSIS/OPENSWATH/DATAACCESS/DataAccessHelper.h>
#include <OpenMS/ANALYSIS/TARGETED/TargetedExperiment.h>
#include <OpenMS/FORMAT/FileHandler.h>
#include <OpenMS/FORMAT/TraMLFile.h>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <omp.h>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace ODIA
{
  namespace
  {
    bool endsWith(std::string_view s, std::string_view suffix)
    {
      if (s.size() < suffix.size()) { return false; }
      for (std::size_t i = 0; i < suffix.size(); ++i)
      {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i]) { return false; }
      }
      return true;
    }

    // DIALibGen / ODIA fragment type and loss codes (odia/Library.h: FragmentType, LossType).
    char typeFromCode(std::uint8_t c)
    {
      static constexpr char map[] = {'?', 'a', 'b', 'c', 'x', 'y', 'z', 'p'};
      return c < sizeof(map) ? map[c] : '?';
    }

    char typeFromString(std::string_view s)
    {
      if (s.empty()) { return '?'; }
      const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
      return std::string_view("abcxyz").find(c) != std::string_view::npos ? c : '?';
    }

    std::uint8_t lossFromString(std::string_view s)
    {
      if (s.empty() || s == "noloss" || s == "None" || s == "none" || s == "0") { return 0; }
      if (s == "H2O" || s == "water") { return 1; }
      if (s == "NH3" || s == "ammonia") { return 2; }
      if (s == "H3PO4") { return 3; }
      if (s == "HPO3") { return 4; }
      if (s == "CO") { return 5; }
      return 6;
    }

    const char* lossName(std::uint8_t l)
    {
      static constexpr const char* names[] = {"", "-H2O", "-NH3", "-H3PO4", "-HPO3", "-CO", "-loss"};
      return l < 7 ? names[l] : "-loss";
    }

    /// Precursor assembled from rows, before fragment selection.
    struct PendingPrecursor
    {
      std::string id, modseq;
      double mz = 0, rt = 0;
      std::uint8_t charge = 0, decoy = 0;
      struct Frag { double mz; float intensity; char type; std::uint8_t ordinal, charge, loss; };
      std::vector<Frag> frags;
    };

    /// Appends finished precursors to the library, applying the load options.
    class Builder
    {
    public:
      Builder(const LibraryLoadOptions& opt, Library& lib) : opt_(opt), lib_(lib)
      {
        lib_ = Library{};
        lib_.fragment_begin.push_back(0);
      }

      void add(PendingPrecursor&& p)
      {
        if (opt_.targets_only && p.decoy) { return; }
        if (!opt_.keep_ids.empty() && !opt_.keep_ids.contains(p.id)) { return; }
        if (p.frags.empty()) { return; }
        // Descending library intensity; ties broken by m/z so the order is total and reproducible.
        std::stable_sort(p.frags.begin(), p.frags.end(), [](const auto& a, const auto& b) {
          return a.intensity != b.intensity ? a.intensity > b.intensity : a.mz < b.mz;
        });
        const std::size_t n = opt_.top_fragments ? std::min(opt_.top_fragments, p.frags.size()) : p.frags.size();
        lib_.precursor_id.push_back(std::move(p.id));
        lib_.modified_sequence.push_back(std::move(p.modseq));
        lib_.precursor_mz.push_back(p.mz);
        lib_.charge.push_back(p.charge);
        lib_.decoy.push_back(p.decoy);
        lib_.library_rt.push_back(p.rt);
        for (std::size_t k = 0; k < n; ++k)
        {
          const auto& f = p.frags[k];
          lib_.fragment_mz.push_back(f.mz);
          lib_.fragment_intensity.push_back(f.intensity);
          lib_.fragment_type.push_back(f.type);
          lib_.fragment_ordinal.push_back(f.ordinal);
          lib_.fragment_charge.push_back(f.charge);
          lib_.fragment_loss.push_back(f.loss);
        }
        lib_.fragment_begin.push_back(static_cast<std::uint32_t>(lib_.fragment_mz.size()));
      }

    private:
      const LibraryLoadOptions& opt_;
      Library& lib_;
    };

    /// Groups transition rows (TSV or flat Parquet) into precursors, in order of first appearance.
    class RowGrouper
    {
    public:
      PendingPrecursor& get(const std::string& id)
      {
        auto [it, inserted] = index_.try_emplace(id, pending_.size());
        if (inserted) { pending_.emplace_back(); pending_.back().id = id; }
        return pending_[it->second];
      }
      void flush(Builder& b)
      {
        for (auto& p : pending_) { b.add(std::move(p)); }
        pending_.clear();
        index_.clear();
      }
    private:
      std::vector<PendingPrecursor> pending_;
      std::unordered_map<std::string, std::size_t> index_;
    };

    void check(const arrow::Status& s)
    {
      if (!s.ok()) { throw std::runtime_error("Arrow: " + s.ToString()); }
    }
    template <typename T> T check(arrow::Result<T> r)
    {
      if (!r.ok()) { throw std::runtime_error("Arrow: " + r.status().ToString()); }
      return std::move(r).ValueUnsafe();
    }

    std::shared_ptr<arrow::Array> column(const arrow::RecordBatch& batch, const char* name,
                                         const std::shared_ptr<arrow::DataType>& to, bool required = true)
    {
      auto a = batch.GetColumnByName(name);
      if (!a)
      {
        if (required) { throw std::runtime_error(std::string("library: missing column ") + name); }
        return nullptr;
      }
      if (a->type()->Equals(*to)) { return a; }
      return check(arrow::compute::Cast(*a, to));
    }

    /// DIA-NN-dialect compact Parquet (one row per precursor, fragment lists), as DIALibGen writes it.
    ///
    /// Row groups are read concurrently, each by its own reader (column-parallel decoding inside), and
    /// only the columns extraction needs; each row group is cast and parsed on its thread into a partial
    /// library, and the parts are joined in row-group order (concurrent copies). The result does not
    /// depend on the thread count. ODIA_PROFILE=1 prints per-step timings.
    void loadParquetCompact(const std::string& filename, const LibraryLoadOptions& opt, Library& lib)
    {
      using clk = std::chrono::steady_clock;
      const auto t_start = clk::now();
      auto since = [](clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); };

      auto meta_file = check(arrow::io::ReadableFile::Open(filename));
      auto meta_reader = check(parquet::arrow::OpenFile(meta_file, arrow::default_memory_pool()));
      const auto& pq_schema = *meta_reader->parquet_reader()->metadata()->schema();
      const int n_rg = meta_reader->num_row_groups();
      static const std::vector<std::string> wanted = {
        "Precursor.Id", "Modified.Sequence", "Precursor.Charge", "Decoy", "RT", "Precursor.Mz", "Product.Mz",
        "Relative.Intensity", "Fragment.Type", "Fragment.Charge", "Fragment.Series.Number", "Fragment.Loss.Type"};
      std::vector<int> leaves;
      for (int c = 0; c < pq_schema.num_columns(); ++c)
      {
        const std::string top = pq_schema.Column(c)->path()->ToDotVector().front();
        if (std::find(wanted.begin(), wanted.end(), top) != wanted.end()) { leaves.push_back(c); }
      }
      meta_reader.reset();

      // Phase A: read, decode and cast the row groups concurrently.
      struct Columns
      {
        std::shared_ptr<arrow::RecordBatch> batch;   // keeps the arrays alive
        std::shared_ptr<arrow::StringArray> id, seq;
        std::shared_ptr<arrow::UInt8Array> z, dec;
        std::shared_ptr<arrow::DoubleArray> rt, pmz;
        std::shared_ptr<arrow::ListArray> qmz, inten, ft, fz, ord, loss;
      };
      const int threads = std::max(1, opt.threads);
      std::vector<Columns> cols(static_cast<std::size_t>(n_rg));
      std::exception_ptr error;
      auto t0 = clk::now();
#pragma omp parallel for schedule(dynamic, 1) num_threads(std::min(threads, std::max(1, n_rg)))
      for (int rg = 0; rg < n_rg; ++rg)
      {
        try
        {
          auto file = check(arrow::io::ReadableFile::Open(filename));
          auto reader = check(parquet::arrow::OpenFile(file, arrow::default_memory_pool()));
          // Column-parallel decoding only while row groups alone cannot occupy the threads: nesting both
          // oversubscribes (measured: 32 row groups 4.9 s with nesting).
          reader->set_use_threads(threads > 1 && n_rg < threads);
          auto table = check(reader->ReadRowGroup(rg, leaves));
          auto& c = cols[static_cast<std::size_t>(rg)];
          c.batch = check(table->CombineChunksToBatch());
          const auto utf8 = arrow::utf8(), f64 = arrow::float64(), u8 = arrow::uint8();
          const auto lf64 = arrow::list(f64), lu8 = arrow::list(u8);
          const auto& b = *c.batch;
          c.id = std::static_pointer_cast<arrow::StringArray>(column(b, "Precursor.Id", utf8));
          c.seq = std::static_pointer_cast<arrow::StringArray>(column(b, "Modified.Sequence", utf8));
          c.z = std::static_pointer_cast<arrow::UInt8Array>(column(b, "Precursor.Charge", u8));
          c.dec = std::static_pointer_cast<arrow::UInt8Array>(column(b, "Decoy", u8, false));
          c.rt = std::static_pointer_cast<arrow::DoubleArray>(column(b, "RT", f64, false));
          c.pmz = std::static_pointer_cast<arrow::DoubleArray>(column(b, "Precursor.Mz", f64));
          c.qmz = std::static_pointer_cast<arrow::ListArray>(column(b, "Product.Mz", lf64));
          c.inten = std::static_pointer_cast<arrow::ListArray>(column(b, "Relative.Intensity", lf64));
          c.ft = std::static_pointer_cast<arrow::ListArray>(column(b, "Fragment.Type", lu8));
          c.fz = std::static_pointer_cast<arrow::ListArray>(column(b, "Fragment.Charge", lu8));
          c.ord = std::static_pointer_cast<arrow::ListArray>(column(b, "Fragment.Series.Number", lu8));
          c.loss = std::static_pointer_cast<arrow::ListArray>(column(b, "Fragment.Loss.Type", lu8));
        }
        catch (...)
        {
#pragma omp critical(odia_library_error)
          if (!error) { error = std::current_exception(); }
        }
      }
      if (error) { std::rethrow_exception(error); }
      const double t_read = since(t0);

      // Phase B: parse rows in chunks on all threads (chunks never straddle a row group).
      constexpr std::int64_t chunk = 16384;
      std::vector<std::pair<int, std::int64_t>> chunks;   // (row group, first row)
      for (int rg = 0; rg < n_rg; ++rg)
      {
        for (std::int64_t r = 0; r < cols[rg].batch->num_rows(); r += chunk) { chunks.emplace_back(rg, r); }
      }
      std::vector<Library> parts(chunks.size());
      t0 = clk::now();
#pragma omp parallel for schedule(dynamic, 1) num_threads(threads)
      for (std::ptrdiff_t k = 0; k < static_cast<std::ptrdiff_t>(chunks.size()); ++k)
      {
        const auto& c = cols[static_cast<std::size_t>(chunks[k].first)];
        const auto& qv = static_cast<const arrow::DoubleArray&>(*c.qmz->values());
        const auto& iv = static_cast<const arrow::DoubleArray&>(*c.inten->values());
        const auto& tv = static_cast<const arrow::UInt8Array&>(*c.ft->values());
        const auto& zv = static_cast<const arrow::UInt8Array&>(*c.fz->values());
        const auto& ov = static_cast<const arrow::UInt8Array&>(*c.ord->values());
        const auto& lv = static_cast<const arrow::UInt8Array&>(*c.loss->values());
        Builder part(opt, parts[static_cast<std::size_t>(k)]);
        const std::int64_t r0 = chunks[k].second, r1 = std::min(c.batch->num_rows(), r0 + chunk);
        for (std::int64_t r = r0; r < r1; ++r)
        {
          PendingPrecursor p;
          p.id.assign(c.id->GetView(r));
          p.modseq.assign(c.seq->GetView(r));
          p.charge = c.z->Value(r);
          p.decoy = c.dec && c.dec->IsValid(r) ? c.dec->Value(r) : 0;
          p.rt = c.rt && c.rt->IsValid(r) ? c.rt->Value(r) : 0.0;
          p.mz = c.pmz->Value(r);
          const auto b = c.qmz->value_offset(r), e = c.qmz->value_offset(r + 1);
          p.frags.reserve(e - b);
          for (auto j = b; j < e; ++j)
          {
            // All fragment lists of a row share offsets (same writer), so index j applies to each.
            p.frags.push_back({qv.Value(j), static_cast<float>(iv.Value(j)), typeFromCode(tv.Value(j)),
                               ov.Value(j), zv.Value(j), lv.Value(j)});
          }
          part.add(std::move(p));
        }
      }
      const double t_parse = since(t0);
      cols.clear();
      t0 = clk::now();
      lib = Library::concat(parts, threads);
      if (std::getenv("ODIA_PROFILE") != nullptr)
      {
        std::fprintf(stderr, "[library profile] %d row groups, %zu columns; read+decode+cast %.2f s, parse %.2f s "
                     "(%zu chunks), join %.2f s, total %.2f s\n", n_rg, leaves.size(), t_read, t_parse, chunks.size(),
                     since(t0), since(t_start));
      }
    }

    // --- Parquet (DIA-NN dialect, DIALibGen compact or flat) -----------------------------------
    void loadParquet(const std::string& filename, const LibraryLoadOptions& opt, Library& lib)
    {
      auto infile = check(arrow::io::ReadableFile::Open(filename));
      auto reader = check(parquet::arrow::OpenFile(infile, arrow::default_memory_pool()));
      std::shared_ptr<arrow::Schema> schema;
      check(reader->GetSchema(&schema));
      auto qmz_field = schema->GetFieldByName("Product.Mz");
      if (!qmz_field) { throw std::runtime_error("library: no Product.Mz column in " + filename); }
      const bool compact = qmz_field->type()->id() == arrow::Type::LIST
                        || qmz_field->type()->id() == arrow::Type::LARGE_LIST;
      if (compact)
      {
        reader.reset();
        loadParquetCompact(filename, opt, lib);
        return;
      }
      reader->set_use_threads(opt.threads > 1);   // decode columns concurrently
      reader->set_batch_size(1 << 20);
      auto batches = check(reader->GetRecordBatchReader());

      Builder builder(opt, lib);
      RowGrouper grouper;
      const auto utf8 = arrow::utf8();
      const auto f64 = arrow::float64();
      const auto u8 = arrow::uint8();
      const bool has_rt = schema->GetFieldByName("RT") != nullptr;

      std::shared_ptr<arrow::RecordBatch> batch;
      while (true)
      {
        check(batches->ReadNext(&batch));
        if (!batch) { break; }
        auto id = std::static_pointer_cast<arrow::StringArray>(column(*batch, "Precursor.Id", utf8));
        auto seq = std::static_pointer_cast<arrow::StringArray>(column(*batch, "Modified.Sequence", utf8));
        auto z = std::static_pointer_cast<arrow::UInt8Array>(column(*batch, "Precursor.Charge", u8));
        auto dec = std::static_pointer_cast<arrow::UInt8Array>(column(*batch, "Decoy", u8, false));
        auto rt = has_rt ? std::static_pointer_cast<arrow::DoubleArray>(column(*batch, "RT", f64)) : nullptr;
        auto pmz = std::static_pointer_cast<arrow::DoubleArray>(column(*batch, "Precursor.Mz", f64));
        {
          auto qmz = std::static_pointer_cast<arrow::DoubleArray>(column(*batch, "Product.Mz", f64));
          auto inten = std::static_pointer_cast<arrow::DoubleArray>(column(*batch, "Relative.Intensity", f64));
          auto ft = column(*batch, "Fragment.Type", utf8, false);
          auto fz = std::static_pointer_cast<arrow::UInt8Array>(column(*batch, "Fragment.Charge", u8, false));
          auto ord = std::static_pointer_cast<arrow::UInt8Array>(column(*batch, "Fragment.Series.Number", u8, false));
          auto loss = column(*batch, "Fragment.Loss.Type", utf8, false);
          auto fts = std::static_pointer_cast<arrow::StringArray>(ft);
          auto ls = std::static_pointer_cast<arrow::StringArray>(loss);
          for (std::int64_t r = 0; r < batch->num_rows(); ++r)
          {
            auto& p = grouper.get(std::string(id->GetView(r)));
            if (p.frags.empty())
            {
              p.modseq = std::string(seq->GetView(r));
              p.charge = z->Value(r);
              p.decoy = dec && dec->IsValid(r) ? dec->Value(r) : 0;
              p.rt = rt && rt->IsValid(r) ? rt->Value(r) : 0.0;
              p.mz = pmz->Value(r);
            }
            p.frags.push_back({qmz->Value(r), static_cast<float>(inten->Value(r)),
                               fts ? typeFromString(fts->GetView(r)) : '?',
                               ord ? ord->Value(r) : std::uint8_t{0}, fz ? fz->Value(r) : std::uint8_t{0},
                               ls ? lossFromString(ls->GetView(r)) : std::uint8_t{0}});
          }
        }
      }
      grouper.flush(builder);
    }

    // --- TSV (DIA-NN and OpenSWATH dialects) -----------------------------------------------------
    std::vector<std::string_view> splitTabs(std::string_view line)
    {
      std::vector<std::string_view> out;
      std::size_t b = 0;
      while (true)
      {
        const auto e = line.find('\t', b);
        out.push_back(line.substr(b, e == std::string_view::npos ? std::string_view::npos : e - b));
        if (e == std::string_view::npos) { break; }
        b = e + 1;
      }
      return out;
    }

    int findColumn(const std::vector<std::string_view>& header, std::initializer_list<std::string_view> names)
    {
      for (auto n : names)
      {
        for (std::size_t i = 0; i < header.size(); ++i) { if (header[i] == n) { return static_cast<int>(i); } }
      }
      return -1;
    }

    double toDouble(std::string_view s)
    {
      double v = 0;
      auto r = std::from_chars(s.data(), s.data() + s.size(), v);
      if (r.ec != std::errc()) { throw std::runtime_error("library: not a number: '" + std::string(s) + "'"); }
      return v;
    }

    void loadTsv(const std::string& filename, const LibraryLoadOptions& opt, Library& lib)
    {
      std::ifstream in(filename);
      if (!in) { throw std::runtime_error("cannot open " + filename); }
      std::string line;
      std::getline(in, line);
      if (!line.empty() && line.back() == '\r') { line.pop_back(); }
      const std::string header_line = line;
      const auto header = splitTabs(header_line);
      const int c_id = findColumn(header, {"Precursor.Id", "TransitionGroupId", "transition_group_id", "PrecursorId"});
      const int c_seq = findColumn(header, {"Modified.Sequence", "ModifiedPeptideSequence", "ModifiedPeptide",
                                            "FullUniModPeptideName", "FullPeptideName", "PeptideSequence"});
      const int c_pmz = findColumn(header, {"Precursor.Mz", "PrecursorMz", "Q1"});
      const int c_qmz = findColumn(header, {"Product.Mz", "ProductMz", "FragmentMz", "Q3"});
      const int c_int = findColumn(header, {"Relative.Intensity", "LibraryIntensity", "RelativeIntensity",
                                            "RelativeFragmentIntensity"});
      const int c_rt = findColumn(header, {"RT", "Tr_recalibrated", "NormalizedRetentionTime", "iRT",
                                           "RetentionTime", "Tr"});
      const int c_z = findColumn(header, {"Precursor.Charge", "PrecursorCharge"});
      const int c_dec = findColumn(header, {"Decoy", "decoy", "IsDecoy"});
      const int c_ft = findColumn(header, {"Fragment.Type", "FragmentType", "FragmentIonType"});
      const int c_fz = findColumn(header, {"Fragment.Charge", "FragmentCharge", "ProductCharge"});
      const int c_ord = findColumn(header, {"Fragment.Series.Number", "FragmentSeriesNumber", "FragmentNumber"});
      const int c_loss = findColumn(header, {"Fragment.Loss.Type", "FragmentLossType"});
      if (c_pmz < 0 || c_qmz < 0 || c_int < 0 || c_z < 0 || (c_id < 0 && c_seq < 0))
      {
        throw std::runtime_error("library: TSV lacks required columns (precursor m/z, product m/z, intensity, "
                                 "charge, id or sequence): " + filename);
      }

      Builder builder(opt, lib);
      RowGrouper grouper;
      while (std::getline(in, line))
      {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        const auto f = splitTabs(line);
        auto at = [&](int c) -> std::string_view { return c >= 0 && c < static_cast<int>(f.size()) ? f[c] : std::string_view{}; };
        const auto z = static_cast<std::uint8_t>(toDouble(at(c_z)));
        std::string id = c_id >= 0 ? std::string(at(c_id)) : std::string(at(c_seq)) + std::to_string(z);
        auto& p = grouper.get(id);
        if (p.frags.empty())
        {
          p.modseq = std::string(at(c_seq));
          p.charge = z;
          const auto d = at(c_dec);
          p.decoy = (d == "1" || d == "True" || d == "TRUE" || d == "true") ? 1 : 0;
          p.rt = c_rt >= 0 && !at(c_rt).empty() ? toDouble(at(c_rt)) : 0.0;
          p.mz = toDouble(at(c_pmz));
        }
        p.frags.push_back({toDouble(at(c_qmz)), static_cast<float>(toDouble(at(c_int))),
                           typeFromString(at(c_ft)),
                           static_cast<std::uint8_t>(at(c_ord).empty() ? 0 : toDouble(at(c_ord))),
                           static_cast<std::uint8_t>(at(c_fz).empty() ? 0 : toDouble(at(c_fz))),
                           lossFromString(at(c_loss))});
      }
      grouper.flush(builder);
    }

    // --- OpenSWATH PQP / TraML via OpenMS ------------------------------------------------------------
    /// Fragment interpretation (ion type, ordinal) per transition name, from a full TraML model.
    /// OpenSwathDataAccessHelper::convertTargetedExp does not carry it into LightTransition.
    using Interpretations = std::unordered_map<std::string, std::pair<char, std::uint8_t>>;

    Interpretations traMLInterpretations(const OpenMS::TargetedExperiment& heavy)
    {
      Interpretations out;
      for (const auto& t : heavy.getTransitions())
      {
        const auto& il = t.getProduct().getInterpretationList();
        if (il.empty()) { continue; }
        char type = '?';
        switch (il.front().iontype)
        {
          case OpenMS::Residue::AIon: type = 'a'; break;
          case OpenMS::Residue::BIon: type = 'b'; break;
          case OpenMS::Residue::CIon: type = 'c'; break;
          case OpenMS::Residue::XIon: type = 'x'; break;
          case OpenMS::Residue::YIon: type = 'y'; break;
          case OpenMS::Residue::ZIon: type = 'z'; break;
          default: break;
        }
        out.emplace(t.getNativeID(), std::make_pair(type, il.front().ordinal));
      }
      return out;
    }

    void loadLightExperiment(const OpenSwath::LightTargetedExperiment& exp, const LibraryLoadOptions& opt, Library& lib,
                             const Interpretations& interp = {})
    {
      Builder builder(opt, lib);
      std::unordered_map<std::string, std::size_t> compound;
      for (std::size_t i = 0; i < exp.compounds.size(); ++i) { compound.emplace(exp.compounds[i].id, i); }
      RowGrouper grouper;
      for (const auto& t : exp.transitions)
      {
        auto& p = grouper.get(t.getPeptideRef());
        if (p.frags.empty())
        {
          const auto it = compound.find(t.getPeptideRef());
          if (it != compound.end())
          {
            const auto& c = exp.compounds[it->second];
            p.modseq = c.sequence;
            p.charge = static_cast<std::uint8_t>(std::max(0, c.getChargeState()));
            p.rt = c.rt;
          }
          p.mz = t.getPrecursorMZ();
          p.decoy = t.getDecoy() ? 1 : 0;
        }
        char type = typeFromString(t.getFragmentType());
        auto ordinal = static_cast<std::uint8_t>(std::max<int>(0, t.fragment_nr));
        if (const auto it = interp.find(t.transition_name); type == '?' && it != interp.end())
        {
          type = it->second.first;
          ordinal = it->second.second;
        }
        p.frags.push_back({t.getProductMZ(), static_cast<float>(t.getLibraryIntensity()), type, ordinal,
                           static_cast<std::uint8_t>(std::max(0, t.getProductChargeState())), 0});
      }
      grouper.flush(builder);
    }
  }

  StringColumn StringColumn::concat(const std::vector<const StringColumn*>& parts, int threads)
  {
    StringColumn out;
    std::vector<std::size_t> n0{0}, b0{0};
    for (const auto* p : parts) { n0.push_back(n0.back() + p->size()); b0.push_back(b0.back() + p->bytes()); }
    out.data_.resize(b0.back());
    out.offset_.resize(n0.back() + 1);
    out.offset_[0] = 0;
#pragma omp parallel for schedule(dynamic, 1) num_threads(std::max(1, threads))
    for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(parts.size()); ++i)
    {
      const auto& p = *parts[i];
      std::copy(p.data_.begin(), p.data_.end(), out.data_.begin() + static_cast<std::ptrdiff_t>(b0[i]));
      for (std::size_t k = 1; k < p.offset_.size(); ++k) { out.offset_[n0[i] + k] = b0[i] + p.offset_[k]; }
    }
    return out;
  }

  Library Library::concat(const std::vector<Library>& parts, int threads)
  {
    Library out;
    std::vector<std::size_t> np{0}, nf{0};
    for (const auto& p : parts) { np.push_back(np.back() + p.size()); nf.push_back(nf.back() + p.fragmentCount()); }
    std::vector<const StringColumn*> ids, seqs;
    for (const auto& p : parts) { ids.push_back(&p.precursor_id); seqs.push_back(&p.modified_sequence); }
    out.precursor_id = StringColumn::concat(ids, threads);
    out.modified_sequence = StringColumn::concat(seqs, threads);
    out.precursor_mz.resize(np.back()); out.charge.resize(np.back()); out.decoy.resize(np.back());
    out.library_rt.resize(np.back()); out.fragment_begin.resize(np.back() + 1); out.fragment_begin[0] = 0;
    out.fragment_mz.resize(nf.back()); out.fragment_intensity.resize(nf.back()); out.fragment_type.resize(nf.back());
    out.fragment_ordinal.resize(nf.back()); out.fragment_charge.resize(nf.back()); out.fragment_loss.resize(nf.back());
#pragma omp parallel for schedule(dynamic, 1) num_threads(std::max(1, threads))
    for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(parts.size()); ++i)
    {
      const auto& p = parts[i];
      const auto P = static_cast<std::ptrdiff_t>(np[i]), F = static_cast<std::ptrdiff_t>(nf[i]);
      std::copy(p.precursor_mz.begin(), p.precursor_mz.end(), out.precursor_mz.begin() + P);
      std::copy(p.charge.begin(), p.charge.end(), out.charge.begin() + P);
      std::copy(p.decoy.begin(), p.decoy.end(), out.decoy.begin() + P);
      std::copy(p.library_rt.begin(), p.library_rt.end(), out.library_rt.begin() + P);
      for (std::size_t k = 1; k < p.fragment_begin.size(); ++k)
      {
        out.fragment_begin[np[i] + k] = static_cast<std::uint32_t>(nf[i] + p.fragment_begin[k]);
      }
      std::copy(p.fragment_mz.begin(), p.fragment_mz.end(), out.fragment_mz.begin() + F);
      std::copy(p.fragment_intensity.begin(), p.fragment_intensity.end(), out.fragment_intensity.begin() + F);
      std::copy(p.fragment_type.begin(), p.fragment_type.end(), out.fragment_type.begin() + F);
      std::copy(p.fragment_ordinal.begin(), p.fragment_ordinal.end(), out.fragment_ordinal.begin() + F);
      std::copy(p.fragment_charge.begin(), p.fragment_charge.end(), out.fragment_charge.begin() + F);
      std::copy(p.fragment_loss.begin(), p.fragment_loss.end(), out.fragment_loss.begin() + F);
    }
    return out;
  }

  void Library::append(const Library& o)
  {
    const std::uint32_t base = static_cast<std::uint32_t>(fragment_mz.size());
    precursor_id.append(o.precursor_id);
    modified_sequence.append(o.modified_sequence);
    precursor_mz.insert(precursor_mz.end(), o.precursor_mz.begin(), o.precursor_mz.end());
    charge.insert(charge.end(), o.charge.begin(), o.charge.end());
    decoy.insert(decoy.end(), o.decoy.begin(), o.decoy.end());
    library_rt.insert(library_rt.end(), o.library_rt.begin(), o.library_rt.end());
    if (fragment_begin.empty()) { fragment_begin.push_back(0); }
    for (std::size_t i = 1; i < o.fragment_begin.size(); ++i) { fragment_begin.push_back(base + o.fragment_begin[i]); }
    fragment_mz.insert(fragment_mz.end(), o.fragment_mz.begin(), o.fragment_mz.end());
    fragment_intensity.insert(fragment_intensity.end(), o.fragment_intensity.begin(), o.fragment_intensity.end());
    fragment_type.insert(fragment_type.end(), o.fragment_type.begin(), o.fragment_type.end());
    fragment_ordinal.insert(fragment_ordinal.end(), o.fragment_ordinal.begin(), o.fragment_ordinal.end());
    fragment_charge.insert(fragment_charge.end(), o.fragment_charge.begin(), o.fragment_charge.end());
    fragment_loss.insert(fragment_loss.end(), o.fragment_loss.begin(), o.fragment_loss.end());
  }

  std::string Library::annotation(std::size_t f) const
  {
    if (fragment_type[f] == '?') { return "?"; }
    std::string s(1, fragment_type[f]);
    s += std::to_string(fragment_ordinal[f]);
    if (fragment_charge[f] > 1) { s += '^'; s += std::to_string(fragment_charge[f]); }
    s += lossName(fragment_loss[f]);
    return s;
  }

  void loadLibrary(const std::string& filename, const LibraryLoadOptions& opt, Library& lib)
  {
    if (endsWith(filename, ".parquet")) { loadParquet(filename, opt, lib); return; }
    if (endsWith(filename, ".tsv") || endsWith(filename, ".csv") || endsWith(filename, ".txt"))
    {
      loadTsv(filename, opt, lib);
      return;
    }
    OpenSwath::LightTargetedExperiment exp;
    Interpretations interp;
    if (endsWith(filename, ".pqp"))
    {
      OpenMS::TransitionPQPFile().convertPQPToTargetedExperiment(filename.c_str(), exp);
    }
    else if (endsWith(filename, ".traml"))
    {
      OpenMS::TargetedExperiment heavy;
      OpenMS::TraMLFile().load(filename, heavy);
      OpenMS::OpenSwathDataAccessHelper::convertTargetedExp(heavy, exp);
      interp = traMLInterpretations(heavy);
    }
    else
    {
      throw std::runtime_error("library: unsupported format: " + filename);
    }
    loadLightExperiment(exp, opt, lib, interp);
  }

  std::unordered_set<std::string> readIdList(const std::string& filename)
  {
    std::ifstream in(filename);
    if (!in) { throw std::runtime_error("cannot open " + filename); }
    std::unordered_set<std::string> ids;
    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
      if (!line.empty() && line.back() == '\r') { line.pop_back(); }
      const auto tab = line.find('\t');
      std::string id = line.substr(0, tab);
      if (first && (id == "Precursor.Id" || id == "precursor_id" || id == "TransitionGroupId")) { first = false; continue; }
      first = false;
      if (!id.empty()) { ids.insert(std::move(id)); }
    }
    return ids;
  }
}
