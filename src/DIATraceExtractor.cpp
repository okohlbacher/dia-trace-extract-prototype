// ODIA-V3 trace extractor -- TOPP tool.
//
// Stages: library -> run grid (one global RT axis) -> parallel gather -> ordered export.
#include "ArrowPool.h"
#include "Calibration.h"
#include "Extractor.h"
#include "Instruments.h"
#include "Library.h"
#include "Pipeline.h"
#include "Profile.h"
#include "RtMap.h"
#include "RunModel.h"
#include "RunGrid.h"
#include "Scheduler.h"
#include "TraceWriter.h"

#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/FORMAT/DATAACCESS/MSDataWritingConsumer.h>
#include <OpenMS/CONCEPT/LogStream.h>

#include <algorithm>
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
#include <iostream>
#include <array>
#include <iterator>
#include <map>
#include <set>
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

  bool g_debug = false;   ///< -debug >= 1: the development detail of the log lines

  std::string mem()
  {
    if (!g_debug) { return ""; }
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

  std::string lowerCase(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
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

/// The -instrument help line: the names and fragment multiples of the instrument files.
std::string instrumentHelp()
{
  std::string t = "Instrument whose defaults apply: auto (from the run's metadata) or its name, for runs whose metadata lack it";
  try
  {
    std::string w, list;
    const std::string names = ODIA::knownInstruments();
    std::istringstream is(names);
    std::string name;
    while (std::getline(is, name, ','))
    {
      while (!name.empty() && name.front() == ' ') { name.erase(0, 1); }
      const ODIA::InstrumentDefaults d = ODIA::resolveInstrument(name, "", "", w);
      std::ostringstream o; o << d.value("mz_accuracy_ms2");
      list += (list.empty() ? "" : ", ") + name + " " + o.str();
    }
    const ODIA::InstrumentDefaults d = ODIA::resolveInstrument("auto", "", "", w);
    std::ostringstream o; o << d.value("mz_accuracy_ms2");
    t += ". Sets the fragment tolerance as a multiple of the measured mass-error width: " + list + ", others " + o.str()
       + " (measured for Astral and timsTOF; the Orbitrap and TOF classes follow them)";
  }
  catch (const std::exception&) {}
  return t + ".";
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

/// The option names of 0.2.14 and earlier (accepted until 0.3, removed in 0.4), for one hint line when they are used.
struct OldName { const char* name; const char* hint; };
const OldName kOldNames[] = {
  {"mz_tol", "renamed to -mz_tol_ms2 in 0.2.15"},
  {"mz_sigma_ms2", "renamed to -mz_accuracy_ms2 in 0.2.15"},
  {"mz_sigma_ms1", "renamed to -mz_accuracy_ms1 in 0.2.15"},
  {"mass_calibration", "renamed to -mz_calibration in 0.2.15"},
  {"mz_offset", "renamed to -mz_offset_ms2 in 0.2.15"},
  {"calibration:mass_model", "renamed to -calibration:mz_model in 0.2.15"},
  {"out_shards", "renamed to -write_parts in 0.2.15"},
  {"rt_intercept", "removed in 0.2.15: the linear RT map; give -rt_anchors"},
  {"rt_slope", "removed in 0.2.15: the linear RT map; give -rt_anchors"},
  {"rt_anchor_model", "removed in 0.2.15: anchors are fitted with LOWESS"},
  {"aggregation", "removed in 0.2.15: OpenSWATH's sum is part of -scheme B"},
  {"ms1_isotope_spacing", "removed in 0.2.15: OpenSWATH's unit spacing is part of -scheme B"},
  {"window_assignment", "removed in 0.2.15: OpenSWATH's assignment is part of -scheme B"},
  {"log8_decades", "removed in 0.2.15: the log8 encoding spans 6 decades"},
  {"ingest_threads", "removed in 0.2.15: the ingest uses min(threads, 32)"},
  {"library_part_rows", "removed in 0.2.15: fixed at 65536"},
  {"calibration:panel_fraction", "removed in 0.2.15: fixed at 0.05; -calibration:panel sets the panel size"},
  {"calibration:search_factor", "removed in 0.2.15: fixed at 3"},
  {"calibration:search_rounds", "removed in 0.2.15: fixed at 3"},
  {"calibration:accuracy_rule", "removed in 0.2.15: the tolerance is -mz_accuracy_ms2 x the measured width"},
  {"calibration:coverage", "removed in 0.2.15: fixed at 0.99"},
  {"calibration:coverage_factor", "removed in 0.2.15: fixed at 1.3"},
  {"calibration:min_relative_intensity", "removed in 0.2.15: fixed at 0"},
  {"calibration:strict_min_fragments", "removed in 0.2.15: fixed at 5"},
  {"calibration:strict_min_correlation", "removed in 0.2.15: fixed at 0.9"},
  {"calibration:strict_min_cosine", "removed in 0.2.15: fixed at 0.85"},
  {"calibration:min_fragments", "removed in 0.2.15: fixed at 4"},
  {"calibration:min_correlation", "removed in 0.2.15: fixed at 0.8"},
  {"calibration:min_cosine", "removed in 0.2.15: fixed at 0.75"},
  {"calibration:half_window_cycles", "removed in 0.2.15: measured from the peak width"},
  {"calibration:seed_window_cycles", "removed in 0.2.15: fixed at 4"},
  {"calibration:anchor_fdr", "removed in 0.2.15: the decoy-calibrated cut is not used"},
  {"calibration:decoy_shift", "removed in 0.2.15: fixed at 0.4 Th"},
  {"calibration:candidates", "removed in 0.2.15: fixed at 5"},
  {"calibration:mode_bins", "removed in 0.2.15: fixed at 20"},
  {"calibration:mode_bandwidth", "removed in 0.2.15: fixed at 15 s"},
  {"calibration:min_window", "removed in 0.2.15: fixed at 30 s"},
  {"calibration:rejection_rounds", "removed in 0.2.15: fixed at 3"},
  {"calibration:min_reject", "removed in 0.2.15: fixed at 3 s"},
};

/// An old option name on the command line or in the -ini file gets one hint line before TOPP's own error.
void hintOldNames(int argc, const char** argv)
{
  std::vector<std::string> ini_names;
  for (int i = 1; i < argc; ++i)
  {
    std::string a = argv[i];
    if ((a == "-ini" || a == "--ini") && i + 1 < argc)
    {
      std::ifstream f(argv[i + 1]);
      const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      for (const auto& o : kOldNames) { if (text.find("name=\"" + std::string(o.name) + "\"") != std::string::npos) { ini_names.push_back("-" + std::string(o.name)); } }
      continue;
    }
    while (!a.empty() && a.front() == '-') { a.erase(0, 1); }
    for (const auto& o : kOldNames) { if (a == o.name) { std::cerr << "-" << o.name << " was " << o.hint << " (accepted until 0.3, removed in 0.4)." << std::endl; } }
    if (a == "rt_mode" && i + 1 < argc)
    {
      const std::string m = argv[i + 1];
      if (m == "identity" || m == "linear") { std::cerr << "-rt_mode " << m << " of 0.2.14: use -rt_mode seconds (library RT = run seconds), or -rt_anchors for another linear map." << std::endl; }
      if (m == "anchors" || m == "trafo") { std::cerr << "-rt_mode " << m << " of 0.2.14: the file alone selects the map (-rt_" << m << " <file>)." << std::endl; }
    }
  }
  if (!ini_names.empty())
  {
    std::string t;
    for (std::size_t k = 0; k < ini_names.size() && k < 5; ++k) { t += (k ? ", " : "") + ini_names[k]; }
    std::cerr << "The -ini file is from 0.2.14 or earlier (" << t << (ini_names.size() > 5 ? ", ..." : "")
              << "): convert it with DIATraceExtractor 0.3 (-write_ini new.ini -ini old.ini) or rename its entries." << std::endl;
  }
}

class TOPPDIATraceExtractor : public TOPPBase
{
public:
  TOPPDIATraceExtractor()
    : TOPPBase("DIATraceExtractor",
               "Extract fragment and MS1 isotope traces (XICs) of a spectral library from a DIA run. "
               "RT map, RT window, mass offsets and tolerances are measured from the run; uses all physical "
               "cores unless -threads is given.", {}, false)
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
    registerStringOption_("in", "<file>", "", "DIA run: centroided mzML, mzPeak (.mzpeak; MS1 centroided in the file) or "
                          "Thermo .raw.");
    registerInputFile_("lib", "<file>", "",
                       "Spectral library: DIA-NN/DIALibGen .parquet or .tsv, OpenSWATH .tsv/.pqp/.TraML.", false);
    registerOutputFile_("out", "<file>", "", "Traces (Parquet), one row per trace. The RT axes go to <out stem>.axes.parquet "
                        "next to it.", false);
    registerOutputFile_("out_tsv", "<file>", "", "Traces as a long TSV, one row per point (large).", false);
    registerOutputFile_("out_stats", "<file>", "", "Run statistics (JSON): calibration, the values used and their sources, stage "
                        "times, memory.", false);
    registerStringOption_("instrument", "<name>", "auto", instrumentHelp(), false);
    registerInputFile_("ids", "<file>", "", "Extract only these precursors: the library's precursor ids, one per line "
                       "(DIA-NN: modified sequence + charge, e.g. AAAFEGELIPASQIDR2; the first column of a TSV with a "
                       "Precursor.Id header also works). Ids not in the library are counted in the log.", false);
    registerIntOption_("top_fragments", "<n>", 6, "Most intense library fragments per precursor (0 = all).", false);
    setMinInt_("top_fragments", 0);

    registerStringOption_("rt_mode", "<mode>", "auto",
                          "How library RT maps to the run: auto (fitted from the run itself; any library RT scale), "
                          "minutes (library RT is run minutes, e.g. a library tuned to this run), seconds (run seconds). "
                          "-rt_anchors or -rt_trafo select a given map instead.", false);
    setValidStrings_("rt_mode", {"auto", "minutes", "seconds"});
    registerDoubleOption_("rt_window", "<s>", 0.0, "Extract +- this many seconds around each precursor's mapped RT. "
                          "0: measured from the run, along the gradient (7 x the RT scatter of the calibration anchors about "
                          "the RT map + 2 x their peak width; +-60 s with a warning when no calibration runs). Negative: "
                          "the whole run.", false);
    registerDoubleOption_("mz_tol_ms2", "<ppm>", 0.0, "Fragment tolerance, +- ppm. 0: measured from the run (the "
                          "instrument's multiple of the fragment mass-error width, see -instrument).", false);
    registerDoubleOption_("mz_tol_ms1", "<ppm>", 0.0, "MS1 tolerance, +- ppm. 0: measured from the run (4 x the MS1 "
                          "mass-error width).", false);
    setMinFloat_("mz_tol_ms2", 0.0); setMinFloat_("mz_tol_ms1", 0.0);
    registerStringOption_("scheme", "<name>", "A", "A (default): DIA-NN's extraction rules (max of the centroids in the window) "
                          "with the measured mass correction. B: OpenSWATH's rules (sum of the centroids, MS1 isotopes at unit "
                          "spacing, every isolation window containing the precursor, no mass correction); needs -mz_tol_ms2 (and "
                          "-mz_tol_ms1 unless -no_ms1).", false);
    setValidStrings_("scheme", {"A", "B"});
    registerOutputFile_("out_mzml", "<file>", "", "Also write the run as a centroided mzML, one spectrum per isolation window "
                        "(diaPASEF frames split by window, scans collapsed), for tools that do not read mzPeak. Without -out "
                        "and -out_tsv only this is done and -lib is not needed.", false);
    setValidFormats_("out_mzml", {"mzML"});

    // ---- advanced
    registerOutputFile_("out_axes", "<file>", "", "RT axes (Parquet) to join with the traces; default <out stem>.axes.parquet.", false, true);
    registerOutputFile_("out_calibration", "<file>", "", "Calibration anchors (TSV); its first two columns are the RT map, "
                        "for -rt_anchors in a later run.", false, true);
    registerStringOption_("run_name", "<name>", "", "The traces' run column (default: the input file name without "
                          "extension).", false, true);
    registerFlag_("targets_only", "Skip library decoys.", true);
    registerFlag_("no_ms1", "No MS1 traces (fragments only).", true);
    registerIntOption_("ms1_isotopes", "<n>", 2, "MS1 isotope peaks after the monoisotopic one (0: monoisotopic only).", false, true);
    setMinInt_("ms1_isotopes", 0); setMaxInt_("ms1_isotopes", 200);
    registerStringOption_("ms1_axis", "<axis>", "scans", "MS1 traces on the MS1 scans (their own times, in the axes file) or "
                          "interpolated onto the MS2 cycle axis (cycles).", false, true);
    setValidStrings_("ms1_axis", {"scans", "cycles"});
    registerInputFile_("rt_anchors", "<file>", "", "Use this RT map instead: two columns, library RT and run RT (s), fitted "
                       "with LOWESS (e.g. -out_calibration of another run).", false, true);
    registerInputFile_("rt_trafo", "<file>", "", "Use this RT map instead: trafoXML, library RT -> run RT (s).", false, true);
    setValidFormats_("rt_trafo", {"trafoXML"});
    registerFlag_("rt_trafo_invert", "The trafoXML maps run RT -> library RT (as OpenSwathRTNormalizer writes it).", true);
    registerInputFile_("instrument_file", "<yaml>", "", "Instrument defaults for this run from this file (keys: name, extends, "
                       "aliases, keywords, priority, mz_accuracy_ms2, mz_accuracy_ms1, rt_window_sigma, rt_window_fwhm), "
                       "over the shipped files in "
                       "<prefix>/share/DIATraceExtractor/instruments. Files of your own for every run: "
                       "$DIATRACEEXTRACTOR_INSTRUMENT_DIR or the user directory (~/.config/DIATraceExtractor/instruments; "
                       "macOS ~/Library/Application Support/..., Windows %APPDATA%\\...), where a file replaces the shipped "
                       "one of the same name.", false, true);
    setValidFormats_("instrument_file", {"yaml"}, false);
    registerDoubleOption_("mz_accuracy_ms2", "<k>", 0.0, "Measured fragment tolerance = k x the robust width of the fragment mass "
                          "errors. 0: the instrument's (see -instrument). Not used with a fixed -mz_tol_ms2.", false, true);
    registerDoubleOption_("mz_accuracy_ms1", "<k>", 0.0, "Measured MS1 tolerance = k x the robust width of the MS1 mass errors. "
                          "0: the instrument's (4 for every shipped instrument). Not used with a fixed -mz_tol_ms1.", false, true);
    setMinFloat_("mz_accuracy_ms2", 0.0); setMinFloat_("mz_accuracy_ms1", 0.0);
    registerStringOption_("mz_calibration", "<mode>", "auto", "Mass correction: auto corrects the m/z by the offsets "
                          "measured from the run; off uses -mz_offset_ms2 and -mz_offset_ms1 instead and needs -mz_tol_ms2 "
                          "(and -mz_tol_ms1 unless -no_ms1).", false, true);
    setValidStrings_("mz_calibration", {"auto", "off"});
    registerDoubleOption_("mz_offset_ms2", "<ppm>", 0.0, "Fragment m/z correction with -mz_calibration off (ppm added to the target).", false, true);
    registerDoubleOption_("mz_offset_ms1", "<ppm>", 0.0, "MS1 m/z correction with -mz_calibration off (ppm added to the target).", false, true);
    registerStringOption_("encoding", "<enc>", "float32", "Trace intensities: float32, or log8 (1 byte per point, log-spaced "
                          "over 6 decades below each trace's maximum).", false, true);
    setValidStrings_("encoding", {"float32", "log8"});
    registerFlag_("expand_rt", "Also write each trace's RT values in its row (larger file).", true);
    registerFlag_("skip_empty", "Do not write traces that are all zero.", true);
    registerIntOption_("write_parts", "<n>", 32, "Parquet writers working in parallel on the one -out file (1: a single "
                       "writer, slower for large libraries). Fixed, so that the output does not depend on -threads.", false, true);
    setMinInt_("write_parts", 1);
    registerIntOption_("unit_precursors", "<n>", 256, "Precursors per parallel work unit.", false, true);
    setMinInt_("unit_precursors", 1);
    registerIntOption_("buffer_mb", "<MiB>", 512, "Memory for extracted traces waiting to be written.", false, true);
    setMinInt_("buffer_mb", 16);
    // TOPPBase::printUsage_ throws for an option whose subsection is not registered: register the subsection.
    registerTOPPSubsection_("calibration", "Calibration from the run itself");
    registerStringOption_("calibration:mz_model", "<model>", "linear", "Mass correction: linear (offset + slope x m/z, kept "
                          "only where the m/z dependence is material, else constant) or constant.", false, true);
    setValidStrings_("calibration:mz_model", {"constant", "linear"});
    registerIntOption_("calibration:panel", "<n>", 0, "Precursors sampled for calibration (0: 5% of the library).", false, true);
    setMinInt_("calibration:panel", 0);
    registerDoubleOption_("calibration:search_ppm", "<ppm>", 50.0, "First-pass mass search half width, before calibration.", false, true);
    setMinFloat_("calibration:search_ppm", 0.1);
    registerDoubleOption_("calibration:lowess_span", "<f>", 0.15, "LOWESS span of the fitted RT map.", false, true);
    setMinFloat_("calibration:lowess_span", 0.01); setMaxFloat_("calibration:lowess_span", 1.0);

  }

  double D(const std::string& n) { return getDoubleOption_(n); }
  int I(const std::string& n) { return getIntOption_(n); }
  std::string S(const std::string& n) { return getStringOption_(n); }
  bool F(const std::string& n) { return getFlag_(n); }

  ExitCodes main_(int argc, const char** argv) override
  {
    // TOPPBase's -threads defaults to 1; this tool uses all cores unless -threads (or an INI file) is given.
    for (int i = 1; i < argc; ++i)
    {
      const std::string a = argv[i];
      if (a == "-threads" || a == "--threads") { threads_given_ = true; }
      if ((a == "-ini" || a == "--ini") && i + 1 < argc)
      {
        // an INI file sets the thread count only when it has a threads entry (TOPP writes one; hand-written may not)
        std::ifstream f(argv[i + 1]);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.find("name=\"threads\"") != std::string::npos) { threads_given_ = true; }
      }
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
    // default: the physical cores the process may use (SMT siblings do not help the extraction); 0: every hardware
    // thread, as TOPP documents
    return threads_given_ ? std::max(1, omp_get_num_procs()) : ODIA::platform::physicalCores();
  }

  /// Two outputs on one path, or an output over an input, is an error (before any work).
  ExitCodes checkPaths_(const std::string& in, const std::string& out_axes)
  {
    namespace fs = std::filesystem;
    const auto norm = [](const std::string& p) { std::error_code ec; const fs::path a = fs::weakly_canonical(fs::absolute(p), ec); return ec ? fs::absolute(p).lexically_normal() : a; };
    std::vector<std::pair<std::string, std::string>> outputs, inputs;
    for (const char* o : {"out", "out_tsv", "out_stats", "out_calibration", "out_mzml"}) { if (!S(o).empty()) { outputs.emplace_back(std::string("-") + o, S(o)); } }
    if (!out_axes.empty()) { outputs.emplace_back(S("out_axes").empty() ? "the axes file of -out (" + out_axes + ")" : "-out_axes", out_axes); }
    inputs.emplace_back("-in", in);
    for (const char* i : {"lib", "ids", "rt_anchors", "rt_trafo", "instrument_file"}) { if (!S(i).empty()) { inputs.emplace_back(std::string("-") + i, S(i)); } }
    for (std::size_t a = 0; a < outputs.size(); ++a)
    {
      for (std::size_t b = a + 1; b < outputs.size(); ++b)
      {
        if (norm(outputs[a].second) == norm(outputs[b].second))
        {
          OPENMS_LOG_ERROR << outputs[a].first << " and " << outputs[b].first << " name the same file: " << outputs[a].second << std::endl;
          return ILLEGAL_PARAMETERS;
        }
      }
      for (const auto& i : inputs)
      {
        if (norm(outputs[a].second) == norm(i.second))
        {
          OPENMS_LOG_ERROR << outputs[a].first << " would overwrite the input " << i.first << ": " << i.second << std::endl;
          return ILLEGAL_PARAMETERS;
        }
      }
    }
    return EXECUTION_OK;
  }

  /// Options that cannot work together are errors; options without effect in the given combination are named in one
  /// WARNING line (an option counts as given when its value differs from its default, on the command line or in an INI).
  ExitCodes checkCombinations_(bool export_only)
  {
    if (F("rt_trafo_invert") && S("rt_trafo").empty()) { OPENMS_LOG_ERROR << "-rt_trafo_invert needs -rt_trafo." << std::endl; return ILLEGAL_PARAMETERS; }
    const auto num = [&](const char* n, double def) { return std::fabs(D(n) - def) > 1e-12; };
    const auto str = [&](const char* n, const char* def) { return S(n) != def; };
    std::vector<std::pair<std::string, std::string>> ignored;   // option, why
    const auto add = [&](bool given, const char* n, const std::string& why) { if (given) { ignored.emplace_back(std::string("-") + n, why); } };
    if (export_only)
    {
      const std::string why = "only -out_mzml is written";
      add(!S("lib").empty(), "lib", why);
      for (const char* n : {"ids", "rt_anchors", "rt_trafo", "out_axes", "out_stats", "out_calibration", "instrument_file"}) { add(!S(n).empty(), n, why); }
      add(str("instrument", "auto"), "instrument", why);
      add(str("rt_mode", "auto"), "rt_mode", why);
      add(num("rt_window", 0.0), "rt_window", why);
      add(num("mz_tol_ms2", 0.0), "mz_tol_ms2", why);
      add(num("mz_tol_ms1", 0.0), "mz_tol_ms1", why);
      add(str("scheme", "A"), "scheme", why);
      add(I("top_fragments") != 6, "top_fragments", why);
    }
    else
    {
      const bool scheme_b = S("scheme") == "B";
      const bool ms1 = !F("no_ms1");
      const bool whole_run = D("rt_window") < 0.0;
      const bool rt_auto = S("rt_mode") == "auto" && S("rt_anchors").empty() && S("rt_trafo").empty() && !whole_run;
      const bool mass_auto = !scheme_b && S("mz_calibration") == "auto";
      if (!ms1)
      {
        const std::string why = "-no_ms1";
        add(I("ms1_isotopes") != 2, "ms1_isotopes", why);
        add(str("ms1_axis", "scans"), "ms1_axis", why);
        add(num("mz_tol_ms1", 0.0), "mz_tol_ms1", why);
        add(num("mz_accuracy_ms1", 0.0), "mz_accuracy_ms1", why);
        add(num("mz_offset_ms1", 0.0) && !scheme_b, "mz_offset_ms1", why);
      }
      if (scheme_b)
      {
        const std::string why = "-scheme B: no mass correction, fixed tolerances";
        add(num("mz_accuracy_ms2", 0.0), "mz_accuracy_ms2", why);
        add(num("mz_accuracy_ms1", 0.0) && ms1, "mz_accuracy_ms1", why);
        add(str("mz_calibration", "auto"), "mz_calibration", why);
        add(str("calibration:mz_model", "linear"), "calibration:mz_model", why);
      }
      else
      {
        add(num("mz_tol_ms2", 0.0) && num("mz_accuracy_ms2", 0.0), "mz_accuracy_ms2", "-mz_tol_ms2 fixes the fragment tolerance");
        add(ms1 && num("mz_tol_ms1", 0.0) && num("mz_accuracy_ms1", 0.0), "mz_accuracy_ms1", "-mz_tol_ms1 fixes the MS1 tolerance");
      }
      if (whole_run)
      {
        const std::string why = "a negative -rt_window extracts the whole run";
        add(str("rt_mode", "auto"), "rt_mode", why);
        add(!S("rt_anchors").empty(), "rt_anchors", why);
        add(!S("rt_trafo").empty(), "rt_trafo", why);
      }
      if (!rt_auto && !mass_auto)
      {
        const std::string why = "no calibration runs (neither the RT map nor the mass correction is measured)";
        add(I("calibration:panel") != 0, "calibration:panel", why);
        add(num("calibration:search_ppm", 50.0), "calibration:search_ppm", why);
        add(str("calibration:mz_model", "linear") && !scheme_b, "calibration:mz_model", why);
        add(num("calibration:lowess_span", 0.15), "calibration:lowess_span", why);
      }
      else if (!rt_auto)
      {
        add(num("calibration:lowess_span", 0.15), "calibration:lowess_span", "the RT map is given (-rt_mode, -rt_anchors or -rt_trafo)");
      }
    }
    if (!ignored.empty())
    {
      std::string t;
      for (std::size_t i = 0; i < ignored.size(); ++i)
      {
        const bool same_why = i + 1 < ignored.size() && ignored[i + 1].second == ignored[i].second;
        t += ignored[i].first + (same_why ? ", " : " (" + ignored[i].second + ")" + (i + 1 < ignored.size() ? "; " : ""));
      }
      OPENMS_LOG_WARN << "WARNING: without effect here: " << t << "." << std::endl;
    }
    return EXECUTION_OK;
  }

  ExitCodes run_(int, const char**)
  {
    g_debug = debug_level_ > 0;
    const ODIA::StageTime t_start = ODIA::stageClock();
    const std::string in = S("in"), lib_file = S("lib");
    if (!std::filesystem::is_regular_file(in) || !std::ifstream(in, std::ios::binary))   // portable readability test
    {
      OPENMS_LOG_ERROR << "Input file not readable: " << in << std::endl;
      return INPUT_FILE_NOT_READABLE;
    }
    const std::string out = S("out"), out_tsv = S("out_tsv");
    std::string out_axes = S("out_axes");
    const std::string out_mzml = S("out_mzml");
    if (out.empty() && out_tsv.empty() && out_mzml.empty())
    {
      OPENMS_LOG_ERROR << "Nothing to do: give -out, -out_tsv and/or -out_mzml." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    const bool export_only = out.empty() && out_tsv.empty();   // only -out_mzml: no library, no calibration
    if (lib_file.empty() && !export_only)
    {
      OPENMS_LOG_ERROR << "-lib is required for an extraction (only -out_mzml works without a library)." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    if (out_axes.empty() && !out.empty())
    {
      std::filesystem::path p(out);
      out_axes = (p.parent_path() / (p.stem().string() + ".axes.parquet")).string();
    }
    if (const ExitCodes c = checkPaths_(in, out_axes); c != EXECUTION_OK) { return c; }
    if (const ExitCodes c = checkCombinations_(export_only); c != EXECUTION_OK) { return c; }
    if (export_only)
    {
      // export only: no library, no calibration
      // the parse is memory-bandwidth bound: flat beyond ~32 threads while its CPU time keeps growing
      ODIA::RunGrid grid = ODIA::RunGrid::load(in, std::min(threadCount_(), 32));
      OPENMS_LOG_INFO << "Run grid: " << grid.cycles() << " cycles x " << grid.windows.size() << " windows, " << grid.stats.ms1_spectra << " MS1 / "
                      << grid.stats.ms2_spectra << " MS2 spectra, " << grid.stats.ms1_peaks + grid.stats.ms2_peaks << " peaks; " << fmt(grid.stats.ingest) << mem() << std::endl;
      writeRunAsMzML(grid, in, out_mzml);
      return EXECUTION_OK;
    }
    // the instrument files: checked before the run is read (a wrong name or file fails at once)
    const std::string instrument_spec = S("instrument"), instrument_file = S("instrument_file");
    if (instrument_file.size() && lowerCase(instrument_spec) != "auto")
    {
      OPENMS_LOG_ERROR << "Give either -instrument or -instrument_file, not both." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    try { std::string w; ODIA::resolveInstrument(instrument_spec, "", instrument_file, w); }
    catch (const std::exception& e) { OPENMS_LOG_ERROR << "Error: " << e.what() << std::endl; return ILLEGAL_PARAMETERS; }
    const int threads = threadCount_();
    omp_set_num_threads(threads);
    OPENMS_LOG_INFO << "Threads: " << threads << (threads_given_ ? "" : " (the physical cores; -threads sets the number)") << std::endl;

    // --- library: opened as a stream of parts (read concurrently with extraction below)
    const ODIA::StageTime t_lib0 = ODIA::stageClock();
    ODIA::LibraryLoadOptions lopt;
    lopt.top_fragments = static_cast<std::size_t>(I("top_fragments"));
    lopt.targets_only = F("targets_only");
    lopt.threads = threads;
    if (!S("ids").empty()) { lopt.keep_ids = ODIA::readIdList(S("ids")); }
    ODIA::LibraryReader library(lib_file, lopt, 65536);   // rows per streamed part; fixed, so the output does not depend on -threads
    const ODIA::StageTime t_lib = ODIA::stageClock() - t_lib0;
    OPENMS_LOG_INFO << "Library: " << library.rows() << " rows";
    if (g_debug) { OPENMS_LOG_INFO << " in " << library.parts() << " parts from " << library.sources() << " source(s), streamed during extraction; open " << fmt(t_lib) << mem(); }
    OPENMS_LOG_INFO << std::endl;
    if (!lopt.keep_ids.empty())
    {
      // the rows a precursor list kept (a library holds each precursor once)
      const std::size_t n_ids = lopt.keep_ids.size();
      if (library.rows() == 0)
      {
        OPENMS_LOG_ERROR << "None of the " << n_ids << " ids of -ids is in the library (ids are the library's precursor ids, "
                            "e.g. AAAFEGELIPASQIDR2 for DIA-NN)." << std::endl;
        return INPUT_FILE_EMPTY;
      }
      if (library.rows() < n_ids)
      {
        OPENMS_LOG_WARN << "WARNING: " << n_ids - library.rows() << " of the " << n_ids << " ids of -ids are not in the library"
                        << (F("targets_only") ? " (or are decoys, skipped by -targets_only)" : "") << "." << std::endl;
      }
    }
    if (library.rows() == 0) { OPENMS_LOG_ERROR << "Library is empty." << std::endl; return INPUT_FILE_EMPTY; }

    // --- calibration panel (needs only the library), loaded on its own thread while the run is ingested
    // RT map: a given anchors or trafoXML file selects that map; otherwise -rt_mode
    std::string mode = S("rt_mode");
    const std::string rt_anchors = S("rt_anchors"), rt_trafo = S("rt_trafo");
    if (!rt_anchors.empty() && !rt_trafo.empty()) { OPENMS_LOG_ERROR << "Give either -rt_anchors or -rt_trafo, not both." << std::endl; return ILLEGAL_PARAMETERS; }
    if (!rt_anchors.empty() || !rt_trafo.empty())
    {
      if (mode != "auto") { OPENMS_LOG_ERROR << "-rt_anchors / -rt_trafo give the RT map; they cannot be combined with -rt_mode " << mode << "." << std::endl; return ILLEGAL_PARAMETERS; }
      mode = rt_anchors.empty() ? "trafo" : "anchors";
    }
    const double rt_window_opt = D("rt_window");
    const bool rt_needed = rt_window_opt >= 0.0;   // a whole-run extraction needs no RT map
    const bool ms1_enabled = !F("no_ms1");
    const double mz_tol = D("mz_tol_ms2"), mz_tol_ms1 = D("mz_tol_ms1");
    const bool scheme_b = S("scheme") == "B";
    const bool offsets_given = D("mz_offset_ms2") != 0.0 || D("mz_offset_ms1") != 0.0;
    if (scheme_b && (mz_tol <= 0 || (ms1_enabled && mz_tol_ms1 <= 0)))
    {
      OPENMS_LOG_ERROR << "-scheme B (OpenSWATH's rules) extracts without mass correction and needs fixed tolerances: give -mz_tol_ms2"
                       << (ms1_enabled ? " and -mz_tol_ms1" : "") << " (ppm)." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    if (scheme_b && offsets_given) { OPENMS_LOG_ERROR << "-scheme B extracts without mass correction; -mz_offset_ms2/-mz_offset_ms1 do not apply." << std::endl; return ILLEGAL_PARAMETERS; }
    const bool mass_auto = !scheme_b && S("mz_calibration") == "auto";
    if (!scheme_b && !mass_auto && (mz_tol <= 0 || (ms1_enabled && mz_tol_ms1 <= 0)))
    {
      OPENMS_LOG_ERROR << "-mz_calibration off needs fixed tolerances (measured widths assume the measured correction): give "
                          "-mz_tol_ms2" << (ms1_enabled ? " and -mz_tol_ms1" : "") << " (ppm)." << std::endl;
      return ILLEGAL_PARAMETERS;
    }
    if (mass_auto && offsets_given) { OPENMS_LOG_ERROR << "-mz_offset_ms2/-mz_offset_ms1 replace the measured correction: give them with -mz_calibration off." << std::endl; return ILLEGAL_PARAMETERS; }
    if (scheme_b) { OPENMS_LOG_INFO << "Scheme B: sum of the centroids in the window, MS1 isotopes at unit spacing, every window containing the precursor, no mass correction." << std::endl; }
    const bool rt_auto = mode == "auto" && rt_needed;
    const int ingest_threads = std::min(threads, 32);   // the parse is memory-bandwidth bound beyond ~32 threads
    struct Panel { ODIA::Library lib; std::size_t keep_every = 1; ODIA::StageTime time; };
    std::future<Panel> panel_future;
    std::uint64_t all_rows = 0;   // the whole library (also with -ids): the calibration panel is drawn from all of it
    // Experiment (results/decoys/): ODIA_ANCHOR_FDR accepts anchors at a decoy-calibrated false rate, scoring the library's
    // own peptide decoys when it has them. Not adopted as a default: at 1% too few anchors survive on timsTOF, at 5%
    // the timsTOF mass width widens, and on Astral the cleaner anchors understate the RT scatter (window coverage 96%).
    const bool decoy_experiment = std::getenv("ODIA_ANCHOR_FDR") != nullptr;
    if (rt_auto || mass_auto)
    {
      // The panel is drawn from the whole library (never from an -ids subset: calibrating on externally
      // confirmed precursors would not be self-calibration), from evenly spaced rows so the library is
      // not loaded whole: about twice the panel size, from which calibrate() stratifies by RT.
      // Row count of the whole library: `library` is already restricted when -ids is given to a non-compact format.
      all_rows = lopt.keep_ids.empty() ? library.rows() : ODIA::LibraryReader(lib_file, ODIA::LibraryLoadOptions{}).rows();
      const auto panel_size = static_cast<std::uint64_t>(I("calibration:panel") > 0
          ? I("calibration:panel")
          : std::max(1.0, ODIA::CalibrationOptions{}.panel_fraction * static_cast<double>(all_rows)));
      const std::size_t keep_every = static_cast<std::size_t>(std::max<std::uint64_t>(1, all_rows / (2 * panel_size)));
      const int panel_threads = std::max(std::min(4, threads), threads - ingest_threads);
      panel_future = std::async(std::launch::async, [lib_file, keep_every, panel_threads, decoy_experiment] {
        const ODIA::StageTime t0 = ODIA::stageClock();
        omp_set_num_threads(panel_threads);
        Panel p;
        ODIA::LibraryLoadOptions popt;
        popt.top_fragments = 0;   // all fragments: the anchor search uses the top 6, the tolerance curve all of them
        popt.targets_only = !decoy_experiment;   // the library's decoys only for the decoy-calibrated experiment
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
                    << gs.ms1_spectra << " MS1 / " << gs.ms2_spectra << " MS2 spectra";
    if (g_debug)
    {
      OPENMS_LOG_INFO << ", " << gs.ms1_peaks + gs.ms2_peaks << " peaks, " << gs.missing_cells << " empty cells, "
                      << (gs.overlapping_windows ? std::to_string(gs.overlapping_windows) + " overlapping window pairs, " : std::string())
                      << grid.bytes() / double(1u << 20) << " MiB";
    }
    OPENMS_LOG_INFO << "; " << fmt(gs.ingest) << mem() << std::endl;

    if (!out_mzml.empty())
    {
      writeRunAsMzML(grid, in, out_mzml);
      if (out.empty() && out_tsv.empty()) { return EXECUTION_OK; }
    }

    // --- instrument defaults: the option given > the instrument's file > default.yaml, per value
    std::string instrument_warning;
    const ODIA::InstrumentDefaults inst = ODIA::resolveInstrument(instrument_spec, grid.instrument, instrument_file, instrument_warning);
    std::vector<std::array<std::string, 3>> sources;   // value name, value, source (logged, footer, -out_stats)
    const auto num = [](double x) { std::ostringstream o; o << x; return o.str(); };
    const auto multiple = [&](const char* key) {
      const double given = D(key);
      const double v = given > 0.0 ? given : inst.value(key);
      sources.push_back({key, num(v), given > 0.0 ? "given" : inst.source(key)});
      return v;
    };
    const double sigma_ms2 = multiple("mz_accuracy_ms2");
    const double sigma_ms1 = multiple("mz_accuracy_ms1");
    const double rt_window_sigma = inst.value("rt_window_sigma"), rt_window_fwhm = inst.value("rt_window_fwhm");
    sources.push_back({"rt_window_sigma", num(rt_window_sigma), inst.source("rt_window_sigma")});
    sources.push_back({"rt_window_fwhm", num(rt_window_fwhm), inst.source("rt_window_fwhm")});
    OPENMS_LOG_INFO << "Instrument: " << inst.instrument << ", " << inst.how << "; defaults " << inst.filesText() << std::endl;
    {
      // the values an instrument file implied, and how to change them
      std::string implied;
      if (D("mz_accuracy_ms2") <= 0.0 && mz_tol <= 0.0) { implied += "fragment tolerance " + num(sigma_ms2) + " x the measured width"; }
      if (D("mz_accuracy_ms1") <= 0.0 && mz_tol_ms1 <= 0.0 && ms1_enabled) { implied += (implied.empty() ? "" : ", ") + std::string("MS1 ") + num(sigma_ms1) + " x"; }
      if (!implied.empty()) { OPENMS_LOG_INFO << inst.instrument << " defaults: " << implied << " (-mz_accuracy_ms2 / -mz_accuracy_ms1 change the multiples, -mz_tol_ms2 / -mz_tol_ms1 fix the tolerances)" << std::endl; }
    }
    if (!instrument_warning.empty()) { OPENMS_LOG_WARN << "WARNING: " << instrument_warning << std::endl; }
    if (debug_level_ > 0)
    {
      OPENMS_LOG_INFO << "Instrument files: " << inst.directory << " (searched:";
      for (const auto& d : ODIA::instrumentDirectories()) { OPENMS_LOG_INFO << " " << d; }
      for (const auto& [label, d] : ODIA::userInstrumentDirectories()) { OPENMS_LOG_INFO << "; " << label << " " << d; }
      OPENMS_LOG_INFO << "); " << inst.filesProvenance() << std::endl;
    }

    // --- calibration (from the run itself) and RT map
    std::optional<ODIA::CalibrationResult> cal;
    if (panel_future.valid())
    {
      const ODIA::StageTime t_wait0 = ODIA::stageClock();
      Panel panel = panel_future.get();
      ODIA::Library& panel_lib = panel.lib;
      if (g_debug)
      {
        OPENMS_LOG_INFO << "Calibration panel library: every " << panel.keep_every << ". row, " << panel_lib.size()
                        << " targets; " << fmt(panel.time) << " (concurrent with ingest; waited "
                        << ODIA::stageClock().wall - t_wait0.wall << " s)" << mem() << std::endl;
      }
      const ODIA::Library* cal_lib = &panel_lib;
      ODIA::CalibrationOptions copt;
      // The absolute panel size (calibration:panel, else panel_fraction of the whole library): the panel library
      // above already holds only ~2x that many rows, so calibrate() must not apply the fraction again.
      copt.panel = static_cast<std::size_t>(I("calibration:panel") > 0
          ? I("calibration:panel")
          : std::max(1.0, copt.panel_fraction * static_cast<double>(all_rows)));
      copt.search_ppm = D("calibration:search_ppm");
      copt.accuracy_sigma = sigma_ms2;
      copt.ms1_sigma = sigma_ms1;
      copt.linear_mass_model = S("calibration:mz_model") == "linear";
      copt.lowess_span = D("calibration:lowess_span");
      if (const char* fdr = std::getenv("ODIA_ANCHOR_FDR")) { copt.anchor_fdr = std::atof(fdr); }   // experiment, see above
      copt.threads = threads;
      try { cal = ODIA::calibrate(*cal_lib, grid, copt); }
      catch (const std::exception& e)
      {
        // Too few anchors (a small file, a library with no peptides in the run). Without a measured RT map or
        // measured tolerances there is nothing to extract with; with explicit ones, go on uncorrected.
        std::string missing;
        if (rt_auto) { missing += " the RT map (-rt_mode auto; give -rt_mode minutes|seconds, -rt_anchors or -rt_trafo)"; }
        if (mz_tol <= 0) { missing += std::string(missing.empty() ? "" : ";") + " the fragment tolerance (give -mz_tol_ms2)"; }
        if (ms1_enabled && mz_tol_ms1 <= 0) { missing += std::string(missing.empty() ? "" : ";") + " the MS1 tolerance (give -mz_tol_ms1)"; }
        if (!missing.empty()) { OPENMS_LOG_ERROR << "Calibration failed: " << e.what() << ". Needed for:" << missing << "." << std::endl; return UNEXPECTED_RESULT; }
        OPENMS_LOG_WARN << "WARNING: calibration failed (" << e.what() << "): no mass correction; the given tolerances apply." << std::endl;
      }
      if (cal)
      {
      releaseFreedMemory();   // the per-window calibration tiles go back before extraction
      // one line at the default level (the timing last: analysis scripts read it), the search detail with -debug 1
      OPENMS_LOG_INFO << "Calibration: " << cal->anchors.size() << " anchors (" << cal->used << " used); RT residual p50/p95 "
                      << cal->rt_residual_p50 << " / " << cal->rt_residual_p95 << " s; peak FWHM " << cal->peak_fwhm_s << " s; MS2 offset "
                      << cal->ms2_offset_ppm << " ppm" << slopeText(cal->ms2_slope_ppm_per_th, cal->ms2_mz_ref) << " (width " << cal->ms2_sigma_ppm
                      << "), MS1 offset " << cal->ms1_offset_ppm << " ppm" << slopeText(cal->ms1_slope_ppm_per_th, cal->ms1_mz_ref)
                      << " (width " << cal->ms1_sigma_ppm << "); " << fmt(cal->time) << std::endl;
      if (g_debug)
      {
        OPENMS_LOG_INFO << "Calibration detail: stage 1 " << cal->stage1_anchors << " anchors (" << cal->stage1_used
                        << " used), stage 2 window +-" << cal->stage2_window_s << " s; panel " << cal->panel << "; RT residual |p99| "
                        << cal->rt_residual_p99 << " s; peak FWHM " << cal->peak_fwhm_cycles << " cycles, co-elution window +-"
                        << cal->half_window_cycles << " cycles; score cut " << cal->score_cut << " (" << cal->decoy_anchors << " decoys above it, "
                        << (cal->decoy_panel ? std::to_string(cal->decoy_panel) + " library decoys" : std::string("shifted-fragment decoys"))
                        << "), strict cut " << cal->strict_score_cut << "; accuracy MS2 +-" << cal->ms2_accuracy_ppm << " / MS1 +-"
                        << cal->ms1_accuracy_ppm << " ppm (coverage half widths " << cal->ms2_coverage_ppm << " / " << cal->ms1_coverage_ppm
                        << " ppm; = " << cal->accuracy_sigma << " sigma; co-elution by k:"
                        << [&] { std::string t; for (const auto& [k, r] : cal->coelution_by_sigma) { t += " " + std::to_string(static_cast<int>(k)) + ":" + (r < 0 ? std::string("-") : std::to_string(r).substr(0, 5)); } return t; }()
                        << "; from " << cal->strict_mass_anchors << " anchors; " << cal->search_rounds << " search passes, final windows +-"
                        << cal->ms2_search_ppm << " / +-" << cal->ms1_search_ppm << " ppm)" << std::endl;
      }
      if (!S("out_calibration").empty()) { ODIA::writeAnchors(S("out_calibration"), *cal_lib, *cal); }

      }
    }
    if (!cal && !S("out_calibration").empty()) { OPENMS_LOG_WARN << "WARNING: no calibration ran, so -out_calibration is not written." << std::endl; }
    ODIA::RtMap rt_map;
    if (mode == "seconds" || !rt_needed) { rt_map = ODIA::RtMap::identity(); }
    else if (mode == "minutes") { rt_map = ODIA::RtMap::identity(60.0); }
    else if (mode == "trafo") { rt_map = ODIA::RtMap::fromTrafoXML(rt_trafo, F("rt_trafo_invert")); }
    else if (mode == "anchors") { rt_map = ODIA::RtMap::fromAnchors(rt_anchors, "lowess"); }
    else { rt_map = std::move(cal->rt_map); }
    OPENMS_LOG_INFO << "RT map: " << rt_map.describe() << std::endl;
    // The run model: RT error and peak width along the gradient from the anchors, about the map in use (RunModel.h);
    // kept for downstream algorithms and recorded with the output. It does not change the extraction.
    ODIA::RunModel run_model;
    if (cal && rt_needed)
    {
      std::vector<double> mrt, mres, mfw;
      for (const auto& a : cal->anchors) { if (a.used) { mrt.push_back(a.run_rt); mres.push_back(a.run_rt - rt_map(a.library_rt)); mfw.push_back(a.fwhm_s); } }
      run_model = ODIA::estimateRunModel(mrt, mres, mfw);
      if (g_debug && !run_model.empty())
      {
        const auto range = [](const std::vector<double>& v) { std::ostringstream o; o.precision(3); o << *std::min_element(v.begin(), v.end()) << "-" << *std::max_element(v.begin(), v.end()); return o.str(); };
        OPENMS_LOG_INFO << "Run model: " << run_model.anchors << " anchors, " << run_model.neighbours << " per estimate; RT error scale "
                        << range(run_model.drt_scale) << " s, 99th percentile " << range(run_model.drt_tail99) << " s, peak FWHM "
                        << range(run_model.fwhm) << " s along " << run_model.rt.front() << "-" << run_model.rt.back() << " s" << std::endl;
      }
    }

    // --- extraction + export
    ODIA::ExtractOptions eopt;
    eopt.ms2_ppm = mz_tol;
    eopt.ms1_ppm = ms1_enabled ? mz_tol_ms1 : 1.0;   // no MS1 traces: the MS1 tolerance is not used
    if (eopt.ms2_ppm <= 0.0)
    {
      if (!cal) { OPENMS_LOG_ERROR << "The fragment tolerance is measured by the calibration, which is off (-mz_calibration off): give -mz_tol_ms2 <ppm>." << std::endl; return ILLEGAL_PARAMETERS; }
      eopt.ms2_ppm = cal->ms2_accuracy_ppm;
      if (eopt.ms2_ppm <= 0.0) { OPENMS_LOG_ERROR << "MS2 accuracy could not be measured (too few apex centroids); give -mz_tol_ms2 <ppm>." << std::endl; return UNEXPECTED_RESULT; }
      OPENMS_LOG_INFO << "MS2 tolerance: +-" << eopt.ms2_ppm << " ppm (" << cal->accuracy_sigma << " x the measured width)" << std::endl;
    }
    if (eopt.ms1_ppm <= 0.0)
    {
      if (!cal) { OPENMS_LOG_ERROR << "The MS1 tolerance is measured by the calibration, which is off (-mz_calibration off): give -mz_tol_ms1 <ppm> (or -no_ms1 for no MS1 traces)." << std::endl; return ILLEGAL_PARAMETERS; }
      eopt.ms1_ppm = cal->ms1_accuracy_ppm;
      if (eopt.ms1_ppm <= 0.0) { OPENMS_LOG_ERROR << "MS1 accuracy could not be measured (too few MS1 centroids); give -mz_tol_ms1 <ppm>." << std::endl; return UNEXPECTED_RESULT; }
      OPENMS_LOG_INFO << "MS1 tolerance: +-" << eopt.ms1_ppm << " ppm (" << sigma_ms1 << " x the measured width)" << std::endl;
    }
    eopt.ms2_ppm_offset = mass_auto && cal ? cal->ms2_offset_ppm : D("mz_offset_ms2");
    eopt.ms1_ppm_offset = mass_auto && cal ? cal->ms1_offset_ppm : D("mz_offset_ms1");
    if (mass_auto && cal)
    {
      eopt.ms2_ppm_slope = cal->ms2_slope_ppm_per_th; eopt.ms2_mz_ref = cal->ms2_mz_ref;
      eopt.ms1_ppm_slope = cal->ms1_slope_ppm_per_th; eopt.ms1_mz_ref = cal->ms1_mz_ref;
    }
    eopt.rt_half_window = rt_window_opt;
    std::string rt_window_text = std::to_string(rt_window_opt);
    if (eopt.rt_half_window == 0.0)
    {
      // Measured, RT-dependent: 7 x the robust sigma (1.4826 MAD) of the calibration anchors' residuals about the RT map
      // in use, plus 2 x their median peak FWHM, per RT bin of ~150 anchors (at most 8), linear in between. The robust
      // sigma is not biased by the calibration's RT search window (which truncates the anchors' tails), but the
      // residuals have heavy tails: on all ten benchmark configurations (Astral runs 01-03 with tuned and predicted
      // libraries, timsTOF A01/B01) 7 x contained >= 99.4% of DIA-NN's whole peaks (median 99.99%); 4 x fell to 94.7%,
      // 6 x to 98.1%, fixed +-60 s to 79% (results/rtwin/all/).
      std::vector<std::array<double, 3>> pts;   // run RT, residual, FWHM
      if (cal) { for (const auto& a : cal->anchors) { if (a.used) { pts.push_back({a.run_rt, a.run_rt - rt_map(a.library_rt), a.fwhm_s}); } } }
      if (const char* table = std::getenv("ODIA_RT_WINDOW_TABLE"))
      {
        // Tests only (tests/CMakeLists.txt, rt_window_table): the RT-dependent window as "rt:half,rt:half,..." (s),
        // so that a small file without calibration anchors gets one
        std::istringstream is(table);
        std::string item;
        while (std::getline(is, item, ','))
        {
          const auto colon = item.find(':');
          if (colon == std::string::npos) { OPENMS_LOG_ERROR << "ODIA_RT_WINDOW_TABLE: expected rt:half,... (s)" << std::endl; return ILLEGAL_PARAMETERS; }
          eopt.rt_window_at.push_back(std::stod(item.substr(0, colon)));
          eopt.rt_window_half.push_back(std::stod(item.substr(colon + 1)));
        }
        eopt.rt_half_window = *std::max_element(eopt.rt_window_half.begin(), eopt.rt_window_half.end());
        rt_window_text = std::string("ODIA_RT_WINDOW_TABLE ") + table;
        OPENMS_LOG_INFO << "RT window: " << rt_window_text << " (test hook)" << std::endl;
      }
      else if (pts.size() >= 50)
      {
        std::sort(pts.begin(), pts.end());
        const std::size_t nb = std::clamp<std::size_t>(pts.size() / 150, 1, 8);
        auto median = [](std::vector<double> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
        for (std::size_t b = 0; b < nb; ++b)
        {
          const std::size_t lo = b * pts.size() / nb, hi = (b + 1) * pts.size() / nb;
          std::vector<double> t, r, f;
          for (std::size_t i = lo; i < hi; ++i) { t.push_back(pts[i][0]); r.push_back(pts[i][1]); f.push_back(pts[i][2]); }
          const double rm = median(r);
          std::vector<double> dev; for (double x : r) { dev.push_back(std::fabs(x - rm)); }
          eopt.rt_window_at.push_back(median(t));
          eopt.rt_window_half.push_back(rt_window_sigma * 1.4826 * median(dev) + rt_window_fwhm * median(f));
        }
        const auto [mn, mx] = std::minmax_element(eopt.rt_window_half.begin(), eopt.rt_window_half.end());
        eopt.rt_half_window = *mx;   // > 0: not a whole-run extraction
        std::ostringstream os; os.precision(3);
        for (std::size_t b = 0; b < nb; ++b) { os << (b ? ", " : "") << eopt.rt_window_half[b] << " s at " << std::lround(eopt.rt_window_at[b]) << " s"; }
        rt_window_text = "measured: " + os.str();
        OPENMS_LOG_INFO << "RT window: +-" << *mn << " to +-" << *mx << " s, " << rt_window_sigma << " x the RT scatter + " << rt_window_fwhm << " x the peak FWHM of " << pts.size()
                        << " anchors in " << nb << " RT bin(s) (" << os.str() << ")" << std::endl;
      }
      else
      {
        eopt.rt_half_window = 60.0;
        rt_window_text = "60 (no calibration anchors to measure it)";
        OPENMS_LOG_WARN << "WARNING: RT window +-60 s: too few calibration anchors to measure the RT scatter (-rt_window sets it)." << std::endl;
      }
    }
    // where each resolved value came from (logged with -debug 1, in the footer and in -out_stats)
    sources.push_back({"mz_tol_ms2", num(eopt.ms2_ppm), mz_tol > 0.0 ? "given" : "measured"});
    sources.push_back({"mz_tol_ms1", ms1_enabled ? num(eopt.ms1_ppm) : "-", !ms1_enabled ? "unused (-no_ms1)" : mz_tol_ms1 > 0.0 ? "given" : "measured"});
    const std::string offset_source = scheme_b ? "none (-scheme B)" : mass_auto ? (cal ? "measured" : "none (no calibration)") : "given";
    sources.push_back({"mz_offset_ms2", num(eopt.ms2_ppm_offset), offset_source});
    sources.push_back({"mz_offset_ms1", num(eopt.ms1_ppm_offset), offset_source});
    sources.push_back({"rt_window", rt_window_opt != 0.0 ? num(rt_window_opt) : rt_window_text, rt_window_opt > 0.0 ? "given" : rt_window_opt < 0.0 ? "given (whole run)"
                                                     : eopt.rt_window_half.empty() ? "default (too few anchors to measure it)" : "measured"});
    sources.push_back({"rt_map", rt_map.describe(), mode == "auto" ? (rt_needed ? "measured" : "unused (whole run)") : "given (" + (mode == "anchors" || mode == "trafo" ? "-rt_" + mode : "-rt_mode " + mode) + ")"});
    if (debug_level_ > 0)
    {
      std::string t;
      for (const auto& v : sources) { t += (t.empty() ? "" : "; ") + v[0] + " " + v[1] + " (" + v[2] + ")"; }
      OPENMS_LOG_INFO << "Values used: " << t << std::endl;
    }
    eopt.aggregate_sum = scheme_b;
    eopt.extract_ms1 = ms1_enabled;
    eopt.ms1_isotopes = I("ms1_isotopes");
    eopt.assign_all_windows = scheme_b;
    eopt.threads = threads;
    eopt.isotope_unit_spacing = scheme_b;
    eopt.ms1_scan_axis = S("ms1_axis") == "scans";
    eopt.unit_precursors = static_cast<std::size_t>(std::max(1, I("unit_precursors")));
    // Trace buffer pool: 256 KiB blocks, pre-faulted once and recycled (no page faults or allocator churn
    // during extraction); a trace run longer than a block goes to the heap.
    constexpr std::size_t block_floats = 64 * 1024;
    const std::size_t pool_blocks = std::max<std::size_t>(16, static_cast<std::size_t>(I("buffer_mb")) * (1u << 20) / (block_floats * sizeof(float)));
    ODIA::BufferPool buffers(block_floats, pool_blocks);
    eopt.block_floats = block_floats;
    ODIA::Extractor extractor(rt_map, grid, eopt);

    ODIA::TraceWriter::Config wcfg;
    wcfg.parquet = out;
    wcfg.axes = out_axes;
    wcfg.tsv = out_tsv;
    wcfg.run_name = S("run_name");
    if (wcfg.run_name.empty()) { wcfg.run_name = std::filesystem::path(in).stem().string(); }
    wcfg.log8 = S("encoding") == "log8";
    wcfg.log8_decades = 6.0;
    wcfg.expand_rt = F("expand_rt");
    wcfg.skip_empty = F("skip_empty");
    wcfg.shards = I("write_parts");
    wcfg.isotope_unit_spacing = eopt.isotope_unit_spacing;
    wcfg.ms1_scan_axis = eopt.ms1_scan_axis;
    wcfg.metadata = {{"input", in}, {"library", lib_file}, {"rt_map", rt_map.describe()},
                     {"mz_tol_ppm", std::to_string(eopt.ms2_ppm)}, {"mz_tol_ms1_ppm", std::to_string(eopt.ms1_ppm)},
                     {"mz_offset_ppm", std::to_string(eopt.ms2_ppm_offset)}, {"mz_offset_ms1_ppm", std::to_string(eopt.ms1_ppm_offset)},
                     {"mz_offset_slope_ppm_per_th", std::to_string(eopt.ms2_ppm_slope)}, {"mz_offset_mz_ref", std::to_string(eopt.ms2_mz_ref)},
                     {"mz_offset_ms1_slope_ppm_per_th", std::to_string(eopt.ms1_ppm_slope)}, {"mz_offset_ms1_mz_ref", std::to_string(eopt.ms1_mz_ref)},
                     {"scheme", S("scheme")}, {"aggregation", eopt.aggregate_sum ? "sum" : "max"},
                     {"rt_window_half_s", rt_window_text},
                     {"ms1_mapping", eopt.ms1_scan_axis ? "raw MS1 scans (axes lane -2)" : "linear interpolation of MS1 scans at cycle_rt"},
                     {"run_model", run_model.empty() ? std::string() : run_model.json()},
                     {"instrument", inst.instrument}, {"instrument_run", grid.instrument}, {"instrument_files", inst.filesProvenance()},
                     {"value_sources", [&] { std::string t; for (const auto& v : sources) { t += (t.empty() ? "" : "; ") + v[0] + "=" + v[1] + " (" + v[2] + ")"; } return t; }()}};
    ODIA::TraceWriter writer(grid, wcfg);

    // One pool of workers, centrally scheduled (ODIA::ExtractionScheduler): read + plan library parts,
    // extract units into recycled pool buffers, write sinks; commits in a fixed order.
    std::uint64_t library_fragments = 0;
    ODIA::SchedulerOptions sopt;
    sopt.threads = threads;
    // units in flight (extracted, not yet committed): 4 per worker up to 64, and 2 per worker beyond, so that more
    // than 64 workers find units to extract (192 threads on 2 x EPYC 9654: 13.6 -> 11.8 s, as fast as 64 threads;
    // the trace pool still bounds their memory; results/scaling64/)
    sopt.ahead = std::max(std::min<std::size_t>(4 * static_cast<std::size_t>(threads), 64), 2 * static_cast<std::size_t>(threads));
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
      if (g_debug) { OPENMS_LOG_INFO << os.str() << std::endl; }
    }
    const ODIA::StageTime t_close0 = ODIA::stageClock();
    writer.close();
    const ODIA::StageTime t_close = ODIA::stageClock() - t_close0;
    const ODIA::StageTime t_total = ODIA::stageClock() - t_start;

    const auto& es = extractor.stats();
    OPENMS_LOG_INFO << "Extraction: " << es.precursors << " precursors, " << es.traces << " traces";
    if (es.without_window > 0) { OPENMS_LOG_INFO << " (" << es.without_window << " precursors in no isolation window)"; }
    if (g_debug)
    {
      OPENMS_LOG_INFO << "; " << library_fragments << " fragments from " << library.parts() << " library parts; " << es.assignments
                      << " precursor-window assignments in " << es.units << " units, " << es.empty_range << " outside the acquired RT range; "
                      << es.ms2_points << " MS2 + " << es.ms1_points << " MS1 points, " << es.peak_lookups << " lookups";
    }
    OPENMS_LOG_INFO << "; " << fmt(es.time) << " with " << threads << " threads" << mem() << std::endl;
    // A wrong RT scale (minutes read as seconds, iRT as minutes) extracts the wrong place without failing: the mapped
    // RTs then fall outside the run or bunch into a small part of it.
    {
      const double run_span = grid.cycle_rt.empty() ? 0.0 : grid.cycle_rt.back() - grid.cycle_rt.front();
      const double lib_span = es.rt_center_max - es.rt_center_min;
      if (es.precursors > 0 && (es.empty_range * 2 > es.precursors || (es.precursors >= 1000 && rt_needed && run_span > 0 && lib_span < 0.25 * run_span)))
      {
        OPENMS_LOG_WARN << "WARNING: the library's RTs map onto " << std::max(0.0, es.rt_center_min) << "-" << std::max(0.0, es.rt_center_max) << " s of a run spanning "
                        << grid.cycle_rt.front() << "-" << grid.cycle_rt.back() << " s (" << es.empty_range << " of " << es.precursors
                        << " precursors outside it): check -rt_mode (the library's RT scale); -rt_mode auto fits the map from the run." << std::endl;
      }
    }
    OPENMS_LOG_INFO << "Written: " << writer.rows() << " rows" << (out.empty() ? "" : " -> " + out);
    if (g_debug)
    {
      OPENMS_LOG_INFO << "; " << writer.points() << " points, " << writer.shardCount() << " Parquet part(s) written in parallel"
                      << (writer.shardCount() > 1 ? " into one file" : "") << " (busiest writer " << writer.busySeconds() << " s, final drain "
                      << t_close.wall << " s; appending to the file " << writer.mergeSeconds() << " s in total)";
    }
    OPENMS_LOG_INFO << std::endl;
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

    const std::string stats_file = S("out_stats");
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
         << "  \"mz_tol_ms1_ppm\": " << eopt.ms1_ppm << ",\n"
         << "  \"instrument\": {\"name\": \"" << escape(inst.instrument) << "\", \"run_metadata\": \"" << escape(grid.instrument)
         << "\", \"chosen\": \"" << escape(inst.how) << "\", \"files\": \"" << escape(inst.filesProvenance()) << "\"},\n"
         << "  \"run_model\": " << (run_model.empty() ? std::string("null") : run_model.json()) << ",\n"
         << "  \"values\": {";
      for (std::size_t i = 0; i < sources.size(); ++i)
      {
        js << (i ? ", " : "") << "\"" << sources[i][0] << "\": {\"value\": \"" << escape(sources[i][1]) << "\", \"source\": \"" << escape(sources[i][2]) << "\"}";
      }
      js << "},\n";
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
  hintOldNames(argc, argv);
  TOPPDIATraceExtractor tool;
  return tool.main(argc, argv);
}
