// ODIA-V3 trace extractor -- compact grid representation of a DIA run on ONE global RT axis.
#include "RunGrid.h"
#include "MzMLFast.h"
#include "Profile.h"

#include "Pipeline.h"

#include <OpenMS/FORMAT/DATAACCESS/MSDataTransformingConsumer.h>
#include <OpenMS/FORMAT/FileHandler.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include "mzpeak/MzPeakFile.h"

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <functional>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>

#include <omp.h>

namespace ODIA
{
  StageTime stageClock()
  {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    const double cpu = u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return {wall, cpu};
  }
  StageTime operator-(const StageTime& a, const StageTime& b) { return {a.wall - b.wall, a.cpu - b.cpu}; }

  namespace
  {
    bool hasExtension(const std::string& f, const std::string& ext)
    {
      if (f.size() < ext.size()) { return false; }
      std::string e = f.substr(f.size() - ext.size());
      std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
      return e == ext;
    }

    bool isRaw(const std::string& f)
    {
      if (f.size() < 4) { return false; }
      std::string ext = f.substr(f.size() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
      return ext == ".raw";
    }

    /// A decoded spectrum on its way from the parser to the router.
    using Decoded = GridSpectrum;

    Decoded decode(OpenMS::MSSpectrum& s, std::uint32_t index)
    {
      Decoded d;
      d.index = index;
      d.rt = s.getRT();
      d.ms_level = static_cast<int>(s.getMSLevel());
      if (d.ms_level >= 2 && !s.getPrecursors().empty())
      {
        const auto& p = s.getPrecursors()[0];
        d.target = p.getMZ();
        d.lower = p.getMZ() - p.getIsolationWindowLowerOffset();
        d.upper = p.getMZ() + p.getIsolationWindowUpperOffset();
      }
      if (!s.isSorted()) { s.sortByPosition(); d.was_unsorted = true; }
      d.n = static_cast<std::uint32_t>(s.size());
      d.mz.resize(s.size());
      d.intensity.resize(s.size());
      for (std::size_t i = 0; i < s.size(); ++i)
      {
        d.mz[i] = toFixed(s[i].getMZ());
        d.intensity[i] = s[i].getIntensity();
      }
      return d;
    }

    /// Routes decoded spectra onto the grid. Single owner: runs on one thread.
    class Router
    {
    public:
      explicit Router(RunGrid& g) : g_(g) {}

      void route(Decoded&& d)
      {
        ++g_.stats.spectra;
        if (d.was_unsorted) { ++g_.stats.unsorted_spectra; }
        if (!d.in_pool && d.n > 0 && (d.ms_level == 1 || d.ms_level == 2))
        {
          // Parsers without a pool path (OpenMS fallback, .raw): copy the peaks into the pool here.
          d.pool_begin = g_.pool->claim(d.n);
          std::copy(d.mz.begin(), d.mz.end(), g_.pool->mz() + d.pool_begin);
          std::copy(d.intensity.begin(), d.intensity.end(), g_.pool->intensity() + d.pool_begin);
          std::vector<MzFixed>().swap(d.mz);
          std::vector<float>().swap(d.intensity);
        }
        if (d.ms_level == 1)
        {
          ++g_.stats.ms1_spectra;
          g_.stats.ms1_peaks += d.n;
          g_.ms1.peaks.push(d.n ? d.pool_begin : 0, d.n);
          g_.ms1.rt.push_back(d.rt);
          g_.ms1.spectrum.push_back(d.index);
          return;
        }
        if (d.ms_level != 2) { ++g_.stats.other_spectra; return; }
        ++g_.stats.ms2_spectra;
        g_.stats.ms2_peaks += d.n;

        const std::size_t w = windowId_(d);
        if (w < seen_.size() && seen_[w])
        {
          // The window repeats: the cycle is complete.
          closeCycle_();
        }
        if (seen_.size() <= w) { seen_.resize(w + 1, false); }
        seen_[w] = true;
        cycle_rt_sum_ += d.rt;
        ++cycle_n_;
        pending_.push_back({w, d.index, d.rt, d.n ? d.pool_begin : 0, d.n});
      }

      void finish()
      {
        if (cycle_n_ > 0) { closeCycle_(); }
        // Order windows by isolation target; pad every lane to the full cycle count.
        std::vector<std::size_t> order(lanes_.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
          return lanes_[a].target < lanes_[b].target;
        });
        const std::size_t n = g_.cycle_rt.size();
        for (auto i : order)
        {
          auto& lane = lanes_[i];
          padTo_(lane, n);
          g_.windows.push_back(std::move(lane));
        }
      }

    private:
      struct Pending
      {
        std::size_t window;
        std::uint32_t index;
        double rt;
        std::uint64_t begin;   // the spectrum's peaks in the pool
        std::uint32_t n;
      };

      std::size_t windowId_(const Decoded& d)
      {
        // Same window: isolation target within 1e-6 Th and same edges (OpenMS SwathFileConsumer rule).
        auto it = ids_.lower_bound(d.target - 1e-6);
        for (; it != ids_.end() && it->first <= d.target + 1e-6; ++it)
        {
          const auto& l = lanes_[it->second];
          if (std::fabs(l.lower - d.lower) < 1e-6 && std::fabs(l.upper - d.upper) < 1e-6) { return it->second; }
        }
        WindowLane lane;
        lane.target = d.target;
        lane.lower = d.lower;
        lane.upper = d.upper;
        lanes_.push_back(std::move(lane));
        ids_.emplace(d.target, lanes_.size() - 1);
        return lanes_.size() - 1;
      }

      void padTo_(WindowLane& lane, std::size_t cycles)
      {
        while (lane.acquired_rt.size() < cycles)
        {
          lane.peaks.push(0, 0);
          lane.acquired_rt.push_back(std::numeric_limits<double>::quiet_NaN());
          lane.spectrum.push_back(std::numeric_limits<std::uint32_t>::max());
          ++g_.stats.missing_cells;
        }
      }

      void closeCycle_()
      {
        const std::size_t c = g_.cycle_rt.size();
        g_.cycle_rt.push_back(cycle_rt_sum_ / static_cast<double>(cycle_n_));
        for (auto& p : pending_)
        {
          auto& lane = lanes_[p.window];
          padTo_(lane, c); // cycles this window skipped
          if (lane.acquired_rt.size() == c + 1)
          {
            // A second spectrum of the window inside one cycle cannot happen by construction
            // (a repeat closes the cycle); kept as a guard.
            ++g_.stats.duplicate_cells;
            continue;
          }
          lane.peaks.push(p.begin, p.n);   // the peaks stay where the parser wrote them
          lane.acquired_rt.push_back(p.rt);
          lane.spectrum.push_back(p.index);
        }
        pending_.clear();
        std::fill(seen_.begin(), seen_.end(), false);
        cycle_rt_sum_ = 0;
        cycle_n_ = 0;
      }

      RunGrid& g_;
      std::vector<WindowLane> lanes_;
      std::multimap<double, std::size_t> ids_;
      std::vector<bool> seen_;
      std::vector<Pending> pending_;
      double cycle_rt_sum_ = 0;
      std::size_t cycle_n_ = 0;
    };

    struct StopParsing {};

    /// Parallel mzML ingest, adapted from OpenMS ProSEAlgorithm.cpp (loadMzMLChunked, BSD-3):
    /// the spectra are cut into chunks at their <spectrum> start tags; every chunk is parsed together
    /// with the file's header and closing tags by an MzMLFile of its own (MzMLFile::loadBuffer), so
    /// the standard handler reads all metadata and encodings. Unlike ProSE, chunks are not collected
    /// into one experiment: each worker converts its chunk to Decoded spectra, and the chunks are routed
    /// strictly in file order through the ordered pipeline (bounded look-ahead => bounded memory).
    /// Returns false (caller falls back to the streaming parser) for compressed files, unexpected
    /// layout (a comment/DOCTYPE inside the run, not exactly one spectrum list, a count attribute that
    /// does not match the spectra found, very few spectra) or a chunk that fails to parse.
    bool loadChunked(const std::string& filename, int threads, PeakPool& pool, const std::function<void(Decoded&&)>& route)
    {
      using Size = std::size_t;
      std::ifstream in(filename, std::ios::binary);
      if (!in) { return false; }
      in.seekg(0, std::ios::end);
      const Size size = static_cast<Size>(in.tellg());
      char magic[2] = {0, 0};
      in.seekg(0);
      in.read(magic, 2);
      if (!in || (magic[0] == 'B' && magic[1] == 'Z') || (magic[0] == '\x1f' && magic[1] == '\x8b') || (magic[0] == 'P' && magic[1] == 'K'))
      {
        return false;
      }

      const std::uint64_t t_marks = prof::nowNs();
      // 1. Markup positions, found in parallel over 1 MiB blocks.
      enum class Mark : unsigned char { spectrum, list_start, list_end, run_end, declaration };
      const auto is_tag = [](std::string_view text, std::string_view name) {
        return text.size() > name.size() && text.substr(0, name.size()) == name
               && std::string_view(" \t\r\n>/").find(text[name.size()]) != std::string_view::npos;
      };
      constexpr Size block = Size(1) << 20, lookahead = 16;
      const Size n_blocks = (size + block - 1) / block;
      std::vector<std::vector<std::pair<Size, Mark>>> marks(n_blocks);
      std::atomic<bool> ok{true};
#pragma omp parallel num_threads(threads)
      {
        std::ifstream file;
        std::string buffer;
#pragma omp for schedule(dynamic, 1)
        for (std::ptrdiff_t sb = 0; sb < static_cast<std::ptrdiff_t>(n_blocks); ++sb)
        {
          bool read = false;
          try
          {
            if (!file.is_open()) { file.open(filename, std::ios::binary); }
            const Size b = static_cast<Size>(sb), from = b * block, end = std::min(block, size - from);
            buffer.resize(std::min(block + lookahead, size - from));
            file.seekg(static_cast<std::streamoff>(from));
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            read = static_cast<bool>(file);
            for (Size i = 0; read && i < end; ++i)
            {
              const char* q = static_cast<const char*>(std::memchr(buffer.data() + i, '<', end - i));
              if (q == nullptr) { break; }
              i = static_cast<Size>(q - buffer.data());
              const std::string_view text(q, std::min(lookahead, buffer.size() - i));
              if (is_tag(text, "<spectrumList")) { marks[b].emplace_back(from + i, Mark::list_start); }
              else if (is_tag(text, "<spectrum")) { marks[b].emplace_back(from + i, Mark::spectrum); }
              else if (is_tag(text, "</spectrumList")) { marks[b].emplace_back(from + i, Mark::list_end); }
              else if (is_tag(text, "</run")) { marks[b].emplace_back(from + i, Mark::run_end); }
              else if (text.substr(0, 2) == "<!") { marks[b].emplace_back(from + i, Mark::declaration); }
            }
          }
          catch (...) { read = false; }
          if (!read) { ok = false; file.clear(); }
        }
      }
      constexpr Size none = std::numeric_limits<Size>::max();
      Size list_start = none, list_end = none, run_end = none;
      std::vector<Size> starts;
      bool good = ok;
      for (const auto& bm : marks)
      {
        for (const auto& [pos, mark] : bm)
        {
          switch (mark)
          {
            case Mark::list_start: good = good && list_start == none; list_start = pos; break;
            case Mark::spectrum: good = good && list_start != none && list_end == none; starts.push_back(pos); break;
            case Mark::list_end: good = good && list_start != none && list_end == none; list_end = pos; break;
            case Mark::run_end: if (run_end == none) { good = good && list_end != none; run_end = pos; } break;
            case Mark::declaration: good = good && run_end != none; break;
          }
        }
      }
      marks = {};
      if (prof::enabled()) { prof::counter("ingest.1_mark_scan(wall)").add(prof::nowNs() - t_marks); }
      if (!good || run_end == none || starts.size() < 64) { return false; }

      // 2. Header (everything before the first spectrum) with the spectrumList count made per chunk.
      std::string header(starts[0], '\0');
      in.seekg(0);
      in.read(header.data(), static_cast<std::streamsize>(header.size()));
      if (!in) { return false; }
      Size count_begin = none, count_end = none;
      for (Size q = list_start + std::string_view("<spectrumList").size();;)
      {
        q = header.find_first_not_of(" \t\r\n", q);
        if (q == std::string::npos) { return false; }
        if (header[q] == '>') { break; }
        const Size name_end = header.find_first_of("= \t\r\n>", q);
        Size v = header.find_first_not_of(" \t\r\n", name_end);
        if (name_end == std::string::npos || v == std::string::npos || header[v] != '=') { return false; }
        v = header.find_first_not_of(" \t\r\n", v + 1);
        if (v == std::string::npos || (header[v] != '"' && header[v] != '\'')) { return false; }
        const Size value_end = header.find(header[v], v + 1);
        if (value_end == std::string::npos) { return false; }
        if (std::string_view(header).substr(q, name_end - q) == "count") { count_begin = v + 1; count_end = value_end; }
        q = value_end + 1;
      }
      if (count_begin == none || header.compare(count_begin, count_end - count_begin, std::to_string(starts.size())) != 0)
      {
        return false;
      }
      const std::string end_of_run = std::string("</run></mzML>") + (header.find("<indexedmzML") != std::string::npos ? "</indexedmzML>" : "");

      // 3. Chunks of at most 4 MiB (and at least 2 per thread, 16 spectra).
      constexpr Size max_chunk_bytes = Size(4) << 20;
      const Size n = starts.size(), bytes = list_end - starts[0];
      const Size n_chunks = std::min(n / 16, std::max(2 * static_cast<Size>(threads), (bytes + max_chunk_bytes - 1) / max_chunk_bytes));
      std::vector<Size> first{0};
      for (Size c = 1; c < n_chunks; ++c)
      {
        const Size f = static_cast<Size>(std::lower_bound(starts.begin(), starts.end(), starts[0] + bytes / n_chunks * c) - starts.begin());
        if (f > first.back() && f < n) { first.push_back(f); }
      }
      first.push_back(n);
      const Size used = first.size() - 1;

      // 4. Parse chunks in parallel, route them in file order. ODIA_MZML_PARSER=openms forces the OpenMS parser.
      std::vector<std::ifstream> files(static_cast<Size>(std::max(1, threads)));
      const char* parser_env = std::getenv("ODIA_MZML_PARSER");
      const bool fast = parser_env == nullptr || std::string_view(parser_env) != "openms";
      std::atomic<Size> fast_chunks{0};
      orderedParallel(
        // Look-ahead bounded in chunks, not threads: each parked chunk holds its decoded spectra.
        used, threads, std::min<Size>(2 * static_cast<Size>(threads), 32),
        [&](Size c) {
          auto& file = files[static_cast<Size>(omp_get_thread_num())];
          if (!file.is_open()) { file.open(filename, std::ios::binary); }
          const bool last = c + 1 == used;
          const Size from = starts[first[c]], to = last ? list_end : starts[first[c + 1]];
          // Fast path: the spectra parsed straight from the file bytes (MzMLFast), into reused buffers.
          thread_local std::string raw;
          thread_local std::vector<unsigned char> s1, s2;
          {
            static prof::Counter& c_read = prof::counter("ingest.chunk_read");
            prof::Scope sc(c_read);
            raw.resize(to - from);
            file.seekg(static_cast<std::streamoff>(from));
            file.read(raw.data(), static_cast<std::streamsize>(to - from));
          }
          if (!file) { throw std::runtime_error("mzML chunk read failed"); }
          static prof::Counter& c_parse = prof::counter("ingest.chunk_parse_fast");
          prof::Scope sc_parse(c_parse);
          if (fast)
          {
            // 1. headers: metadata and peak counts of the chunk's spectra; 2. one pool range for the chunk;
            // 3. decode every spectrum straight into its slot (written once, by this thread).
            std::vector<Decoded> out(first[c + 1] - first[c]);
            thread_local std::vector<SpectrumArrays> arrays;
            arrays.assign(out.size(), SpectrumArrays{});
            bool ok_fast = true;
            std::uint64_t total = 0;
            for (Size k = first[c]; k < first[c + 1] && ok_fast; ++k)
            {
              const Size b = starts[k] - from, e = (k + 1 < first[c + 1] ? starts[k + 1] : to) - from;
              Decoded& d = out[k - first[c]];
              d.index = static_cast<std::uint32_t>(k);
              ok_fast = parseSpectrumHeader(std::string_view(raw).substr(b, e - b), d, arrays[k - first[c]]);
              total += d.n;
            }
            if (ok_fast)
            {
              std::uint64_t at = pool.claim(total);
              for (std::size_t i = 0; i < out.size() && ok_fast; ++i)
              {
                Decoded& d = out[i];
                d.pool_begin = at;
                d.in_pool = true;
                ok_fast = decodeSpectrumArrays(arrays[i], d.n, pool.mz() + at, pool.intensity() + at, d.was_unsorted, s1, s2);
                at += d.n;
              }
              // (on failure the claimed range stays unused and the chunk takes the OpenMS path below)
            }
            if (ok_fast) { fast_chunks.fetch_add(1, std::memory_order_relaxed); return out; }
          }
          // Fallback: the chunk with the file's header and closing tags, parsed by OpenMS (all encodings).
          std::string chunk(header, 0, count_begin);
          chunk += std::to_string(first[c + 1] - first[c]);
          chunk.append(header, count_end, std::string::npos);
          chunk += raw;
          chunk += "</spectrumList>" + end_of_run;
          OpenMS::MzMLFile mzml;
          mzml.getOptions().setSkipXMLChecks(true);
          OpenMS::PeakMap part;
          mzml.loadBuffer(chunk, part);
          if (part.size() != first[c + 1] - first[c]) { throw std::runtime_error("mzML chunk spectrum count mismatch"); }
          std::string().swap(chunk);
          std::vector<Decoded> out;
          out.reserve(part.size());
          for (Size k = 0; k < part.size(); ++k)
          {
            out.push_back(decode(part[k], static_cast<std::uint32_t>(first[c] + k)));
            part[k].clear(true);
          }
          return out;
        },
        [&](std::vector<Decoded>&& v) { for (auto& d : v) { route(std::move(d)); } }, "ingest.3_chunks");
      if (std::getenv("ODIA_PROFILE") != nullptr)
      {
        std::fprintf(stderr, "[ingest] %zu of %zu chunks parsed by the fast mzML parser\n", fast_chunks.load(), used);
      }
      return true;
    }
  }

  PeakPool::PeakPool(std::uint64_t capacity_peaks) : cap_(std::max<std::uint64_t>(capacity_peaks, 1))
  {
    // Virtual reservations only: pages are backed when first written (by the thread that decodes into them).
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void* m = ::mmap(nullptr, cap_ * sizeof(MzFixed), PROT_READ | PROT_WRITE, flags, -1, 0);
    void* i = ::mmap(nullptr, cap_ * sizeof(float), PROT_READ | PROT_WRITE, flags, -1, 0);
    if (m == MAP_FAILED || i == MAP_FAILED)
    {
      if (m != MAP_FAILED) { ::munmap(m, cap_ * sizeof(MzFixed)); }
      if (i != MAP_FAILED) { ::munmap(i, cap_ * sizeof(float)); }
      throw std::runtime_error("cannot reserve the peak pool (" + std::to_string(cap_) + " peaks)");
    }
    mz_ = static_cast<MzFixed*>(m);
    in_ = static_cast<float*>(i);
  }

  PeakPool::~PeakPool()
  {
    if (mz_) { ::munmap(mz_, cap_ * sizeof(MzFixed)); }
    if (in_) { ::munmap(in_, cap_ * sizeof(float)); }
  }

  std::uint64_t PeakPool::claim(std::uint64_t n)
  {
    const std::uint64_t at = used_.fetch_add(n, std::memory_order_relaxed);
    if (at + n > cap_)
    {
      throw std::runtime_error("peak pool exhausted (" + std::to_string(cap_) + " peaks reserved); input denser than estimated");
    }
    return at;
  }

  void PeakArena::buildIndex(std::uint32_t target_buckets)
  {
    buckets = 0;
    bucket_start.clear();
    if (target_buckets == 0 || cells() == 0 || mz == nullptr) { return; }
    MzFixed lo = std::numeric_limits<MzFixed>::max(), hi = 0;
    for (std::size_t c = 0; c < cells(); ++c)
    {
      if (count[c] >= 65535) { return; }
      if (count[c] == 0) { continue; }
      lo = std::min(lo, mz[cellBegin(c)]);
      hi = std::max(hi, mz[cellEnd(c) - 1]);
    }
    if (hi < lo) { return; }
    bucket_base = lo;
    const std::uint64_t range = static_cast<std::uint64_t>(hi) - bucket_base + 1;
    bucket_shift = 0;
    while ((range >> bucket_shift) > target_buckets) { ++bucket_shift; }
    buckets = static_cast<std::uint32_t>(((range - 1) >> bucket_shift) + 1);
    bucket_start.assign(cells() * buckets, 0);
    for (std::size_t c = 0; c < cells(); ++c)
    {
      const std::size_t a = cellBegin(c), b = cellEnd(c);
      std::uint16_t* row = bucket_start.data() + c * buckets;
      std::size_t i = a;
      for (std::uint32_t k = 0; k < buckets; ++k)
      {
        // first peak whose bucket >= k (peaks are sorted by m/z)
        while (i < b && ((mz[i] - bucket_base) >> bucket_shift) < k) { ++i; }
        row[k] = static_cast<std::uint16_t>(i - a);
      }
    }
  }

  std::size_t RunGrid::bytes() const
  {
    auto arena = [](const PeakArena& a) {
      return a.begin.capacity() * 8 + a.count.capacity() * 4 + a.bucket_start.capacity() * 2;
    };
    std::size_t b = (pool ? pool->size() * (sizeof(MzFixed) + sizeof(float)) : 0) + arena(ms1.peaks) + ms1.rt.capacity() * 8 + ms1.spectrum.capacity() * 4 + cycle_rt.capacity() * 8;
    for (const auto& w : windows) { b += arena(w.peaks) + w.acquired_rt.capacity() * 8 + w.spectrum.capacity() * 4; }
    return b;
  }

  std::size_t RunGrid::lowerCycle(double t) const
  {
    return static_cast<std::size_t>(std::lower_bound(cycle_rt.begin(), cycle_rt.end(), t) - cycle_rt.begin());
  }

  std::size_t RunGrid::upperCycle(double t) const
  {
    return static_cast<std::size_t>(std::upper_bound(cycle_rt.begin(), cycle_rt.end(), t) - cycle_rt.begin());
  }

  RunGrid RunGrid::load(const std::string& filename, int decode_threads)
  {
    const StageTime t0 = stageClock();
    RunGrid g;
    {
      // Peak pool reservation (virtual): generous for the input; untouched pages cost nothing. An mzML peak
      // takes >= ~8 base64 characters (uncompressed or zlib), so file bytes / 2 leaves ample headroom.
      std::error_code ec;
      const std::uint64_t bytes = std::filesystem::file_size(filename, ec);
      const std::uint64_t est = ec ? (std::uint64_t(1) << 32) : bytes;
      g.pool = std::make_unique<PeakPool>(std::max<std::uint64_t>(est / 2, std::uint64_t(1) << 26)
                                          * (hasExtension(filename, ".mzml") ? 1 : 4));
    }
    Router router(g);

    if (isRaw(filename))
    {
      omp_set_num_threads(std::max(1, decode_threads));
      OpenMS::MSExperiment exp;
      OpenMS::FileHandler().loadExperiment(filename, exp, {OpenMS::FileTypes::RAW});
      exp.sortSpectra(false);
      std::uint32_t index = 0;
      for (auto& s : exp.getSpectra()) { router.route(decode(s, index++)); }
    }
    else if (hasExtension(filename, ".mzpeak"))
    {
      // mzPeak through the vendored OpenMS MzPeakFile reader (the only mzPeak reader we use), streamed spectrum
      // by spectrum. Each spectrum goes straight into the pool (no intermediate vectors) and is routed as it
      // arrives. The reader's transform() is serial and dominates (~85% of ingest); handing spectra to other
      // threads for conversion was measured and gained nothing, so this stays on the reader's thread.
      static prof::Counter& c_cb = prof::counter("ingest.mzpeak_callback");
      OpenMS::MzPeakFile file;
      OpenMS::MSDataTransformingConsumer consumer;
      std::uint32_t index = 0;
      std::uint64_t n_level[3] = {0, 0, 0}, n_peaks[3] = {0, 0, 0}, n_prec = 0, n_profile[3] = {0, 0, 0};
      PeakPool& pool = *g.pool;
      consumer.setSpectraProcessingFunc([&](OpenMS::MSSpectrum& s) {
        prof::Scope sc(c_cb);
        const int l = std::min<int>(2, static_cast<int>(s.getMSLevel()));
        ++n_level[l]; n_peaks[l] += s.size(); n_prec += !s.getPrecursors().empty();
        n_profile[l] += s.getType(false) == OpenMS::MSSpectrum::SpectrumType::PROFILE;
        Decoded d;
        d.index = index++;
        d.rt = s.getRT();
        d.ms_level = static_cast<int>(s.getMSLevel());
        if (d.ms_level >= 2 && !s.getPrecursors().empty())
        {
          const auto& p = s.getPrecursors()[0];
          d.target = p.getMZ();
          d.lower = p.getMZ() - p.getIsolationWindowLowerOffset();
          d.upper = p.getMZ() + p.getIsolationWindowUpperOffset();
        }
        if (!s.isSorted()) { s.sortByPosition(); d.was_unsorted = true; }
        d.n = static_cast<std::uint32_t>(s.size());
        if (d.n > 0 && (d.ms_level == 1 || d.ms_level == 2))
        {
          d.in_pool = true;
          d.pool_begin = pool.claim(d.n);
          MzFixed* mz = pool.mz() + d.pool_begin;
          float* in = pool.intensity() + d.pool_begin;
          for (std::size_t i = 0; i < s.size(); ++i) { mz[i] = toFixed(s[i].getMZ()); in[i] = s[i].getIntensity(); }
        }
        router.route(std::move(d));
      });
      file.transform(filename, &consumer, true, true);
      if (std::getenv("ODIA_PROFILE"))
      {
        std::fprintf(stderr, "[mzpeak] MS1 %llu spectra (%llu profile) %llu peaks; MS2 %llu spectra (%llu profile) %llu peaks; "
                     "with precursor %llu; other %llu\n", (unsigned long long)n_level[1], (unsigned long long)n_profile[1],
                     (unsigned long long)n_peaks[1], (unsigned long long)n_level[2], (unsigned long long)n_profile[2],
                     (unsigned long long)n_peaks[2], (unsigned long long)n_prec, (unsigned long long)n_level[0]);
      }
    }
    else if (decode_threads > 1 && loadChunked(filename, decode_threads, *g.pool, [&](Decoded&& d) { router.route(std::move(d)); }))
    {
      // parsed in parallel chunks (see loadChunked)
    }
    else
    {
      // Producer/consumer: the serial XML parse (the ingest bottleneck) runs on its own thread;
      // routing onto the grid runs on this one. A bounded queue of batches bounds memory.
      constexpr std::size_t batch_size = 1024, capacity = 4;
      std::mutex mtx;
      std::condition_variable cv;
      std::deque<std::vector<Decoded>> queue;
      bool done = false, abort = false;
      std::exception_ptr error;

      std::thread producer([&] {
        // A new thread starts with the default OpenMP thread count (all cores): bound it.
        omp_set_num_threads(std::max(1, decode_threads));
        try
        {
          OpenMS::MzMLFile file;
          file.getOptions().setSkipXMLChecks(true);
          OpenMS::MSDataTransformingConsumer consumer;
          std::vector<Decoded> batch;
          std::uint32_t index = 0;
          auto push = [&] {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return queue.size() < capacity || abort; });
            if (abort) { throw StopParsing{}; }
            queue.push_back(std::move(batch));
            batch.clear();
            cv.notify_all();
          };
          consumer.setSpectraProcessingFunc([&](OpenMS::MSSpectrum& s) {
            batch.push_back(decode(s, index++));
            if (batch.size() == batch_size) { push(); }
          });
          file.transform(filename, &consumer, true, true);
          if (!batch.empty()) { push(); }
        }
        catch (const StopParsing&) {}
        catch (...) { std::lock_guard lock(mtx); error = std::current_exception(); }
        std::lock_guard lock(mtx);
        done = true;
        cv.notify_all();
      });

      try
      {
        while (true)
        {
          std::vector<Decoded> batch;
          {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return !queue.empty() || done; });
            if (queue.empty()) { break; }
            batch = std::move(queue.front());
            queue.pop_front();
            cv.notify_all();
          }
          for (auto& d : batch) { router.route(std::move(d)); }
        }
      }
      catch (...)
      {
        { std::lock_guard lock(mtx); abort = true; cv.notify_all(); }
        producer.join();
        throw;
      }
      producer.join();
      if (error) { std::rethrow_exception(error); }
    }
    {
      prof::Scope sc(prof::counter("ingest.4_router_finish(wall)"));
      router.finish();
    }

    // Every lane reads the pool.
    for (auto& w : g.windows) { w.peaks.mz = g.pool->mz(); w.peaks.intensity = g.pool->intensity(); }
    g.ms1.peaks.mz = g.pool->mz();
    g.ms1.peaks.intensity = g.pool->intensity();
    if (g.windows.empty()) { throw std::runtime_error("no MS2 isolation windows in " + filename); }
    // m/z bucket index per lane (the extraction's merge join jumps over m/z gaps with it).
    prof::Scope sc_index(prof::counter("ingest.5_bucket_index(wall)"));
#pragma omp parallel for schedule(dynamic, 1) num_threads(std::max(1, decode_threads))
    for (std::ptrdiff_t w = -1; w < static_cast<std::ptrdiff_t>(g.windows.size()); ++w)
    {
      (w < 0 ? g.ms1.peaks : g.windows[static_cast<std::size_t>(w)].peaks).buildIndex();
    }
    g.stats.ingest = stageClock() - t0;
    return g;
  }
}
