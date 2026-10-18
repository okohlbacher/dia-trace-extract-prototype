#pragma once
// Instrument defaults as data files (data/instruments/, shipped as share/DIATraceExtractor/instruments/): default.yaml
// holds every default; one file per instrument or instrument class holds only what differs and may extend another
// (astral.yaml extends orbitrap.yaml). Precedence per value: the option given > the instrument's file > the files it
// extends > default.yaml.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ODIA
{
  struct InstrumentFile
  {
    std::string path;                       ///< as read (UTF-8)
    std::string file;                       ///< file name (astral.yaml)
    std::string name;                       ///< Astral
    std::string extends;                    ///< the file (stem or name) this one extends; "" for none
    std::vector<std::string> aliases;       ///< further names for -instrument
    std::vector<std::string> keywords;      ///< matched in the run's instrument name (ignoring case)
    std::map<std::string, double> values;   ///< mz_accuracy_ms2, mz_accuracy_ms1, rt_window_sigma, rt_window_fwhm, priority
    std::string sha1;                       ///< content hash (provenance only)
  };

  /// The defaults that apply to one run.
  struct InstrumentDefaults
  {
    std::string instrument = "default";     ///< the resolved instrument (the most specific file's name)
    std::string how;                        ///< how it was chosen, for the log
    std::vector<InstrumentFile> chain;      ///< most specific first, default.yaml last
    std::string directory;                  ///< where the files were found

    double value(const std::string& key) const;
    /// "instrument-default:Astral" or "default" (the file that supplied the value).
    std::string source(const std::string& key) const;
    /// "astral.yaml (Astral) over orbitrap.yaml over default.yaml"
    std::string filesText() const;
    /// "astral.yaml sha1:...; orbitrap.yaml sha1:...; default.yaml sha1:..."
    std::string filesProvenance() const;
  };

  /// The keys an instrument file may set (besides name, extends, aliases, keywords).
  const std::vector<std::string>& instrumentValueKeys();

  /// The directories searched for the instrument files, in order: next to the executable
  /// (<prefix>/share/DIATraceExtractor/instruments), then the source tree's data/instruments.
  std::vector<std::string> instrumentDirectories();

  /// Reads and validates one file (throws std::runtime_error naming the file and the key).
  InstrumentFile readInstrumentFile(const std::filesystem::path& file, bool is_default);

  /// Resolves the defaults for a run: `spec` is "auto" (from `run_instrument`, the run's metadata) or a name;
  /// `instrument_file` (may be empty) is a file used for this run on top of the shipped ones. `warning` receives a
  /// message for the log ("" when none). Throws std::runtime_error for unusable files or an unknown name.
  InstrumentDefaults resolveInstrument(const std::string& spec, const std::string& run_instrument,
                                       const std::string& instrument_file, std::string& warning);

  /// The names -instrument accepts (file names and aliases), for the help and errors.
  std::string knownInstruments();
}
