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
#include <OpenMS/CONCEPT/LogStream.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <arrow/memory_pool.h>
#include <filesystem>
#include <malloc.h>
#include <unistd.h>
#include <vector>
#include <future>
#include <optional>
#include <omp.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/prctl.h>
#include <sys/resource.h>

using namespace OpenMS;

namespace
{
  double peakRssGiB()
  {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_maxrss) / (1024.0 * 1024.0); // KiB on Linux
  }

  /// Current and peak resident memory (VmRSS, VmHWM) in GiB.
  std::pair<double, double> rssGiB()
  {
    std::ifstream st("/proc/self/status");
    std::string line;
    double rss = 0, hwm = 0;
    while (std::getline(st, line))
    {
      if (line.rfind("VmRSS:", 0) == 0) { rss = std::stod(line.substr(6)) / (1024.0 * 1024.0); }
      if (line.rfind("VmHWM:", 0) == 0) { hwm = std::stod(line.substr(6)) / (1024.0 * 1024.0); }
    }
    return {rss, hwm};
  }

  std::string mem()
  {
    const auto [rss, hwm] = rssGiB();
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << "; memory " << rss << " GiB (peak so far " << hwm << " GiB)";
    // Page faults since the previous report (first touch of new memory: allocator churn shows here).
    static long last_minflt = 0;
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    os << ", " << std::setprecision(2) << (ru.ru_minflt - last_minflt) / 1e6 << " M page faults";
    last_minflt = ru.ru_minflt;
    return os.str();
  }

  /// Hand memory freed by a finished stage back to the OS: Arrow's pool (decoded Parquet row groups) and
  /// glibc's arenas (ingest chunks) otherwise keep it resident for the rest of the run.
  void releaseFreedMemory()
  {
    arrow::default_memory_pool()->ReleaseUnused();
    ODIA::arrowPool()->ReleaseUnused();
    ::malloc_trim(0);
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
    for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; } o += c; }
    return o;
  }
}

class TOPPDIATraceExtractor : public TOPPBase
{
public:
  TOPPDIATraceExtractor()
    : TOPPBase("DIATraceExtractor",
               "Extract fragment and MS1 isotope traces (XICs) of a spectral library from a DIA run "
               "on one global RT (cycle) axis.", {}, false)
  {
  }

protected:
  void registerOptionsAndFlags_() override
  {
    // A plain string, not registerInputFile_: for unregistered extensions OpenMS (develop) sniffs the
    // content, takes the mzPeak ZIP container for a single-file .zip and fails; -in is checked below.
    registerStringOption_("in", "<file>", "", "DIA run: centroided mzML, mzPeak (.mzpeak) or Thermo .raw.");
    registerInputFile_("lib", "<file>", "",
                       "Spectral library: DIA-NN/DIALibGen .parquet or .tsv, OpenSWATH .tsv/.pqp/.TraML.");
    registerOutputFile_("out", "<file>", "", "Traces (Parquet, one row per trace on the global cycle axis).", false);
    registerOutputFile_("out_axes", "<file>", "", "Axes (Parquet): global cycle axis, exact acquisition times, "
                        "MS1 scan times. Default with -out: <out stem>.axes.parquet.", false);
    registerOutputFile_("out_tsv", "<file>", "", "Traces as long TSV (one row per point; large).", false);
    registerOutputFile_("out_stats", "<file>", "", "Run statistics (JSON): stages, wall/CPU, memory.", false);
    registerInputFile_("ids", "<file>", "", "Restrict to these precursor ids (one per line).", false);
    registerIntOption_("top_fragments", "<n>", 6, "Fragments per precursor by library intensity (0 = all).", false);
    setMinInt_("top_fragments", 0);
    registerFlag_("targets_only", "Skip library decoys.");

    registerStringOption_("rt_mode", "<mode>", "linear",
                          "Library RT -> run seconds: identity (library in run seconds), minutes (library in run "
                          "minutes), linear (-rt_intercept/-rt_slope), trafo (-rt_trafo), anchors (-rt_anchors), "
                          "auto (calibrate from the run itself, see -calibration:*).", false);
    setValidStrings_("rt_mode", {"identity", "minutes", "linear", "trafo", "anchors", "auto"});
    registerDoubleOption_("rt_intercept", "<s>", 0.0, "Linear RT map intercept (seconds).", false);
    registerDoubleOption_("rt_slope", "<s>", 1.0, "Linear RT map slope (seconds per library unit).", false);
    registerInputFile_("rt_trafo", "<file>", "", "trafoXML mapping library RT -> run RT (seconds).", false);
    setValidFormats_("rt_trafo", {"trafoXML"});
    registerFlag_("rt_trafo_invert", "The trafoXML maps run -> library RT (as OpenSwathRTNormalizer writes); invert it.");
    registerInputFile_("rt_anchors", "<file>", "", "Two columns: library RT, observed run RT (seconds).", false);
    registerStringOption_("rt_anchor_model", "<model>", "lowess", "Model fitted to the anchors.", false);
    setValidStrings_("rt_anchor_model", {"linear", "b_spline", "lowess", "interpolated"});
    registerDoubleOption_("rt_window", "<s>", 60.0, "Half width (seconds) of the extraction window around the "
                          "mapped RT; <= 0 extracts the whole run.", false);

    registerDoubleOption_("mz_tol", "<ppm>", 0.0, "Fragment tolerance, +- ppm (half width). 0 (default) = the MS2 accuracy measured "
                          "by -mass_calibration auto (calibration:accuracy_sigma x the robust error width; about 6 ppm on Astral).", false);
    registerDoubleOption_("mz_tol_ms1", "<ppm>", 0.0, "MS1 tolerance, +- ppm (half width). 0 (default) = the MS1 accuracy measured "
                          "by -mass_calibration auto (calibration:accuracy_sigma x the robust MS1 error width; about 2.4 ppm on Astral).", false);
    registerStringOption_("mass_calibration", "<mode>", "auto", "auto (default): measure the fragment and MS1 mass offsets "
                          "from the run (calibration anchors) and apply them instead of -mz_offset*.", false);
    setValidStrings_("mass_calibration", {"off", "auto"});
    registerOutputFile_("out_calibration", "<file>", "", "Calibration anchors (TSV; usable as -rt_anchors).", false);
    registerIntOption_("calibration:panel", "<n>", 0, "Precursors sampled for calibration (stratified over library RT; "
                       "never restricted by -ids); 0 = calibration:panel_fraction of the eligible precursors.", false, true);
    registerDoubleOption_("calibration:panel_fraction", "<f>", 0.05, "Share of the eligible precursors in the panel.", false, true);
    registerDoubleOption_("calibration:search_ppm", "<ppm>", 50.0, "First-pass mass search half width (uncalibrated). Later "
                          "passes search +- calibration:search_factor x the measured accuracy around the measured offset.", false, true);
    registerDoubleOption_("calibration:search_factor", "<f>", 3.0, "Narrowing factor of the mass search (see search_ppm).", false, true);
    registerIntOption_("calibration:search_rounds", "<n>", 3, "Maximum stage-2 passes (stops when the search window "
                       "changes by less than 25%).", false, true);
    registerDoubleOption_("calibration:accuracy_sigma", "<k>", 6.0, "Measured tolerance = k x the robust width (1.4826 x MAD) "
                          "of the apex mass errors; 6 per the tolerance trade-off analysis, 5 reproduces DIA-NN's rule.", false, true);
    registerDoubleOption_("calibration:min_relative_intensity", "<f>", 0.0, "Apex centroids below this share of the "
                          "anchor's strongest fragment are left out of the mass model (0 = all).", false, true);
    registerIntOption_("calibration:strict_min_fragments", "<n>", 5, "Stage 1: fragments with signal at an anchor apex.", false, true);
    registerDoubleOption_("calibration:strict_min_correlation", "<r>", 0.9, "Stage 1: mean pairwise fragment correlation "
                          "at an anchor apex.", false, true);
    registerDoubleOption_("calibration:strict_min_cosine", "<c>", 0.85, "Stage 1: cosine of sqrt(apex) vs sqrt(library) "
                          "intensities.", false, true);
    registerIntOption_("calibration:min_fragments", "<n>", 4, "Stage 2: fragments with signal at an anchor apex.", false, true);
    registerDoubleOption_("calibration:min_correlation", "<r>", 0.8, "Stage 2: mean pairwise fragment correlation "
                          "at an anchor apex.", false, true);
    registerDoubleOption_("calibration:min_cosine", "<c>", 0.75, "Stage 2: cosine of sqrt(apex) vs sqrt(library) "
                          "intensities at an anchor apex.", false, true);
    registerIntOption_("calibration:half_window_cycles", "<n>", 4, "Co-elution window around a candidate apex (+- cycles).", false, true);
    registerIntOption_("calibration:candidates", "<n>", 5, "Local maxima of the summed fragment trace tried per precursor.", false, true);
    registerIntOption_("calibration:mode_bins", "<n>", 20, "Library-RT bins of the initial RT curve (run-RT mode per bin).", false, true);
    registerDoubleOption_("calibration:mode_bandwidth", "<s>", 15.0, "Half width (s) of the window that finds the run-RT mode "
                          "per bin; anchors farther than 4 x this from the mode curve are dropped.", false, true);
    registerDoubleOption_("calibration:min_window", "<s>", 30.0, "Smallest stage-2 RT search half width (s); the width is "
                          "max(4 sigma of the stage-1 fit, this).", false, true);
    registerDoubleOption_("calibration:lowess_span", "<f>", 0.15, "LOWESS span of the RT model.", false, true);
    registerIntOption_("calibration:rejection_rounds", "<n>", 3, "Refits of the RT model, each dropping anchors beyond "
                       "max(3 sigma, calibration:min_reject).", false, true);
    registerDoubleOption_("calibration:min_reject", "<s>", 3.0, "Smallest rejection distance (s) of the RT refits.", false, true);
    registerDoubleOption_("mz_offset", "<ppm>", 0.0, "Fragment mass correction (ppm added to the target).", false);
    registerDoubleOption_("mz_offset_ms1", "<ppm>", 0.0, "MS1 mass correction (ppm added to the target).", false);
    registerStringOption_("aggregation", "<mode>", "max", "Combine centroids within tolerance: max (default, as DIA-NN) "
                          "or sum (as OpenSWATH).", false);
    setValidStrings_("aggregation", {"sum", "max"});
    registerStringOption_("ms1_axis", "<axis>", "scans", "MS1 traces on the MS1 scans (raw values; times on axes lane -2) "
                          "or interpolated linearly onto the global cycle axis (cycles).", false);
    setValidStrings_("ms1_axis", {"scans", "cycles"});
    registerIntOption_("ms1_isotopes", "<n>", 2, "MS1 isotopes after the monoisotope (-1 disables MS1).", false);
    setMinInt_("ms1_isotopes", -1);
    registerStringOption_("ms1_isotope_spacing", "<mode>", "charge", "MS1 isotope k at mz + k*1.00336/z (charge) "
                          "or at mz + k*1.00336 regardless of charge (unit; what OpenSWATH extracts).", false);
    setValidStrings_("ms1_isotope_spacing", {"charge", "unit"});
    registerStringOption_("window_assignment", "<mode>", "nearest", "Precursor -> isolation window: nearest "
                          "(closest centre) or all (every window containing it, as OpenSWATH).", false);
    setValidStrings_("window_assignment", {"nearest", "all"});
    registerStringOption_("encoding", "<enc>", "float32", "Trace intensities: float32 (exact) or log8 (1 byte "
                          "per point, log-spaced below the per-trace maximum).", false);
    setValidStrings_("encoding", {"float32", "log8"});
    registerDoubleOption_("log8_decades", "<n>", 6.0, "Dynamic range of the log8 encoding (decades).", false, true);
    registerFlag_("expand_rt", "Also write the RT list per trace row (convenience; larger file).");
    registerFlag_("skip_empty", "Do not write traces that are all zero.");
    registerIntOption_("ingest_threads", "<n>", 0, "Threads for the mzML ingest (parallel chunk parsing; 1 = serial "
                       "streaming parser; 0 = min(-threads, 32): the parse is memory-bandwidth bound, measured flat beyond "
                       "~32 threads while CPU time keeps growing).", false, true);
    setMinInt_("ingest_threads", 0);
    registerIntOption_("out_shards", "<n>", 32, "Parquet part files written concurrently; > 1 makes -out a directory of "
                       "part-NNN.parquet files (read as one table by pyarrow/DuckDB). Each part is written serially, so the "
                       "part count caps write parallelism (8 parts capped a 7 M-precursor run at ~16 s; 32: ~7 s); row groups "
                       "shrink with the count, keeping the writers' buffered rows constant. Fixed default: the output does "
                       "not depend on -threads.", false, true);
    setMinInt_("out_shards", 1);
    registerIntOption_("unit_precursors", "<n>", 256, "Precursors per parallel work unit.", false, true);
    registerIntOption_("buffer_mb", "<MiB>", 512, "Trace buffer pool (pre-faulted, recycled): extracted traces live "
                       "here until they are written. Units that do not fit wait for writes to free blocks.", false, true);
    registerIntOption_("library_part_rows", "<n>", 65536, "Library rows per streamed part (parts are read, planned "
                       "and released while extraction runs; fixed, so the output does not depend on -threads).", false, true);
    registerStringOption_("run_name", "<name>", "", "Run name in the output (default: input file stem).", false);
  }

  ExitCodes main_(int argc, const char** argv) override
  {
    // TOPPBase reports OpenMS exceptions; report everything else too instead of aborting.
    try { return run_(argc, argv); }
    catch (const OpenMS::Exception::BaseException&) { throw; }
    catch (const std::exception& e)
    {
      OPENMS_LOG_ERROR << "Error: " << e.what() << std::endl;
      return INTERNAL_ERROR;
    }
  }

  ExitCodes run_(int, const char**)
  {
    const ODIA::StageTime t_start = ODIA::stageClock();
    const std::string in = getStringOption_("in"), lib_file = getStringOption_("lib");
    if (!std::filesystem::is_regular_file(in) || ::access(in.c_str(), R_OK) != 0)
    {
      OPENMS_LOG_ERROR << "Input file not readable: " << in << std::endl;
      return INPUT_FILE_NOT_READABLE;
    }
    const std::string out = getStringOption_("out"), out_tsv = getStringOption_("out_tsv");
    std::string out_axes = getStringOption_("out_axes");
    if (out.empty() && out_tsv.empty())
    {
      OPENMS_LOG_ERROR << "Nothing to do: give -out and/or -out_tsv." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    if (out_axes.empty() && !out.empty())
    {
      std::filesystem::path p(out);
      out_axes = (p.parent_path() / (p.stem().string() + ".axes.parquet")).string();
    }
    const int threads = std::max(1, getIntOption_("threads"));

    // --- library: opened as a stream of parts (read concurrently with extraction below)
    const ODIA::StageTime t_lib0 = ODIA::stageClock();
    ODIA::LibraryLoadOptions lopt;
    lopt.top_fragments = static_cast<std::size_t>(getIntOption_("top_fragments"));
    lopt.targets_only = getFlag_("targets_only");
    lopt.threads = threads;
    if (!getStringOption_("ids").empty()) { lopt.keep_ids = ODIA::readIdList(getStringOption_("ids")); }
    ODIA::LibraryReader library(lib_file, lopt, static_cast<std::size_t>(std::max(1, getIntOption_("library_part_rows"))));
    const ODIA::StageTime t_lib = ODIA::stageClock() - t_lib0;
    OPENMS_LOG_INFO << "Library: " << library.rows() << " rows in " << library.parts() << " parts from "
                    << library.sources() << " source(s), streamed during extraction; open " << fmt(t_lib) << mem() << std::endl;
    if (library.rows() == 0) { OPENMS_LOG_ERROR << "Library is empty." << std::endl; return INPUT_FILE_EMPTY; }

    // --- calibration panel (needs only the library), loaded on its own thread while the run is ingested
    const std::string mode = getStringOption_("rt_mode");
    const bool mass_auto = getStringOption_("mass_calibration") == "auto";
    const int ingest_threads = getIntOption_("ingest_threads") > 0 ? getIntOption_("ingest_threads") : std::min(threads, 32);
    struct Panel { ODIA::Library lib; std::size_t keep_every = 1; ODIA::StageTime time; };
    std::future<Panel> panel_future;
    if (mode == "auto" || mass_auto)
    {
      // The panel is drawn from the whole library (never from an -ids subset: calibrating on externally
      // confirmed precursors would not be self-calibration), from evenly spaced rows so the library is
      // not loaded whole: about twice the panel size, from which calibrate() stratifies by RT.
      const auto panel_size = static_cast<std::uint64_t>(getIntOption_("calibration:panel") > 0
          ? getIntOption_("calibration:panel")
          : std::max(1.0, getDoubleOption_("calibration:panel_fraction") * static_cast<double>(library.rows())));
      const std::size_t keep_every = static_cast<std::size_t>(std::max<std::uint64_t>(1, library.rows() / (2 * panel_size)));
      const int panel_threads = std::max(4, threads - ingest_threads);
      panel_future = std::async(std::launch::async, [lib_file, keep_every, panel_threads] {
        const ODIA::StageTime t0 = ODIA::stageClock();
        omp_set_num_threads(panel_threads);
        Panel p;
        ODIA::LibraryLoadOptions popt;
        popt.top_fragments = 6;
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
                    << grid.bytes() / double(1u << 20) << " MiB; " << fmt(gs.ingest) << mem() << std::endl;

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
          : std::max(1.0, getDoubleOption_("calibration:panel_fraction") * static_cast<double>(library.rows())));
      copt.panel_fraction = getDoubleOption_("calibration:panel_fraction");
      copt.search_ppm = getDoubleOption_("calibration:search_ppm");
      copt.search_narrow_factor = getDoubleOption_("calibration:search_factor");
      copt.search_rounds = getIntOption_("calibration:search_rounds");
      copt.accuracy_sigma = getDoubleOption_("calibration:accuracy_sigma");
      copt.min_relative_intensity = getDoubleOption_("calibration:min_relative_intensity");
      copt.strict_min_fragments = getIntOption_("calibration:strict_min_fragments");
      copt.strict_min_correlation = getDoubleOption_("calibration:strict_min_correlation");
      copt.strict_min_cosine = getDoubleOption_("calibration:strict_min_cosine");
      copt.min_fragments = getIntOption_("calibration:min_fragments");
      copt.min_correlation = getDoubleOption_("calibration:min_correlation");
      copt.min_library_cosine = getDoubleOption_("calibration:min_cosine");
      copt.half_window_cycles = getIntOption_("calibration:half_window_cycles");
      copt.candidates = getIntOption_("calibration:candidates");
      copt.mode_bins = getIntOption_("calibration:mode_bins");
      copt.mode_bandwidth_s = getDoubleOption_("calibration:mode_bandwidth");
      copt.min_window_s = getDoubleOption_("calibration:min_window");
      copt.lowess_span = getDoubleOption_("calibration:lowess_span");
      copt.rejection_rounds = getIntOption_("calibration:rejection_rounds");
      copt.min_reject_s = getDoubleOption_("calibration:min_reject");
      copt.threads = threads;
      cal = ODIA::calibrate(*cal_lib, grid, copt);
      releaseFreedMemory();   // the per-window calibration tiles (up to ~0.5 GiB) go back before extraction
      OPENMS_LOG_INFO << "Calibration: stage 1 " << cal->stage1_anchors << " anchors (" << cal->stage1_used
                      << " used), stage 2 window +-" << cal->stage2_window_s << " s; "
                      << cal->anchors.size() << " anchors from a panel of " << cal->panel << ", "
                      << cal->used << " used; RT residual |p50/p95/p99| " << cal->rt_residual_p50 << " / "
                      << cal->rt_residual_p95 << " / " << cal->rt_residual_p99 << " s; MS2 offset "
                      << cal->ms2_offset_ppm << " ppm (sigma " << cal->ms2_sigma_ppm << "), MS1 offset "
                      << cal->ms1_offset_ppm << " ppm (sigma " << cal->ms1_sigma_ppm << "); accuracy MS2 +-" << cal->ms2_accuracy_ppm
                      << " / MS1 +-" << cal->ms1_accuracy_ppm << " ppm (from " << cal->strict_mass_anchors << " anchors; " << cal->search_rounds << " search passes, final windows +-"
                      << cal->ms2_search_ppm << " / +-" << cal->ms1_search_ppm << " ppm); " << fmt(cal->time) << std::endl;
      if (!getStringOption_("out_calibration").empty()) { ODIA::writeAnchors(getStringOption_("out_calibration"), *cal_lib, *cal); }
    }
    ODIA::RtMap rt_map;
    if (mode == "identity") { rt_map = ODIA::RtMap::identity(); }
    else if (mode == "minutes") { rt_map = ODIA::RtMap::identity(60.0); }
    else if (mode == "linear") { rt_map = ODIA::RtMap::linear(getDoubleOption_("rt_intercept"), getDoubleOption_("rt_slope")); }
    else if (mode == "trafo") { rt_map = ODIA::RtMap::fromTrafoXML(getStringOption_("rt_trafo"), getFlag_("rt_trafo_invert")); }
    else if (mode == "anchors") { rt_map = ODIA::RtMap::fromAnchors(getStringOption_("rt_anchors"), getStringOption_("rt_anchor_model")); }
    else { rt_map = std::move(cal->rt_map); }
    OPENMS_LOG_INFO << "RT map: " << rt_map.describe() << std::endl;

    // --- extraction + export
    ODIA::ExtractOptions eopt;
    eopt.ms2_ppm = getDoubleOption_("mz_tol");
    eopt.ms1_ppm = getDoubleOption_("mz_tol_ms1");
    if (eopt.ms2_ppm <= 0.0)
    {
      if (!cal) { OPENMS_LOG_ERROR << "-mz_tol 0 (measured MS2 accuracy) needs -mass_calibration auto; with it off, give -mz_tol <ppm>." << std::endl; return ILLEGAL_PARAMETERS; }
      eopt.ms2_ppm = cal->ms2_accuracy_ppm;
      OPENMS_LOG_INFO << "MS2 tolerance: +-" << eopt.ms2_ppm << " ppm (measured)" << std::endl;
    }
    if (eopt.ms1_ppm <= 0.0)
    {
      if (!cal) { OPENMS_LOG_ERROR << "-mz_tol_ms1 0 (measured MS1 accuracy, the default) needs -mass_calibration auto; with it off, give -mz_tol_ms1 <ppm>." << std::endl; return ILLEGAL_PARAMETERS; }
      eopt.ms1_ppm = cal->ms1_accuracy_ppm;
      OPENMS_LOG_INFO << "MS1 tolerance: +-" << eopt.ms1_ppm << " ppm (measured)" << std::endl;
    }
    eopt.ms2_ppm_offset = mass_auto ? cal->ms2_offset_ppm : getDoubleOption_("mz_offset");
    eopt.ms1_ppm_offset = mass_auto ? cal->ms1_offset_ppm : getDoubleOption_("mz_offset_ms1");
    eopt.rt_half_window = getDoubleOption_("rt_window");
    eopt.aggregate_sum = getStringOption_("aggregation") == "sum";
    eopt.extract_ms1 = getIntOption_("ms1_isotopes") >= 0;
    eopt.ms1_isotopes = std::max(0, getIntOption_("ms1_isotopes"));
    eopt.assign_all_windows = getStringOption_("window_assignment") == "all";
    eopt.threads = threads;
    eopt.isotope_unit_spacing = getStringOption_("ms1_isotope_spacing") == "unit";
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
    wcfg.axes = out.empty() ? out_axes : out_axes;
    wcfg.tsv = out_tsv;
    wcfg.run_name = getStringOption_("run_name");
    if (wcfg.run_name.empty()) { wcfg.run_name = std::filesystem::path(in).stem().string(); }
    wcfg.log8 = getStringOption_("encoding") == "log8";
    wcfg.log8_decades = getDoubleOption_("log8_decades");
    wcfg.expand_rt = getFlag_("expand_rt");
    wcfg.skip_empty = getFlag_("skip_empty");
    wcfg.shards = getIntOption_("out_shards");
    wcfg.isotope_unit_spacing = eopt.isotope_unit_spacing;
    wcfg.ms1_scan_axis = eopt.ms1_scan_axis;
    wcfg.metadata = {{"input", in}, {"library", lib_file}, {"rt_map", rt_map.describe()},
                     {"mz_tol_ppm", std::to_string(eopt.ms2_ppm)}, {"mz_tol_ms1_ppm", std::to_string(eopt.ms1_ppm)},
                     {"aggregation", getStringOption_("aggregation")},
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
    OPENMS_LOG_INFO << "Written: " << writer.rows() << " rows, " << writer.points() << " points in " << writer.shardCount() << " Parquet part(s) (busiest writer "
                    << writer.busySeconds() << " s, final drain " << t_close.wall << " s)" << std::endl;
    if (ODIA::prof::enabled()) { ODIA::prof::report(stderr); }
    if (std::getenv("ODIA_PROFILE") != nullptr)
    {
      auto* ap = ODIA::arrowPool();
      const struct mallinfo2 mi = ::mallinfo2();
      std::fprintf(stderr, "[memory profile] arrow pool '%s': %.2f GiB allocated now, %.2f GiB max, %.2f GiB mapped, %lld reuses; glibc: %.2f GiB arena "
                   "(%.2f in use, %.2f free), %.2f GiB mmapped\n", ap->backend_name().c_str(), ap->bytes_allocated() / 1073741824.0,
                   ap->max_memory() / 1073741824.0, ap->mappedBytes() / 1073741824.0, static_cast<long long>(ap->reuses()), mi.arena / 1073741824.0, mi.uordblks / 1073741824.0,
                   mi.fordblks / 1073741824.0, mi.hblkhd / 1073741824.0);
    }
    OPENMS_LOG_INFO << "Total: " << fmt(t_total) << ", peak RSS " << peakRssGiB() << " GiB" << std::endl;

    const std::string stats_file = getStringOption_("out_stats");
    if (!stats_file.empty())
    {
      std::ofstream js(stats_file);
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
  if (std::getenv("ODIA_ALLOW_PTRACE") != nullptr) { ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0); }
  // Idle OpenMP threads should sleep, not spin (spinning burned ~70% extra CPU while workers waited on
  // I/O). libgomp reads OMP_WAIT_POLICY only when it is loaded, so set it and re-execute once; an
  // explicit user setting is respected.
  if (std::getenv("OMP_WAIT_POLICY") == nullptr)
  {
    ::setenv("OMP_WAIT_POLICY", "PASSIVE", 1);
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) { args.push_back(const_cast<char*>(argv[i])); }
    args.push_back(nullptr);
    ::execv("/proc/self/exe", args.data());
    // execv only returns on failure: continue with the inherited policy
  }
  TOPPDIATraceExtractor tool;
  return tool.main(argc, argv);
}
