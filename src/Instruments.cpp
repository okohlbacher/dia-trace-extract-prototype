#include "Instruments.h"
#include "Platform.h"

#include <OpenMS/FORMAT/FileHandler.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ODIA
{
  namespace
  {
    namespace fs = std::filesystem;

    std::string lower(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
    }

    /// Name as matched by -instrument: case, ' ', '-', '_', a leading vendor prefix and a trailing model number
    /// ignored, and nothing more ("Thermo Orbitrap Astral" = "orbitrap-astral"; "Exploris 480" = "Exploris").
    std::string normalise(const std::string& name)
    {
      std::string s;
      for (char c : lower(name)) { if (c != ' ' && c != '-' && c != '_') { s += c; } }
      for (const char* v : {"thermofisherscientific", "thermofisher", "thermoscientific", "thermo", "brukerdaltonics", "bruker", "sciex", "waters", "agilent"})
      {
        const std::string p = v;
        if (s.size() > p.size() && s.compare(0, p.size(), p) == 0) { s = s.substr(p.size()); break; }
      }
      while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back()))) { s.pop_back(); }
      return s;
    }

    [[noreturn]] void fail(const std::string& path, const std::string& key, const std::string& msg)
    {
      throw std::runtime_error("instrument file " + path + (key.empty() ? "" : ", key '" + key + "'") + ": " + msg);
    }

    std::vector<std::string> stringList(const YAML::Node& n, const std::string& path, const std::string& key)
    {
      if (!n.IsSequence()) { fail(path, key, "expected a list of names, e.g. [a, b]"); }
      std::vector<std::string> out;
      for (const auto& x : n)
      {
        if (!x.IsScalar() || x.Scalar().empty()) { fail(path, key, "expected non-empty names"); }
        out.push_back(x.Scalar());
      }
      return out;
    }

    struct Range { double lo, hi; bool lo_open; };
    const std::map<std::string, Range>& ranges()
    {
      static const std::map<std::string, Range> r = {
        {"mz_accuracy_ms2", {0.0, 100.0, true}}, {"mz_accuracy_ms1", {0.0, 100.0, true}},
        {"rt_window_sigma", {0.0, 100.0, true}}, {"rt_window_fwhm", {0.0, 100.0, false}}};
      return r;
    }

    std::vector<InstrumentFile> loadDirectory(const std::string& dir)
    {
      std::vector<std::string> paths;
      for (const auto& e : fs::directory_iterator(dir))
      {
        if (e.is_regular_file() && e.path().extension() == ".yaml") { paths.push_back(e.path().string()); }
      }
      std::sort(paths.begin(), paths.end());
      std::vector<InstrumentFile> files;
      for (const auto& p : paths) { files.push_back(readInstrumentFile(p, fs::path(p).filename() == "default.yaml")); }
      return files;
    }

    std::string stem(const InstrumentFile& f) { return fs::path(f.file).stem().string(); }

    /// The file `ref` names (its stem or its name), or nullptr.
    const InstrumentFile* byRef(const std::vector<InstrumentFile>& files, const std::string& ref)
    {
      for (const auto& f : files) { if (stem(f) != "default" && (normalise(stem(f)) == normalise(ref) || normalise(f.name) == normalise(ref))) { return &f; } }
      return nullptr;
    }

    /// The file whose name or alias is `name`, or nullptr.
    const InstrumentFile* byName(const std::vector<InstrumentFile>& files, const std::string& name)
    {
      const std::string n = normalise(name);
      if (n.empty()) { return nullptr; }
      for (const auto& f : files)
      {
        if (stem(f) == "default") { continue; }
        if (normalise(f.name) == n || normalise(stem(f)) == n) { return &f; }
        for (const auto& a : f.aliases) { if (normalise(a) == n) { return &f; } }
      }
      return nullptr;
    }

    std::vector<const InstrumentFile*> chainOf(const std::vector<InstrumentFile>& files, const InstrumentFile* f)
    {
      std::vector<const InstrumentFile*> chain;
      while (f != nullptr)
      {
        if (std::find(chain.begin(), chain.end(), f) != chain.end()) { fail(f->path, "extends", "the files extend each other in a cycle"); }
        chain.push_back(f);
        if (f->extends.empty()) { break; }
        const InstrumentFile* next = byRef(files, f->extends);
        if (next == nullptr) { fail(f->path, "extends", "no instrument file '" + f->extends + "'"); }
        f = next;
      }
      return chain;
    }

    /// The most specific file whose keywords occur in `instrument` (the deepest extends chain; then the longest
    /// keyword), or nullptr.
    const InstrumentFile* classify(const std::vector<InstrumentFile>& files, const std::string& instrument)
    {
      const std::string s = lower(instrument);
      const InstrumentFile* best = nullptr;
      std::size_t best_depth = 0, best_len = 0;
      for (const auto& f : files)
      {
        std::size_t len = 0;
        for (const auto& k : f.keywords) { if (!k.empty() && s.find(lower(k)) != std::string::npos) { len = std::max(len, k.size()); } }
        if (len == 0) { continue; }
        const std::size_t depth = chainOf(files, &f).size();
        if (best == nullptr || depth > best_depth || (depth == best_depth && len > best_len)) { best = &f; best_depth = depth; best_len = len; }
      }
      return best;
    }

    std::vector<InstrumentFile> loadShipped(std::string& dir)
    {
      for (const auto& d : instrumentDirectories())
      {
        std::error_code ec;
        if (fs::is_regular_file(fs::path(d) / "default.yaml", ec)) { dir = d; return loadDirectory(d); }
      }
      std::string searched;
      for (const auto& d : instrumentDirectories()) { searched += (searched.empty() ? "" : ", ") + d; }
      throw std::runtime_error("the instrument files (default.yaml) were not found; searched: " + searched);
    }
  }

  const std::vector<std::string>& instrumentValueKeys()
  {
    static const std::vector<std::string> keys = {"mz_accuracy_ms2", "mz_accuracy_ms1", "rt_window_sigma", "rt_window_fwhm"};
    return keys;
  }

  std::vector<std::string> instrumentDirectories()
  {
    std::vector<std::string> dirs;
    const std::string exe = platform::executablePath();
    if (!exe.empty())
    {
      // <prefix>/bin/DIATraceExtractor or <prefix>/libexec/DIATraceExtractor (the release layout); a build
      // directory has no share/ next to it and falls through to the source tree
      dirs.push_back((fs::path(exe).parent_path().parent_path() / "share" / "DIATraceExtractor" / "instruments").string());
    }
#ifdef ODIA_SOURCE_DIR
    dirs.push_back((fs::path(ODIA_SOURCE_DIR) / "data" / "instruments").string());
#endif
    return dirs;
  }

  InstrumentFile readInstrumentFile(const std::string& path, bool is_default)
  {
    InstrumentFile f;
    f.path = path;
    f.file = fs::path(path).filename().string();
    YAML::Node root;
    try { root = YAML::LoadFile(path); }
    catch (const YAML::BadFile&) { fail(path, "", "cannot be read"); }
    catch (const YAML::Exception& e) { fail(path, "", std::string("not valid YAML (") + e.what() + ")"); }
    if (!root.IsMap()) { fail(path, "", "expected a map of keys (name: ..., mz_accuracy_ms2: ...)"); }
    std::set<std::string> seen;
    for (const auto& kv : root)
    {
      const std::string key = kv.first.as<std::string>();
      if (!seen.insert(key).second) { fail(path, key, "given twice"); }
      const YAML::Node& v = kv.second;
      if (key == "name")
      {
        if (!v.IsScalar() || v.Scalar().empty()) { fail(path, key, "expected a non-empty name"); }
        f.name = v.Scalar();
      }
      else if (key == "extends" && !is_default)
      {
        if (!v.IsScalar() || v.Scalar().empty()) { fail(path, key, "expected the name of another instrument file"); }
        f.extends = v.Scalar();
      }
      else if (key == "aliases" && !is_default) { f.aliases = stringList(v, path, key); }
      else if (key == "keywords" && !is_default) { f.keywords = stringList(v, path, key); }
      else if (const auto r = ranges().find(key); r != ranges().end())
      {
        double x = 0;
        if (!v.IsScalar()) { fail(path, key, "expected a number"); }
        try { std::size_t used = 0; x = std::stod(v.Scalar(), &used); if (used != v.Scalar().size()) { throw std::invalid_argument(""); } }
        catch (const std::exception&) { fail(path, key, "expected a number, not '" + v.Scalar() + "'"); }
        const Range& rg = r->second;
        if (!(rg.lo_open ? x > rg.lo : x >= rg.lo) || !(x <= rg.hi))
        {
          std::ostringstream o; o << x << " is out of range (" << (rg.lo_open ? "> " : ">= ") << rg.lo << ", <= " << rg.hi << ")";
          fail(path, key, o.str());
        }
        f.values[key] = x;
      }
      else
      {
        std::string allowed = "name, ";
        if (!is_default) { allowed += "extends, aliases, keywords, "; }
        for (const auto& k : instrumentValueKeys()) { allowed += k + (k == instrumentValueKeys().back() ? "" : ", "); }
        fail(path, key, "unknown key (allowed: " + allowed + ")");
      }
    }
    if (f.name.empty()) { fail(path, "name", "missing"); }
    if (is_default)
    {
      for (const auto& k : instrumentValueKeys()) { if (f.values.count(k) == 0) { fail(path, k, "missing (default.yaml sets every value)"); } }
    }
    f.sha1 = OpenMS::FileHandler::computeFileHash(path);
    return f;
  }

  InstrumentDefaults resolveInstrument(const std::string& spec, const std::string& run_instrument,
                                       const std::string& instrument_file, std::string& warning)
  {
    warning.clear();
    InstrumentDefaults d;
    std::vector<InstrumentFile> files = loadShipped(d.directory);
    // names must be unambiguous
    {
      std::map<std::string, std::string> owner;
      for (const auto& f : files)
      {
        if (stem(f) == "default") { continue; }
        std::vector<std::string> names = f.aliases;
        names.push_back(f.name);
        for (const auto& n : names)
        {
          const auto [it, fresh] = owner.emplace(normalise(n), f.file);
          if (!fresh && it->second != f.file) { fail(f.path, "aliases", "'" + n + "' also names " + it->second); }
        }
      }
    }
    const auto def = std::find_if(files.begin(), files.end(), [](const InstrumentFile& f) { return f.file == "default.yaml"; });
    const InstrumentFile default_file = *def;
    files.erase(def);

    if (!instrument_file.empty())
    {
      InstrumentFile own = readInstrumentFile(instrument_file, false);
      // a file of the same name replaces the shipped one for this run
      files.erase(std::remove_if(files.begin(), files.end(), [&](const InstrumentFile& f) { return normalise(f.name) == normalise(own.name); }), files.end());
      files.push_back(std::move(own));
    }
    // (pointers into `files` from here on: it is not modified any more)
    const InstrumentFile* chosen = nullptr;
    const InstrumentFile* from_run = run_instrument.empty() ? nullptr : classify(files, run_instrument);
    const std::string run_text = run_instrument.empty() ? std::string("none in the run's metadata") : "'" + run_instrument + "' in the run's metadata";
    if (!instrument_file.empty())
    {
      chosen = &files.back();
      d.how = "-instrument_file " + instrument_file;
    }
    else if (lower(spec) != "auto" && !spec.empty())
    {
      chosen = byName(files, spec);
      if (chosen == nullptr) { chosen = classify(files, spec); }
      if (chosen == nullptr) { throw std::runtime_error("unknown instrument '" + spec + "' (-instrument): use auto or one of " + knownInstruments() + "; other instruments: -instrument_file"); }
      d.how = "-instrument " + spec;
    }
    else
    {
      chosen = from_run;
      d.how = "from the run's metadata (" + (run_instrument.empty() ? std::string("none") : "'" + run_instrument + "'") + ")";
    }
    if (chosen != nullptr)
    {
      for (const InstrumentFile* f : chainOf(files, chosen)) { d.chain.push_back(*f); }
      d.instrument = chosen->name;
      // a named instrument that contradicts the run's metadata
      if (from_run != nullptr && d.how.rfind("-instrument", 0) == 0)
      {
        const auto run_chain = chainOf(files, from_run);
        const bool related = std::any_of(d.chain.begin(), d.chain.end(), [&](const InstrumentFile& f) { return f.file == from_run->file; })
                          || std::any_of(run_chain.begin(), run_chain.end(), [&](const InstrumentFile* f) { return f->file == chosen->file; });
        if (!related) { warning = "the instrument given (" + chosen->name + ") differs from " + run_text + " (" + from_run->name + "); using " + chosen->name + "."; }
      }
    }
    else
    {
      std::ostringstream o;
      o << "instrument not recognised (" << run_text << "): the generic defaults (fragment tolerance "
        << default_file.values.at("mz_accuracy_ms2") << " x the measured width); -instrument names it (" << knownInstruments() << ").";
      warning = o.str();
    }
    d.chain.push_back(default_file);
    return d;
  }

  std::string knownInstruments()
  {
    std::string dir;
    std::string out;
    for (const auto& f : loadShipped(dir)) { if (stem(f) != "default") { out += (out.empty() ? "" : ", ") + f.name; } }
    return out;
  }

  double InstrumentDefaults::value(const std::string& key) const
  {
    for (const auto& f : chain) { if (const auto it = f.values.find(key); it != f.values.end()) { return it->second; } }
    throw std::runtime_error("no instrument default for '" + key + "'");
  }

  std::string InstrumentDefaults::source(const std::string& key) const
  {
    for (const auto& f : chain)
    {
      if (f.values.count(key) != 0) { return f.file == "default.yaml" ? std::string("default") : "instrument-default:" + f.name; }
    }
    return "default";
  }

  std::string InstrumentDefaults::filesText() const
  {
    std::string t;
    for (std::size_t i = 0; i < chain.size(); ++i)
    {
      t += (i ? " over " : "") + chain[i].file;
      if (i == 0 && chain[i].file != "default.yaml") { t += " (" + chain[i].name + ")"; }
    }
    return t;
  }

  std::string InstrumentDefaults::filesProvenance() const
  {
    std::string t;
    for (const auto& f : chain) { t += (t.empty() ? "" : "; ") + f.file + " sha1:" + f.sha1; }
    return t;
  }
}
