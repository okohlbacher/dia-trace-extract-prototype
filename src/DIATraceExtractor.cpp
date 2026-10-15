// ODIA-V3 trace extractor -- TOPP tool.
//
// Stages: library -> run grid (one global RT axis) -> parallel gather -> ordered export.
#include "ArrowPool.h"
#include "Calibration.h"
#include "Extractor.h"
#include "Library.h"
#include "Pipeline.h"
#include "Profile.h"
#include "RtMap.h"
#include "RunGrid.h"
#include "Scheduler.h"
#include "TraceWriter.h"

#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/FORMAT/DATAACCESS/MSDataWritingConsumer.h>
#include <OpenMS/CONCEPT/LogStream.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <arrow/memory_pool.h>
#include <filesystem>
#include <vector>
#include <cctype>
#include <future>
#include <optional>
#include <omp.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "Platform.h"
#include <OpenMS/CONCEPT/VersionInfo.h>
#ifdef __linux__
#include <malloc.h>
#include <sys/prctl.h>
#endif

using namespace OpenMS;

namespace
{
  std::string slopeText(double slope, double ref)
  {
    if (slope == 0) { return ""; }
    std::ostringstream o;
    o << " " << (slope >= 0 ? "+" : "") << slope * 1000 << " ppm/kTh about " << ref << " Th";
    return o.str();
  }
  double peakRssGiB() { return ODIA::platform::peakRssGiB(); }
  std::pair<double, double> rssGiB() { return ODIA::platform::rssGiB(); }

  std::string mem()
  {
    const auto [rss, hwm] = rssGiB();
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << "; memory " << rss << " GiB (peak so far " << hwm << " GiB)";
    // Page faults since the previous report (first touch of new memory: allocator churn shows here).
    static std::uint64_t last_minflt = 0;
    const std::uint64_t minflt = ODIA::platform::minorFaults();
    os << ", " << std::setprecision(2) << static_cast<double>(minflt - last_minflt) / 1e6 << " M page faults";
    last_minflt = minflt;
    return os.str();
  }

  /// Hand memory freed by a finished stage back to the OS: Arrow's pool (decoded Parquet row groups) and
  /// glibc's arenas (ingest chunks) otherwise keep it resident for the rest of the run.
  void releaseFreedMemory()
  {
    arrow::default_memory_pool()->ReleaseUnused();
    ODIA::arrowPool()->ReleaseUnused();
    ODIA::platform::trimHeap();
  }

  std::string fmt(const ODIA::StageTime& t)
  {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << t.wall << " s wall, " << t.cpu << " s CPU ("
       << std::setprecision(0) << (t.wall > 0 ? 100.0 * t.cpu / t.wall : 0.0) << "%)";
    return os.str();
  }

  std::string json(const ODIA::StageTime& t)
  {
    std::ostringstream os;
    os << "{\"wall\": " << t.wall << ", \"cpu\": " << t.cpu << "}";
    return os.str();
  }

  std::string escape(const std::string& s)
  {
    std::string o;
    for (char c : s)
    {
      if (c == '"' || c == '\\') { o += '\\'; o += c; }
      else if (c == '\n') { o += "\\n"; }
      else if (c == '\t') { o += "\\t"; }
      else if (c == '\r') { o += "\\r"; }
      else if (static_cast<unsigned char>(c) < 0x20) { o += '?'; }
      else { o += c; }
    }
    return o;
  }
}

/// Instrument class and its MS2 tolerance multiple (docs/00-dossier.md section 7) from the model name.
std::pair<std::string, double> instrumentClass(std::string name)
{
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  const auto has = [&](const char* w) { return name.find(w) != std::string::npos; };
  if (has("tims")) { return {"timsTOF", 4.0}; }
  if (has("orbitrap") || has("astral") || has("exploris") || has("exactive") || has("lumos") || has("fusion") || has("eclipse") || has("velos") || has("elite") || has("tribrid") || has("ascend")) { return {"Orbitrap", 9.0}; }
  if (has("tof")) { return {"TOF", 4.0}; }
  return {"unknown", 6.0};
}

/// The run grid as mzML: every MS1 scan and every window cell in acquisition-time order, centroided, each MS2
/// spectrum carrying its window's isolation target and offsets.
void writeRunAsMzML(const ODIA::RunGrid& grid, const std::string& source, const std::string& filename)
{
  struct Item { double rt; std::int32_t lane; std::size_t cell; };
  std::vector<Item> items;
  for (std::size_t j = 0; j < grid.ms1.rt.size(); ++j) { items.push_back({grid.ms1.rt[j], -1, j}); }
  for (std::size_t w = 0; w < grid.windows.size(); ++w)
  {
    const auto& lane = grid.windows[w];
    for (std::size_t c = 0; c < lane.acquired_rt.size(); ++c)
    {
      if (lane.acquired_rt[c] == lane.acquired_rt[c] && lane.peaks.cellEnd(c) > lane.peaks.cellBegin(c)) { items.push_back({lane.acquired_rt[c], static_cast<std::int32_t>(w), c}); }
    }
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rt < b.rt; });
  OpenMS::PlainMSDataWritingConsumer consumer(filename);
  consumer.getOptions().setCompression(true);
  consumer.setExpectedSize(items.size(), 0);
  OpenMS::ExperimentalSettings settings;
  OpenMS::SourceFile sf;
  sf.setNameOfFile(std::filesystem::path(source).filename().string());
  sf.setPathToFile(std::filesystem::path(source).parent_path().string());
  settings.getSourceFiles().push_back(sf);
  consumer.setExperimentalSettings(settings);
  std::size_t scan = 1;
  for (const Item& it : items)
  {
    const ODIA::PeakArena& arena = it.lane < 0 ? grid.ms1.peaks : grid.windows[static_cast<std::size_t>(it.lane)].peaks;
    OpenMS::MSSpectrum s;
    s.setRT(it.rt);
    s.setMSLevel(it.lane < 0 ? 1 : 2);
    s.setType(OpenMS::MSSpectrum::SpectrumType::CENTROID);
    s.setNativeID("scan=" + std::to_string(scan++));
    if (it.lane >= 0)
    {
      const auto& lane = grid.windows[static_cast<std::size_t>(it.lane)];
      OpenMS::Precursor p;
      p.setMZ(lane.target);
      p.setIsolationWindowLowerOffset(lane.target - lane.lower);
      p.setIsolationWindowUpperOffset(lane.upper - lane.target);
      s.getPrecursors().push_back(p);
    }
    const std::size_t a = arena.cellBegin(it.cell), b = arena.cellEnd(it.cell);
    s.reserve(b - a);
    for (std::size_t i = a; i < b; ++i) { s.emplace_back(ODIA::fromFixed(arena.mz[i]), arena.intensity[i]); }
    consumer.consumeSpectrum(s);
  }
  OPENMS_LOG_INFO << "mzML: " << items.size() << " spectra (" << grid.ms1.rt.size() << " MS1, " << grid.windows.size() << " windows) -> " << filename << std::endl;
}

class TOPPDIATraceExtractor : public TOPPBase
{
public:
  TOPPDIATraceExtractor()
    : TOPPBase("DIATraceExtractor",
               "Extract fragment and MS1 isotope traces (XICs) of a spectral library from a DIA run. "
               "RT map, mass offsets and tolerances are measured from the run; uses all cores (up to 64) "
               "unless -threads is given.", {}, false)
  {
    version_ = ODIA_VERSION;
    verboseVersion_ = std::string(ODIA_VERSION) + " (OpenMS " + VersionInfo::getVersion() + ")";
  }

protected:
  void registerOptionsAndFlags_() override
  {
    // Basic options first (--help); everything marked advanced (last argument true) appears with --helphelp only.
    // A plain string, not registerInputFile_: for unregistered extensions OpenMS (develop) sniffs the
    // content, takes the mzPeak ZIP container for a single-file .zip and fails; -in is checked below.
    registerStringOption_("in", "<file>", "", "DIA run: centroided mzML, mzPeak (.mzpeak) or Thermo .raw.");
    registerInputFile_("lib", "<file>", "",
                       "Spectral library: DIA-NN/DIALibGen .parquet or .tsv, OpenSWATH .tsv/.pqp/.TraML.", false);
    registerOutputFile_("out", "<file>", "", "Traces (Parquet; a directory of part files that pyarrow, polars and DuckDB "
                        "read as one table). The RT axes go to <out stem>.axes.parquet next to it.", false);
    registerOutputFile_("out_tsv", "<file>", "", "Traces as a long TSV, one row per point (large).", false);
    registerOutputFile_("out_stats", "<file>", "", "Run statistics (JSON): calibration, stage times, memory.", false);
    registerInputFile_("ids", "<file>", "", "Extract only these precursors (ids, one per line).", false);
    registerIntOption_("top_fragments", "<n>", 6, "Most intense library fragments per precursor (0 = all).", false);
    setMinInt_("top_fragments", 0);

    registerStringOption_("rt_mode", "<mode>", "auto",
                          "How library RT maps to the run: auto (fitted from the run itself; any library RT scale), "
                          "minutes (library RT is run minutes, e.g. a library tuned to this run), seconds (run seconds). "
                          "-rt_anchors or -rt_trafo select a given map instead.", false);
    setValidStrings_("rt_mode", {"auto", "minutes", "seconds"});
    registerDoubleOption_("rt_window", "<s>", 0.0, "Extract +- this many seconds around each precursor's mapped RT. "
                          "0 (default): 60 s, widened to the measured RT scatter when -rt_mode auto finds it larger; "
                          "negative: the whole run.", false);
    registerDoubleOption_("mz_tol", "<ppm>", 0.0, "Fragment tolerance, +- ppm. 0 (default): measured from the run "
                          "(-mz_sigma_ms2 x the fragment mass-error width).", false);
    registerDoubleOption_("mz_tol_ms1", "<ppm>", 0.0, "MS1 tolerance, +- ppm. 0 (default): measured from the run "
                          "(-mz_sigma_ms1 x the MS1 mass-error width).", false);
    setMinFloat_("mz_tol", 0.0); setMinFloat_("mz_tol_ms1", 0.0);
    registerStringOption_("scheme", "<name>", "A", "A (default): DIA-NN's extraction rules (max of the centroids in the window) "
                          "with the measured mass correction. B: OpenSWATH's rules (sum of the centroids, MS1 isotopes at unit "
                          "spacing, every isolation window containing the precursor, no mass correction); needs -mz_tol and "
                          "-mz_tol_ms1.", false);
    setValidStrings_("scheme", {"A", "B"});
    registerOutputFile_("out_mzml", "<file>", "", "Also write the run as a centroided mzML, one spectrum per isolation window "
                        "(diaPASEF frames split by window, scans collapsed), for tools that do not read mzPeak. Without -out "
                        "and -out_tsv only this is done and -lib is not needed.", false);
    setValidFormats_("out_mzml", {"mzML"});

    // ---- advanced
    registerOutputFile_("out_axes", "<file>", "", "RT axes (Parquet) to join with the traces; default <out stem>.axes.parquet.", false, true);
    registerOutputFile_("out_calibration", "<file>", "", "Calibration anchors (TSV), usable as -rt_anchors.", false, true);
    registerStringOption_("run_name", "<name>", "", "Run name in the output (default: the input file name without extension).", false, true);
    registerFlag_("targets_only", "Skip library decoys.", true);
    registerIntOption_("ms1_isotopes", "<n>", 2, "MS1 isotope peaks after the monoisotopic one (0: monoisotopic only; -1: no MS1 traces).", false, true);
    setMinInt_("ms1_isotopes", -1); setMaxInt_("ms1_isotopes", 200);
    registerStringOption_("ms1_axis", "<axis>", "scans", "MS1 traces on the MS1 scans (their own times, in the axes file) or "
                          "interpolated onto the MS2 cycle axis (cycles).", false, true);
    setValidStrings_("ms1_axis", {"scans", "cycles"});
    registerInputFile_("rt_anchors", "<file>", "", "Use this RT map instead: two columns, library RT and run RT (s); fitted with LOWESS.", false, true);
    registerInputFile_("rt_trafo", "<file>", "", "Use this RT map instead: trafoXML, library RT -> run RT (s).", false, true);
    setValidFormats_("rt_trafo", {"trafoXML"});
    registerFlag_("rt_trafo_invert", "The trafoXML maps run RT -> library RT (as OpenSwathRTNormalizer writes it).", true);
    registerDoubleOption_("mz_sigma_ms2", "<k>", 0.0, "Measured fragment tolerance = k x the robust width of the fragment mass "
                          "errors. 0 (default): by instrument class from the run's metadata (Orbitrap/Astral 9, TOF 4, unknown 6).", false, true);
    registerDoubleOption_("mz_sigma_ms1", "<k>", 4.0, "Measured MS1 tolerance = k x the robust width of the MS1 mass errors.", false, true);
    setMinFloat_("mz_sigma_ms2", 0.0); setMinFloat_("mz_sigma_ms1", 0.1);
    registerStringOption_("mass_calibration", "<mode>", "auto", "auto: correct the m/z by the offsets measured from the run. "
                          "off: use -mz_offset and -mz_offset_ms1 instead (needs -mz_tol and -mz_tol_ms1).", false, true);
    setValidStrings_("mass_calibration", {"auto", "off"});
    registerDoubleOption_("mz_offset", "<ppm>", 0.0, "Fragment m/z correction with -mass_calibration off (ppm added to the target).", false, true);
    registerDoubleOption_("mz_offset_ms1", "<ppm>", 0.0, "MS1 m/z correction with -mass_calibration off (ppm added to the target).", false, true);
    registerStringOption_("encoding", "<enc>", "float32", "Trace intensities: float32, or log8 (1 byte per point, log-spaced "
                          "over 6 decades below each trace's maximum).", false, true);
    setValidStrings_("encoding", {"float32", "log8"});
    registerFlag_("expand_rt", "Also write each trace's RT values in its row (larger file).", true);
    registerFlag_("skip_empty", "Do not write traces that are all zero.", true);
    registerIntOption_("out_shards", "<n>", 32, "Parquet part files written in parallel; 1 writes -out as a single file "
                       "(slower for large libraries).", false, true);
    setMinInt_("out_shards", 1);
    registerIntOption_("unit_precursors", "<n>", 256, "Precursors per parallel work unit.", false, true);
    setMinInt_("unit_precursors", 1);
    registerIntOption_("buffer_mb", "<MiB>", 512, "Memory for extracted traces waiting to be written.", false, true);
    setMinInt_("buffer_mb", 16);
    // TOPPBase::printUsage_ throws for an option whose subsection is not registered: register the subsection.
    registerTOPPSubsection_("calibration", "Calibration from the run itself");
    registerStringOption_("calibration:mass_model", "<model>", "linear", "Mass correction: linear (offset + slope x m/z, kept "
                          "only where the m/z dependence is material, else constant) or constant.", false, true);
    setValidStrings_("calibration:mass_model", {"constant", "linear"});
    registerIntOption_("calibration:panel", "<n>", 0, "Precursors sampled for calibration (0: 5% of the library).", false, true);
    setMinInt_("calibration:panel", 0);
    registerDoubleOption_("calibration:search_ppm", "<ppm>", 50.0, "First-pass mass search half width, before calibration.", false, true);
    setMinFloat_("calibration:search_ppm", 0.1);
    registerDoubleOption_("calibration:lowess_span", "<f>", 0.15, "LOWESS span of the fitted RT map.", false, true);
    setMinFloat_("calibration:lowess_span", 0.01); setMaxFloat_("calibration:lowess_span", 1.0);
  }

  ExitCodes main_(int argc, const char** argv) override
  {
    // TOPPBase's -threads defaults to 1; this tool uses all cores unless -threads (or an INI file) is given.
    for (int i = 1; i < argc; ++i)
    {
      const std::string a = argv[i];
      if (a == "-threads" || a == "--threads" || a == "-ini" || a == "--ini") { threads_given_ = true; }
    }
    // TOPPBase reports OpenMS exceptions; report everything else too instead of aborting.
    try { return run_(argc, argv); }
    catch (const OpenMS::Exception::BaseException&) { throw; }
    catch (const std::exception& e)
    {
      OPENMS_LOG_ERROR << "Error: " << e.what() << std::endl;
      return INTERNAL_ERROR;
    }
  }

  bool threads_given_ = false;

  int threadCount_()
  {
    if (threads_given_ && getIntOption_("threads") > 0) { return getIntOption_("threads"); }
    // 0 = all cores, as TOPP documents; the default (not given) likewise, capped: extraction is flat beyond ~64 threads
    return std::max(1, std::min(omp_get_num_procs(), threads_given_ ? omp_get_num_procs() : 64));
  }

  ExitCodes run_(int, const char**)
  {
    const ODIA::StageTime t_start = ODIA::stageClock();
    const std::string in = getStringOption_("in"), lib_file = getStringOption_("lib");
    if (!std::filesystem::is_regular_file(in) || !std::ifstream(in, std::ios::binary))   // portable readability test
    {
      OPENMS_LOG_ERROR << "Input file not readable: " << in << std::endl;
      return INPUT_FILE_NOT_READABLE;
    }
    const std::string out = getStringOption_("out"), out_tsv = getStringOption_("out_tsv");
    std::string out_axes = getStringOption_("out_axes");
    const std::string out_mzml = getStringOption_("out_mzml");
    if (out.empty() && out_tsv.empty() && out_mzml.empty())
    {
      OPENMS_LOG_ERROR << "Nothing to do: give -out, -out_tsv and/or -out_mzml." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    if (lib_file.empty() && (out_mzml.empty() || !out.empty() || !out_tsv.empty()))
    {
      OPENMS_LOG_ERROR << "-lib is required for an extraction (only -out_mzml works without a library)." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    if (lib_file.empty())
    {
      // export only: no library, no calibration
      // the parse is memory-bandwidth bound: flat beyond ~32 threads while its CPU time keeps growing
      ODIA::RunGrid grid = ODIA::RunGrid::load(in, std::min(threadCount_(), 32));
      OPENMS_LOG_INFO << "Run grid: " << grid.cycles() << " cycles x " << grid.windows.size() << " windows, " << grid.stats.ms1_spectra << " MS1 / "
                      << grid.stats.ms2_spectra << " MS2 spectra, " << grid.stats.ms1_peaks + grid.stats.ms2_peaks << " peaks; " << fmt(grid.stats.ingest) << mem() << std::endl;
      writeRunAsMzML(grid, in, out_mzml);
      return EXECUTION_OK;
    }
    if (out_axes.empty() && !out.empty())
    {
      std::filesystem::path p(out);
      out_axes = (p.parent_path() / (p.stem().string() + ".axes.parquet")).string();
    }
    const int threads = threadCount_();
    omp_set_num_threads(threads);
    OPENMS_LOG_INFO << "Threads: " << threads << (threads_given_ ? "" : " (all cores, up to 64; -threads sets the number)") << std::endl;

    // --- library: opened as a stream of parts (read concurrently with extraction below)
    const ODIA::StageTime t_lib0 = ODIA::stageClock();
    ODIA::LibraryLoadOptions lopt;
    lopt.top_fragments = static_cast<std::size_t>(getIntOption_("top_fragments"));
    lopt.targets_only = getFlag_("targets_only");
    lopt.threads = threads;
    if (!getStringOption_("ids").empty()) { lopt.keep_ids = ODIA::readIdList(getStringOption_("ids")); }
    ODIA::LibraryReader library(lib_file, lopt, 65536);   // rows per streamed part; fixed, so the output does not depend on -threads
    const ODIA::StageTime t_lib = ODIA::stageClock() - t_lib0;
    OPENMS_LOG_INFO << "Library: " << library.rows() << " rows in " << library.parts() << " parts from "
                    << library.sources() << " source(s), streamed during extraction; open " << fmt(t_lib) << mem() << std::endl;
    if (library.rows() == 0) { OPENMS_LOG_ERROR << "Library is empty." << std::endl; return INPUT_FILE_EMPTY; }

    // --- calibration panel (needs only the library), loaded on its own thread while the run is ingested
    // RT map: a given anchors or trafoXML file selects that map; otherwise -rt_mode
    std::string mode = getStringOption_("rt_mode");
    const std::string rt_anchors = getStringOption_("rt_anchors"), rt_trafo = getStringOption_("rt_trafo");
    if (!rt_anchors.empty() && !rt_trafo.empty()) { OPENMS_LOG_ERROR << "Give either -rt_anchors or -rt_trafo, not both." << std::endl; return ILLEGAL_PARAMETERS; }
    if (!rt_anchors.empty() || !rt_trafo.empty())
    {
      if (mode != "auto") { OPENMS_LOG_ERROR << "-rt_anchors / -rt_trafo give the RT map; they cannot be combined with -rt_mode " << mode << "." << std::endl; return ILLEGAL_PARAMETERS; }
      mode = rt_anchors.empty() ? "trafo" : "anchors";
    }
    else if (getFlag_("rt_trafo_invert")) { OPENMS_LOG_ERROR << "-rt_trafo_invert needs -rt_trafo." << std::endl; return ILLEGAL_PARAMETERS; }
    const double rt_window_opt = getDoubleOption_("rt_window");
    const bool rt_needed = rt_window_opt >= 0.0;   // a whole-run extraction needs no RT map
    const bool ms1_enabled = getIntOption_("ms1_isotopes") >= 0;
    const double mz_tol = getDoubleOption_("mz_tol"), mz_tol_ms1 = getDoubleOption_("mz_tol_ms1");
    const bool scheme_b = getStringOption_("scheme") == "B";
    const bool offsets_given = getDoubleOption_("mz_offset") != 0.0 || getDoubleOption_("mz_offset_ms1") != 0.0;
    if (scheme_b && (mz_tol <= 0 || (ms1_enabled && mz_tol_ms1 <= 0)))
    {
      OPENMS_LOG_ERROR << "-scheme B (OpenSWATH's rules) extracts without mass correction and needs fixed tolerances: give -mz_tol"
                       << (ms1_enabled ? " and -mz_tol_ms1" : "") << " (ppm)." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    if (scheme_b && offsets_given) { OPENMS_LOG_ERROR << "-scheme B extracts without mass correction; -mz_offset/-mz_offset_ms1 do not apply." << std::endl; return ILLEGAL_PARAMETERS; }
    const bool mass_auto = !scheme_b && getStringOption_("mass_calibration") == "auto";
    if (mass_auto && offsets_given) { OPENMS_LOG_ERROR << "-mz_offset/-mz_offset_ms1 replace the measured correction: give them with -mass_calibration off." << std::endl; return ILLEGAL_PARAMETERS; }
    if (scheme_b) { OPENMS_LOG_INFO << "Scheme B: sum of the centroids in the window, MS1 isotopes at unit spacing, every window containing the precursor, no mass correction." << std::endl; }
    const bool rt_auto = mode == "auto" && rt_needed;
    const int ingest_threads = std::min(threads, 32);   // the parse is memory-bandwidth bound beyond ~32 threads
    struct Panel { ODIA::Library lib; std::size_t keep_every = 1; ODIA::StageTime time; };
    std::future<Panel> panel_future;
    std::uint64_t all_rows = 0;   // the whole library (also with -ids): the calibration panel is drawn from all of it
    if (rt_auto || mass_auto)
    {
      // The panel is drawn from the whole library (never from an -ids subset: calibrating on externally
      // confirmed precursors would not be self-calibration), from evenly spaced rows so the library is
      // not loaded whole: about twice the panel size, from which calibrate() stratifies by RT.
      // Row count of the whole library: `library` is already restricted when -ids is given to a non-compact format.
      all_rows = lopt.keep_ids.empty() ? library.rows() : ODIA::LibraryReader(lib_file, ODIA::LibraryLoadOptions{}).rows();
      const auto panel_size = static_cast<std::uint64_t>(getIntOption_("calibration:panel") > 0
          ? getIntOption_("calibration:panel")
          : std::max(1.0, ODIA::CalibrationOptions{}.panel_fraction * static_cast<double>(all_rows)));
      const std::size_t keep_every = static_cast<std::size_t>(std::max<std::uint64_t>(1, all_rows / (2 * panel_size)));
      const int panel_threads = std::max(std::min(4, threads), threads - ingest_threads);
      panel_future = std::async(std::launch::async, [lib_file, keep_every, panel_threads] {
        const ODIA::StageTime t0 = ODIA::stageClock();
        omp_set_num_threads(panel_threads);
        Panel p;
        ODIA::LibraryLoadOptions popt;
        popt.top_fragments = 0;   // all fragments: the anchor search uses the top 6, the tolerance curve all of them
        popt.targets_only = true;
        popt.threads = panel_threads;
        popt.keep_every = keep_every;
        ODIA::LibraryReader(lib_file, popt).readAll(p.lib, panel_threads);   // streamed: never the whole library
        p.keep_every = keep_every;
        p.time = ODIA::stageClock() - t0;
        return p;
      });
    }

    // --- run grid
    ODIA::RunGrid grid = ODIA::RunGrid::load(in, ingest_threads);
    omp_set_num_threads(threads);
    releaseFreedMemory();
    const auto& gs = grid.stats;
    OPENMS_LOG_INFO << "Run grid: " << grid.cycles() << " cycles x " << grid.windows.size() << " windows, "
                    << gs.ms1_spectra << " MS1 / " << gs.ms2_spectra << " MS2 spectra, "
                    << gs.ms1_peaks + gs.ms2_peaks << " peaks, " << gs.missing_cells << " empty cells, "
                    << (gs.overlapping_windows ? std::to_string(gs.overlapping_windows) + " overlapping window pairs, " : std::string()) << ""
                    << grid.bytes() / double(1u << 20) << " MiB; " << fmt(gs.ingest) << mem() << std::endl;

    if (!out_mzml.empty())
    {
      writeRunAsMzML(grid, in, out_mzml);
      if (out.empty() && out_tsv.empty()) { return EXECUTION_OK; }
    }

    // --- tolerance multiples: given, or by instrument class
    double sigma_ms2 = getDoubleOption_("mz_sigma_ms2");
    {
      const auto cls = instrumentClass(grid.instrument);
      if (sigma_ms2 <= 0.0) { sigma_ms2 = cls.second; }
      OPENMS_LOG_INFO << "Instrument: " << (grid.instrument.empty() ? std::string("(none in the run's metadata)") : "'" + grid.instrument + "'")
                      << " -> " << cls.first << "; tolerances " << sigma_ms2 << " sigma (MS2), " << getDoubleOption_("mz_sigma_ms1") << " sigma (MS1)"
                      << (getDoubleOption_("mz_sigma_ms2") > 0 ? " [MS2 given]" : "") << std::endl;
      if (grid.instrument.empty() || cls.first == "unknown")
      {
        OPENMS_LOG_WARN << "Instrument class unknown: MS2 tolerance " << sigma_ms2 << " sigma; set -mz_sigma_ms2 (Orbitrap 9, TOF 4)." << std::endl;
      }
    }

    // --- calibration (from the run itself) and RT map
    std::optional<ODIA::CalibrationResult> cal;
    if (panel_future.valid())
    {
      const ODIA::StageTime t_wait0 = ODIA::stageClock();
      Panel panel = panel_future.get();
      ODIA::Library& panel_lib = panel.lib;
      OPENMS_LOG_INFO << "Calibration panel library: every " << panel.keep_every << ". row, " << panel_lib.size()
                      << " targets; " << fmt(panel.time) << " (concurrent with ingest; waited "
                      << ODIA::stageClock().wall - t_wait0.wall << " s)" << mem() << std::endl;
      const ODIA::Library* cal_lib = &panel_lib;
      ODIA::CalibrationOptions copt;
      // The absolute panel size (calibration:panel, else panel_fraction of the whole library): the panel library
      // above already holds only ~2x that many rows, so calibrate() must not apply the fraction again.
      copt.panel = static_cast<std::size_t>(getIntOption_("calibration:panel") > 0
          ? getIntOption_("calibration:panel")
          : std::max(1.0, copt.panel_fraction * static_cast<double>(all_rows)));
      copt.search_ppm = getDoubleOption_("calibration:search_ppm");
      copt.accuracy_sigma = sigma_ms2;
      copt.ms1_sigma = getDoubleOption_("mz_sigma_ms1");
      copt.linear_mass_model = getStringOption_("calibration:mass_model") == "linear";
      copt.lowess_span = getDoubleOption_("calibration:lowess_span");
      copt.threads = threads;
      try { cal = ODIA::calibrate(*cal_lib, grid, copt); }
      catch (const std::exception& e)
      {
        // Too few anchors (a small file, a library with no peptides in the run). Without a measured RT map or
        // measured tolerances there is nothing to extract with; with explicit ones, go on uncorrected.
        std::string missing;
        if (rt_auto) { missing += " the RT map (-rt_mode auto; give -rt_mode minutes|seconds, -rt_anchors or -rt_trafo)"; }
        if (mz_tol <= 0) { missing += std::string(missing.empty() ? "" : ";") + " the fragment tolerance (give -mz_tol)"; }
        if (ms1_enabled && mz_tol_ms1 <= 0) { missing += std::string(missing.empty() ? "" : ";") + " the MS1 tolerance (give -mz_tol_ms1)"; }
        if (!missing.empty()) { OPENMS_LOG_ERROR << "Calibration failed: " << e.what() << ". Needed for:" << missing << "." << std::endl; return UNEXPECTED_RESULT; }
        OPENMS_LOG_WARN << "Calibration failed: " << e.what() << "; no mass correction, the given tolerances." << std::endl;
      }
      if (cal)
      {
      releaseFreedMemory();   // the per-window calibration tiles go back before extraction
      OPENMS_LOG_INFO << "Calibration: stage 1 " << cal->stage1_anchors << " anchors (" << cal->stage1_used
                      << " used), stage 2 window +-" << cal->stage2_window_s << " s; "
                      << cal->anchors.size() << " anchors from a panel of " << cal->panel << ", "
                      << cal->used << " used; RT residual |p50/p95/p99| " << cal->rt_residual_p50 << " / "
                      << cal->rt_residual_p95 << " / " << cal->rt_residual_p99 << " s; peak FWHM " << cal->peak_fwhm_cycles << " cycles ("
                      << cal->peak_fwhm_s << " s), co-elution window +-" << cal->half_window_cycles << " cycles; score cut " << cal->score_cut
                      << " (" << cal->decoy_anchors << " decoys above it), strict cut " << cal->strict_score_cut << "; MS2 offset "
                      << cal->ms2_offset_ppm << " ppm" << slopeText(cal->ms2_slope_ppm_per_th, cal->ms2_mz_ref) << " (sigma " << cal->ms2_sigma_ppm << "), MS1 offset "
                      << cal->ms1_offset_ppm << " ppm" << slopeText(cal->ms1_slope_ppm_per_th, cal->ms1_mz_ref) << " (sigma " << cal->ms1_sigma_ppm << "); accuracy MS2 +-" << cal->ms2_accuracy_ppm
                      << " / MS1 +-" << cal->ms1_accuracy_ppm << " ppm (coverage half widths " << cal->ms2_coverage_ppm << " / " << cal->ms1_coverage_ppm << " ppm; = " << cal->accuracy_sigma << " sigma; co-elution by k:"
                      << [&] { std::string t; for (const auto& [k, r] : cal->coelution_by_sigma) { t += " " + std::to_string(static_cast<int>(k)) + ":" + (r < 0 ? std::string("-") : std::to_string(r).substr(0, 5)); } return t; }()
                      << "; from " << cal->strict_mass_anchors << " anchors; " << cal->search_rounds << " search passes, final windows +-"
                      << cal->ms2_search_ppm << " / +-" << cal->ms1_search_ppm << " ppm); " << fmt(cal->time) << std::endl;
      if (!getStringOption_("out_calibration").empty()) { ODIA::writeAnchors(getStringOption_("out_calibration"), *cal_lib, *cal); }

      }
    }
    if (!cal && !getStringOption_("out_calibration").empty()) { OPENMS_LOG_WARN << "No calibration ran: -out_calibration not written." << std::endl; }
    ODIA::RtMap rt_map;
    if (mode == "seconds" || !rt_needed) { rt_map = ODIA::RtMap::identity(); }
    else if (mode == "minutes") { rt_map = ODIA::RtMap::identity(60.0); }
    else if (mode == "trafo") { rt_map = ODIA::RtMap::fromTrafoXML(rt_trafo, getFlag_("rt_trafo_invert")); }
    else if (mode == "anchors") { rt_map = ODIA::RtMap::fromAnchors(rt_anchors, "lowess"); }
    else { rt_map = std::move(cal->rt_map); }
    OPENMS_LOG_INFO << "RT map: " << rt_map.describe() << std::endl;

    // --- extraction + export
    ODIA::ExtractOptions eopt;
    eopt.ms2_ppm = mz_tol;
    eopt.ms1_ppm = ms1_enabled ? mz_tol_ms1 : 1.0;   // no MS1 traces: the MS1 tolerance is not used
    if (eopt.ms2_ppm <= 0.0)
    {
      if (!cal) { OPENMS_LOG_ERROR << "The fragment tolerance is measured by the calibration, which is off (-mass_calibration off): give -mz_tol <ppm>." << std::endl; return ILLEGAL_PARAMETERS; }
      eopt.ms2_ppm = cal->ms2_accuracy_ppm;
      if (eopt.ms2_ppm <= 0.0) { OPENMS_LOG_ERROR << "MS2 accuracy could not be measured (too few apex centroids); give -mz_tol <ppm>." << std::endl; return UNEXPECTED_RESULT; }
      OPENMS_LOG_INFO << "MS2 tolerance: +-" << eopt.ms2_ppm << " ppm (" << cal->accuracy_sigma << " x the measured width)" << std::endl;
    }
    if (eopt.ms1_ppm <= 0.0)
    {
      if (!cal) { OPENMS_LOG_ERROR << "The MS1 tolerance is measured by the calibration, which is off (-mass_calibration off): give -mz_tol_ms1 <ppm> (or -ms1_isotopes -1 for no MS1 traces)." << std::endl; return ILLEGAL_PARAMETERS; }
      eopt.ms1_ppm = cal->ms1_accuracy_ppm;
      if (eopt.ms1_ppm <= 0.0) { OPENMS_LOG_ERROR << "MS1 accuracy could not be measured (too few MS1 centroids); give -mz_tol_ms1 <ppm>." << std::endl; return UNEXPECTED_RESULT; }
      OPENMS_LOG_INFO << "MS1 tolerance: +-" << eopt.ms1_ppm << " ppm (" << getDoubleOption_("mz_sigma_ms1") << " x the measured width)" << std::endl;
    }
    eopt.ms2_ppm_offset = mass_auto && cal ? cal->ms2_offset_ppm : getDoubleOption_("mz_offset");
    eopt.ms1_ppm_offset = mass_auto && cal ? cal->ms1_offset_ppm : getDoubleOption_("mz_offset_ms1");
    if (mass_auto && cal)
    {
      eopt.ms2_ppm_slope = cal->ms2_slope_ppm_per_th; eopt.ms2_mz_ref = cal->ms2_mz_ref;
      eopt.ms1_ppm_slope = cal->ms1_slope_ppm_per_th; eopt.ms1_mz_ref = cal->ms1_mz_ref;
    }
    eopt.rt_half_window = rt_window_opt;
    if (eopt.rt_half_window == 0.0)
    {
      // 60 s, widened when the run's own RT map scatters more: its anchors' 99th-percentile residual plus a peak width
      // (timsTOF, predicted RT: about 105 s; Astral: about 25 s, so 60 s)
      eopt.rt_half_window = 60.0;
      if (rt_auto && cal) { eopt.rt_half_window = std::max(60.0, cal->rt_residual_p99 + cal->peak_fwhm_s); }
      OPENMS_LOG_INFO << "RT window: +-" << eopt.rt_half_window << " s" << (eopt.rt_half_window > 60.0 ? " (measured RT scatter)" : "") << std::endl;
    }
    eopt.aggregate_sum = scheme_b;
    eopt.extract_ms1 = ms1_enabled;
    eopt.ms1_isotopes = std::max(0, getIntOption_("ms1_isotopes"));
    eopt.assign_all_windows = scheme_b;
    eopt.threads = threads;
    eopt.isotope_unit_spacing = scheme_b;
    eopt.ms1_scan_axis = getStringOption_("ms1_axis") == "scans";
    eopt.unit_precursors = static_cast<std::size_t>(std::max(1, getIntOption_("unit_precursors")));
    // Trace buffer pool: 256 KiB blocks, pre-faulted once and recycled (no page faults or allocator churn
    // during extraction); a trace run longer than a block goes to the heap.
    constexpr std::size_t block_floats = 64 * 1024;
    const std::size_t pool_blocks = std::max<std::size_t>(16, static_cast<std::size_t>(getIntOption_("buffer_mb")) * (1u << 20) / (block_floats * sizeof(float)));
    ODIA::BufferPool buffers(block_floats, pool_blocks);
    eopt.block_floats = block_floats;
    ODIA::Extractor extractor(rt_map, grid, eopt);

    ODIA::TraceWriter::Config wcfg;
    wcfg.parquet = out;
    wcfg.axes = out_axes;
    wcfg.tsv = out_tsv;
    wcfg.run_name = getStringOption_("run_name");
    if (wcfg.run_name.empty()) { wcfg.run_name = std::filesystem::path(in).stem().string(); }
    wcfg.log8 = getStringOption_("encoding") == "log8";
    wcfg.log8_decades = 6.0;
    wcfg.expand_rt = getFlag_("expand_rt");
    wcfg.skip_empty = getFlag_("skip_empty");
    wcfg.shards = getIntOption_("out_shards");
    wcfg.isotope_unit_spacing = eopt.isotope_unit_spacing;
    wcfg.ms1_scan_axis = eopt.ms1_scan_axis;
    wcfg.metadata = {{"input", in}, {"library", lib_file}, {"rt_map", rt_map.describe()},
                     {"mz_tol_ppm", std::to_string(eopt.ms2_ppm)}, {"mz_tol_ms1_ppm", std::to_string(eopt.ms1_ppm)},
                     {"mz_offset_ppm", std::to_string(eopt.ms2_ppm_offset)}, {"mz_offset_ms1_ppm", std::to_string(eopt.ms1_ppm_offset)},
                     {"mz_offset_slope_ppm_per_th", std::to_string(eopt.ms2_ppm_slope)}, {"mz_offset_mz_ref", std::to_string(eopt.ms2_mz_ref)},
                     {"mz_offset_ms1_slope_ppm_per_th", std::to_string(eopt.ms1_ppm_slope)}, {"mz_offset_ms1_mz_ref", std::to_string(eopt.ms1_mz_ref)},
                     {"scheme", getStringOption_("scheme")}, {"aggregation", eopt.aggregate_sum ? "sum" : "max"},
                     {"rt_window_half_s", std::to_string(eopt.rt_half_window)},
                     {"ms1_mapping", eopt.ms1_scan_axis ? "raw MS1 scans (axes lane -2)" : "linear interpolation of MS1 scans at cycle_rt"}};
    ODIA::TraceWriter writer(grid, wcfg);

    // One pool of workers, centrally scheduled (ODIA::ExtractionScheduler): read + plan library parts,
    // extract units into recycled pool buffers, write sinks; commits in a fixed order.
    std::uint64_t library_fragments = 0;
    ODIA::SchedulerOptions sopt;
    sopt.threads = threads;
    sopt.ahead = std::min<std::size_t>(4 * static_cast<std::size_t>(threads), 64);
    sopt.parts_ahead = std::max<std::size_t>(4, 2 * library.sources());
    ODIA::ExtractionScheduler scheduler(library, extractor, writer, buffers, sopt);
    scheduler.on_part = [&](const ODIA::Library& lib, const ODIA::Extractor::Plan& plan) {
      extractor.account(plan);
      library_fragments += lib.fragmentCount();
    };
    scheduler.on_unit = [&](const ODIA::TraceBlock& b) { extractor.account(b.records, b.lookups); };
    const ODIA::StageTime t_ext0 = ODIA::stageClock();
    scheduler.run();
    extractor.setTime(ODIA::stageClock() - t_ext0);
    {
      const auto& ss = scheduler.stats();
      std::ostringstream os;
      os << "Scheduler: " << threads << " workers;";
      for (int c = 0; c < ODIA::SchedulerStats::classes; ++c)
      {
        os << " " << ODIA::SchedulerStats::names[c] << " " << ss.tasks[c] << " tasks, " << ss.busy[c] << " thread-s (avg "
           << (ss.wall > 0 ? ss.busy[c] / ss.wall : 0.0) << ", max " << ss.max_threads[c] << " workers);";
      }
      os << " trace pool " << buffers.bytes() / double(1u << 20) << " MiB, peak " << buffers.peakInUse() << " of "
         << buffers.capacity() << " blocks, " << ss.heap_units << " units on the heap; library parts resident <= "
         << ss.peak_parts << ", " << ss.part_objects << " part objects";
      OPENMS_LOG_INFO << os.str() << std::endl;
    }
    const ODIA::StageTime t_close0 = ODIA::stageClock();
    writer.close();
    const ODIA::StageTime t_close = ODIA::stageClock() - t_close0;
    const ODIA::StageTime t_total = ODIA::stageClock() - t_start;

    const auto& es = extractor.stats();
    OPENMS_LOG_INFO << "Extraction: " << es.precursors << " precursors (" << library_fragments << " fragments) from "
                    << library.parts() << " library parts; " << es.assignments << " precursor-window assignments in " << es.units
                    << " units, " << es.without_window << " precursors without window, " << es.empty_range
                    << " outside the acquired RT range; " << es.traces << " traces, " << es.ms2_points << " MS2 + "
                    << es.ms1_points << " MS1 points, " << es.peak_lookups << " lookups; " << fmt(es.time)
                    << " with " << threads << " threads" << mem() << std::endl;
    // A wrong RT scale (minutes read as seconds, iRT as minutes) extracts the wrong place without failing: the mapped
    // RTs then fall outside the run or bunch into a small part of it.
    {
      const double run_span = grid.cycle_rt.empty() ? 0.0 : grid.cycle_rt.back() - grid.cycle_rt.front();
      const double lib_span = es.rt_center_max - es.rt_center_min;
      if (es.precursors > 0 && (es.empty_range * 2 > es.precursors || (es.precursors >= 1000 && rt_needed && run_span > 0 && lib_span < 0.25 * run_span)))
      {
        OPENMS_LOG_WARN << "The library's RTs map onto " << std::max(0.0, es.rt_center_min) << "-" << std::max(0.0, es.rt_center_max) << " s of a run spanning "
                        << grid.cycle_rt.front() << "-" << grid.cycle_rt.back() << " s (" << es.empty_range << " of " << es.precursors
                        << " precursors outside it): check -rt_mode (the library's RT scale); -rt_mode auto fits the map from the run." << std::endl;
      }
    }
    OPENMS_LOG_INFO << "Written: " << writer.rows() << " rows, " << writer.points() << " points in " << writer.shardCount() << " Parquet part(s) (busiest writer "
                    << writer.busySeconds() << " s, final drain " << t_close.wall << " s)" << std::endl;
    if (ODIA::prof::enabled()) { ODIA::prof::report(stderr); }
    if (std::getenv("ODIA_PROFILE") != nullptr)
    {
      auto* ap = ODIA::arrowPool();
      std::fprintf(stderr, "[memory profile] arrow pool '%s': %.2f GiB allocated now, %.2f GiB max, %.2f GiB mapped, %lld reuses",
                   ap->backend_name().c_str(), ap->bytes_allocated() / 1073741824.0, ap->max_memory() / 1073741824.0,
                   ap->mappedBytes() / 1073741824.0, static_cast<long long>(ap->reuses()));
#ifdef __linux__
      const struct mallinfo2 mi = ::mallinfo2();
      std::fprintf(stderr, "; glibc: %.2f GiB arena (%.2f in use, %.2f free), %.2f GiB mmapped", mi.arena / 1073741824.0,
                   mi.uordblks / 1073741824.0, mi.fordblks / 1073741824.0, mi.hblkhd / 1073741824.0);
#endif
      std::fprintf(stderr, "\n");
    }
    OPENMS_LOG_INFO << "Total: " << fmt(t_total) << ", peak RSS " << peakRssGiB() << " GiB" << std::endl;

    const std::string stats_file = getStringOption_("out_stats");
    if (!stats_file.empty())
    {
      std::ofstream js(stats_file);
      if (!js) { OPENMS_LOG_ERROR << "cannot write " << stats_file << std::endl; return CANNOT_WRITE_OUTPUT_FILE; }
      js << std::setprecision(10) << "{\n"
         << "  \"input\": \"" << escape(in) << "\",\n"
         << "  \"library\": \"" << escape(lib_file) << "\",\n"
         << "  \"rt_map\": \"" << escape(rt_map.describe()) << "\",\n"
         << "  \"threads\": " << threads << ",\n"
         << "  \"library_rows\": " << library.rows() << ",\n"
         << "  \"library_parts\": " << library.parts() << ",\n"
         << "  \"library_precursors\": " << es.precursors << ",\n"
         << "  \"library_fragments\": " << library_fragments << ",\n"
         << "  \"cycles\": " << grid.cycles() << ",\n"
         << "  \"windows\": " << grid.windows.size() << ",\n"
         << "  \"ms1_spectra\": " << gs.ms1_spectra << ",\n"
         << "  \"ms2_spectra\": " << gs.ms2_spectra << ",\n"
         << "  \"peaks\": " << gs.ms1_peaks + gs.ms2_peaks << ",\n"
         << "  \"empty_cells\": " << gs.missing_cells << ",\n"
         << "  \"overlapping_windows\": " << gs.overlapping_windows << ",\n"
         << "  \"grid_bytes\": " << grid.bytes() << ",\n"
         << "  \"assignments\": " << es.assignments << ",\n"
         << "  \"precursors_without_window\": " << es.without_window << ",\n"
         << "  \"outside_rt_range\": " << es.empty_range << ",\n"
         << "  \"traces\": " << es.traces << ",\n"
         << "  \"ms2_points\": " << es.ms2_points << ",\n"
         << "  \"ms1_points\": " << es.ms1_points << ",\n"
         << "  \"rows_written\": " << writer.rows() << ",\n"
         << "  \"stage_library\": " << json(t_lib) << ",\n"
         << "  \"stage_ingest\": " << json(gs.ingest) << ",\n"
         << "  \"stage_extract\": " << json(es.time) << ",\n"
         << "  \"stage_final_drain\": " << json(t_close) << ",\n"
         << "  \"writer_busy_wall\": " << writer.busySeconds() << ",\n"
         << "  \"total\": " << json(t_total) << ",\n"
         << "  \"ms2_offset_ppm\": " << eopt.ms2_ppm_offset << ",\n"
         << "  \"ms1_offset_ppm\": " << eopt.ms1_ppm_offset << ",\n"
         << "  \"ms2_offset_slope_ppm_per_th\": " << eopt.ms2_ppm_slope << ", \"ms2_offset_mz_ref\": " << eopt.ms2_mz_ref << ",\n"
         << "  \"ms1_offset_slope_ppm_per_th\": " << eopt.ms1_ppm_slope << ", \"ms1_offset_mz_ref\": " << eopt.ms1_mz_ref << ",\n"
         << "  \"mz_tol_ppm\": " << eopt.ms2_ppm << ",\n"
         << "  \"mz_tol_ms1_ppm\": " << eopt.ms1_ppm << ",\n";
      if (cal)
      {
        js << "  \"calibration\": {\"panel\": " << cal->panel << ", \"anchors\": " << cal->anchors.size()
           << ", \"used\": " << cal->used << ", \"rt_residual_p50\": " << cal->rt_residual_p50
           << ", \"rt_residual_p95\": " << cal->rt_residual_p95 << ", \"rt_residual_p99\": " << cal->rt_residual_p99
           << ", \"ms2_offset_ppm\": " << cal->ms2_offset_ppm << ", \"ms2_sigma_ppm\": " << cal->ms2_sigma_ppm
           << ", \"ms1_offset_ppm\": " << cal->ms1_offset_ppm << ", \"ms1_sigma_ppm\": " << cal->ms1_sigma_ppm
           << ", \"ms2_accuracy_ppm\": " << cal->ms2_accuracy_ppm << ", \"ms1_accuracy_ppm\": " << cal->ms1_accuracy_ppm
           << ", \"strict_mass_anchors\": " << cal->strict_mass_anchors
           << ", \"ms2_search_ppm\": " << cal->ms2_search_ppm << ", \"ms1_search_ppm\": " << cal->ms1_search_ppm
           << ", \"search_rounds\": " << cal->search_rounds
           << ", \"time\": " << json(cal->time) << "},\n";
      }
      js << "  \"peak_rss_gib\": " << peakRssGiB() << "\n"
         << "}\n";
    }
    return EXECUTION_OK;
  }
};

int main(int argc, const char** argv)
{
  // ODIA_ALLOW_PTRACE=1: let a debugger or stack sampler that is not our parent attach (Yama ptrace_scope 1),
  // for profiling (gdb stack samples); off by default.
#ifdef __linux__
  if (std::getenv("ODIA_ALLOW_PTRACE") != nullptr) { ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0); }
#endif
  // Idle OpenMP threads should sleep, not spin (spinning burned ~70% extra CPU while workers waited on
  // I/O). libgomp reads OMP_WAIT_POLICY only when it is loaded, so set it and re-execute once; an
  // explicit user setting is respected.
  if (std::getenv("OMP_WAIT_POLICY") == nullptr) { ODIA::platform::reexecWithEnv("OMP_WAIT_POLICY", "PASSIVE", argc, argv); }
  TOPPDIATraceExtractor tool;
  return tool.main(argc, argv);
}
