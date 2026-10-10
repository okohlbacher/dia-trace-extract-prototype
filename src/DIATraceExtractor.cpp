// ODIA-V3 trace extractor -- TOPP tool.
//
// Stages: library -> run grid (one global RT axis) -> parallel gather -> ordered export.
#include "Calibration.h"
#include "Extractor.h"
#include "Library.h"
#include "Pipeline.h"
#include "RtMap.h"
#include "RunGrid.h"
#include "TraceWriter.h"

#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/CONCEPT/LogStream.h>

#include <filesystem>
#include <optional>
#include <omp.h>
#include <fstream>
#include <iomanip>
#include <sstream>
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
    registerInputFile_("in", "<file>", "", "DIA run: centroided mzML or Thermo .raw.");
    setValidFormats_("in", {"mzML", "raw"});
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

    registerDoubleOption_("mz_tol", "<ppm>", 10.0, "Fragment tolerance, +- ppm (half width).", false);
    registerDoubleOption_("mz_tol_ms1", "<ppm>", 10.0, "MS1 tolerance, +- ppm (half width).", false);
    registerStringOption_("mass_calibration", "<mode>", "off", "auto: measure the fragment and MS1 mass offsets "
                          "from the run (calibration anchors) and apply them instead of -mz_offset*.", false);
    setValidStrings_("mass_calibration", {"off", "auto"});
    registerOutputFile_("out_calibration", "<file>", "", "Calibration anchors (TSV; usable as -rt_anchors).", false);
    registerIntOption_("calibration:panel", "<n>", 200000, "Precursors sampled for calibration (stratified over "
                       "library RT; never restricted by -ids).", false, true);
    registerDoubleOption_("calibration:search_ppm", "<ppm>", 20.0, "Uncalibrated half width for the panel.", false, true);
    registerDoubleOption_("calibration:min_correlation", "<r>", 0.8, "Stage 2: mean pairwise fragment correlation "
                          "at an anchor apex (stage 1 uses 0.9).", false, true);
    registerDoubleOption_("calibration:min_cosine", "<c>", 0.75, "Stage 2: cosine of sqrt(apex) vs sqrt(library) "
                          "intensities at an anchor apex (stage 1 uses 0.85).", false, true);
    registerDoubleOption_("calibration:lowess_span", "<f>", 0.15, "LOWESS span of the RT model.", false, true);
    registerDoubleOption_("mz_offset", "<ppm>", 0.0, "Fragment mass correction (ppm added to the target).", false);
    registerDoubleOption_("mz_offset_ms1", "<ppm>", 0.0, "MS1 mass correction (ppm added to the target).", false);
    registerStringOption_("aggregation", "<mode>", "sum", "Combine centroids within tolerance: sum (OpenSWATH) "
                          "or max (DIA-NN).", false);
    setValidStrings_("aggregation", {"sum", "max"});
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
    registerIntOption_("ingest_threads", "<n>", 1, "Threads for OpenMS's binary-array decoding during ingest "
                       "(the XML parse is serial; more threads mostly spin).", false, true);
    setMinInt_("ingest_threads", 1);
    registerIntOption_("unit_precursors", "<n>", 256, "Precursors per parallel work unit.", false, true);
    registerStringOption_("run_name", "<name>", "", "Run name in the output (default: input file stem).", false);
  }

  ExitCodes main_(int, const char**) override
  {
    const ODIA::StageTime t_start = ODIA::stageClock();
    const std::string in = getStringOption_("in"), lib_file = getStringOption_("lib");
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

    // --- library
    const ODIA::StageTime t_lib0 = ODIA::stageClock();
    ODIA::LibraryLoadOptions lopt;
    lopt.top_fragments = static_cast<std::size_t>(getIntOption_("top_fragments"));
    lopt.targets_only = getFlag_("targets_only");
    if (!getStringOption_("ids").empty()) { lopt.keep_ids = ODIA::readIdList(getStringOption_("ids")); }
    ODIA::Library lib;
    ODIA::loadLibrary(lib_file, lopt, lib);
    const ODIA::StageTime t_lib = ODIA::stageClock() - t_lib0;
    OPENMS_LOG_INFO << "Library: " << lib.size() << " precursors, " << lib.fragmentCount() << " fragments; "
                    << fmt(t_lib) << std::endl;
    if (lib.size() == 0) { OPENMS_LOG_ERROR << "Library is empty after filtering." << std::endl; return INPUT_FILE_EMPTY; }

    // --- run grid
    ODIA::RunGrid grid = ODIA::RunGrid::load(in, getIntOption_("ingest_threads"));
    omp_set_num_threads(threads);
    const auto& gs = grid.stats;
    OPENMS_LOG_INFO << "Run grid: " << grid.cycles() << " cycles x " << grid.windows.size() << " windows, "
                    << gs.ms1_spectra << " MS1 / " << gs.ms2_spectra << " MS2 spectra, "
                    << gs.ms1_peaks + gs.ms2_peaks << " peaks, " << gs.missing_cells << " empty cells, "
                    << grid.bytes() / double(1u << 20) << " MiB; " << fmt(gs.ingest) << std::endl;

    // --- calibration (from the run itself) and RT map
    const std::string mode = getStringOption_("rt_mode");
    const bool mass_auto = getStringOption_("mass_calibration") == "auto";
    std::optional<ODIA::CalibrationResult> cal;
    if (mode == "auto" || mass_auto)
    {
      // The panel is drawn from the whole library, never from an -ids subset: calibrating on externally
      // confirmed precursors would not be self-calibration.
      ODIA::Library full;
      const ODIA::Library* cal_lib = &lib;
      if (!lopt.keep_ids.empty())
      {
        ODIA::LibraryLoadOptions copt;
        copt.top_fragments = 6;
        copt.targets_only = true;
        ODIA::loadLibrary(lib_file, copt, full);
        cal_lib = &full;
      }
      ODIA::CalibrationOptions copt;
      copt.panel = static_cast<std::size_t>(getIntOption_("calibration:panel"));
      copt.search_ppm = getDoubleOption_("calibration:search_ppm");
      copt.min_correlation = getDoubleOption_("calibration:min_correlation");
      copt.min_library_cosine = getDoubleOption_("calibration:min_cosine");
      copt.lowess_span = getDoubleOption_("calibration:lowess_span");
      copt.threads = threads;
      cal = ODIA::calibrate(*cal_lib, grid, copt);
      OPENMS_LOG_INFO << "Calibration: stage 1 " << cal->stage1_anchors << " anchors (" << cal->stage1_used
                      << " used), stage 2 window +-" << cal->stage2_window_s << " s; "
                      << cal->anchors.size() << " anchors from a panel of " << cal->panel << ", "
                      << cal->used << " used; RT residual |p50/p95/p99| " << cal->rt_residual_p50 << " / "
                      << cal->rt_residual_p95 << " / " << cal->rt_residual_p99 << " s; MS2 offset "
                      << cal->ms2_offset_ppm << " ppm (sigma " << cal->ms2_sigma_ppm << "), MS1 offset "
                      << cal->ms1_offset_ppm << " ppm (sigma " << cal->ms1_sigma_ppm << "); " << fmt(cal->time) << std::endl;
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
    eopt.ms2_ppm_offset = mass_auto ? cal->ms2_offset_ppm : getDoubleOption_("mz_offset");
    eopt.ms1_ppm_offset = mass_auto ? cal->ms1_offset_ppm : getDoubleOption_("mz_offset_ms1");
    eopt.rt_half_window = getDoubleOption_("rt_window");
    eopt.aggregate_sum = getStringOption_("aggregation") == "sum";
    eopt.extract_ms1 = getIntOption_("ms1_isotopes") >= 0;
    eopt.ms1_isotopes = std::max(0, getIntOption_("ms1_isotopes"));
    eopt.assign_all_windows = getStringOption_("window_assignment") == "all";
    eopt.threads = threads;
    eopt.isotope_unit_spacing = getStringOption_("ms1_isotope_spacing") == "unit";
    eopt.unit_precursors = static_cast<std::size_t>(std::max(1, getIntOption_("unit_precursors")));
    ODIA::Extractor extractor(lib, rt_map, grid, eopt);

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
    wcfg.isotope_unit_spacing = eopt.isotope_unit_spacing;
    wcfg.metadata = {{"input", in}, {"library", lib_file}, {"rt_map", rt_map.describe()},
                     {"mz_tol_ppm", std::to_string(eopt.ms2_ppm)}, {"mz_tol_ms1_ppm", std::to_string(eopt.ms1_ppm)},
                     {"aggregation", getStringOption_("aggregation")},
                     {"rt_window_half_s", std::to_string(eopt.rt_half_window)},
                     {"ms1_mapping", "linear interpolation of MS1 scans at cycle_rt"}};
    ODIA::TraceWriter writer(lib, grid, wcfg);

    // Parallel stage: extract a unit and encode it for output. Ordered stage: account + write.
    struct Done
    {
      std::vector<ODIA::TraceRecord> records;
      std::uint64_t lookups = 0;
      ODIA::TraceWriter::Encoded encoded;
    };
    const ODIA::StageTime t_ext0 = ODIA::stageClock();
    ODIA::orderedParallel(
      extractor.units(), threads, 4 * static_cast<std::size_t>(threads),
      [&](std::size_t u) {
        ODIA::TraceBlock b = extractor.extract(u);
        Done d;
        d.encoded = writer.encode(b);
        d.lookups = b.lookups;
        d.records = std::move(b.records);
        return d;
      },
      [&](Done&& d) {
        extractor.account(d.records, d.lookups);
        writer.push(std::move(d.encoded));
      });
    extractor.setTime(ODIA::stageClock() - t_ext0);
    const ODIA::StageTime t_close0 = ODIA::stageClock();
    writer.close();
    const ODIA::StageTime t_close = ODIA::stageClock() - t_close0;
    const ODIA::StageTime t_total = ODIA::stageClock() - t_start;

    const auto& es = extractor.stats();
    OPENMS_LOG_INFO << "Extraction: " << es.assignments << " precursor-window assignments in " << es.units
                    << " units, " << es.without_window << " precursors without window, " << es.empty_range
                    << " outside the acquired RT range; " << es.traces << " traces, " << es.ms2_points << " MS2 + "
                    << es.ms1_points << " MS1 points, " << es.peak_lookups << " lookups; " << fmt(es.time)
                    << " with " << threads << " threads" << std::endl;
    OPENMS_LOG_INFO << "Written: " << writer.rows() << " rows, " << writer.points() << " points (writer busy "
                    << writer.busySeconds() << " s, final drain " << t_close.wall << " s)" << std::endl;
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
         << "  \"library_precursors\": " << lib.size() << ",\n"
         << "  \"library_fragments\": " << lib.fragmentCount() << ",\n"
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
         << "  \"ms1_offset_ppm\": " << eopt.ms1_ppm_offset << ",\n";
      if (cal)
      {
        js << "  \"calibration\": {\"panel\": " << cal->panel << ", \"anchors\": " << cal->anchors.size()
           << ", \"used\": " << cal->used << ", \"rt_residual_p50\": " << cal->rt_residual_p50
           << ", \"rt_residual_p95\": " << cal->rt_residual_p95 << ", \"rt_residual_p99\": " << cal->rt_residual_p99
           << ", \"ms2_offset_ppm\": " << cal->ms2_offset_ppm << ", \"ms2_sigma_ppm\": " << cal->ms2_sigma_ppm
           << ", \"ms1_offset_ppm\": " << cal->ms1_offset_ppm << ", \"ms1_sigma_ppm\": " << cal->ms1_sigma_ppm
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
  TOPPDIATraceExtractor tool;
  return tool.main(argc, argv);
}
