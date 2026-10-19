#include "Instruments.h"
#include "Platform.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ODIA
{
  namespace
  {
    namespace fs = std::filesystem;

    /// Paths inside the tool are UTF-8 strings; on Windows a narrow std::filesystem::path would use the ANSI code page.
    fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
    std::string toUtf8(const fs::path& p) { const std::u8string u = p.u8string(); return std::string(u.begin(), u.end()); }

    /// SHA-1 of a byte string (FIPS 180-4), for the provenance of the files' contents.
    std::string sha1Hex(const std::string& data)
    {
      std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
      std::string m = data;
      const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
      m += static_cast<char>(0x80);
      while (m.size() % 64 != 56) { m += '\0'; }
      for (int i = 7; i >= 0; --i) { m += static_cast<char>((bits >> (8 * i)) & 0xff); }
      const auto rol = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
      for (std::size_t off = 0; off < m.size(); off += 64)
      {
        std::array<std::uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i)
        {
          w[i] = (std::uint32_t(static_cast<unsigned char>(m[off + 4 * i])) << 24) | (std::uint32_t(static_cast<unsigned char>(m[off + 4 * i + 1])) << 16)
               | (std::uint32_t(static_cast<unsigned char>(m[off + 4 * i + 2])) << 8) | std::uint32_t(static_cast<unsigned char>(m[off + 4 * i + 3]));
        }
        for (int i = 16; i < 80; ++i) { w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1); }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
          const std::uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
          const std::uint32_t k = i < 20 ? 0x5A827999u : i < 40 ? 0x6ED9EBA1u : i < 60 ? 0x8F1BBCDCu : 0xCA62C1D6u;
          const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
          e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
      }
      std::ostringstream o;
      for (std::uint32_t x : h) { o << std::hex << std::setw(8) << std::setfill('0') << x; }
      return o.str();
    }

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
        {"rt_window_sigma", {0.0, 100.0, true}}, {"rt_window_fwhm", {0.0, 100.0, false}}, {"priority", {-100.0, 100.0, false}}};
      return r;
    }

    std::vector<InstrumentFile> loadDirectory(const std::string& dir)
    {
      std::vector<fs::path> paths;
      for (const auto& e : fs::directory_iterator(fromUtf8(dir)))
      {
        if (e.is_regular_file() && e.path().extension() == ".yaml") { paths.push_back(e.path()); }
      }
      std::sort(paths.begin(), paths.end());
      std::vector<InstrumentFile> files;
      for (const auto& p : paths) { files.push_back(readInstrumentFile(p, p.filename() == "default.yaml")); }
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

    /// The file whose keywords occur in `instrument`: the highest priority (the order of the class rule of 0.2.16:
    /// timsTOF before the Orbitraps before other TOFs), then the most specific (the deepest extends chain), then the
    /// longest keyword; nullptr when none matches.
    const InstrumentFile* classify(const std::vector<InstrumentFile>& files, const std::string& instrument)
    {
      const std::string s = lower(instrument);
      const InstrumentFile* best = nullptr;
      std::size_t best_depth = 0, best_len = 0;
      double best_priority = 0;
      for (const auto& f : files)
      {
        std::size_t len = 0;
        for (const auto& k : f.keywords) { if (!k.empty() && s.find(lower(k)) != std::string::npos) { len = std::max(len, k.size()); } }
        if (len == 0) { continue; }
        const std::size_t depth = chainOf(files, &f).size();
        const double priority = f.values.count("priority") ? f.values.at("priority") : 0.0;
        if (best == nullptr || priority > best_priority || (priority == best_priority && (depth > best_depth || (depth == best_depth && len > best_len))))
        {
          best = &f; best_depth = depth; best_len = len; best_priority = priority;
        }
      }
      return best;
    }

    /// The shipped files, then the user's directories over them: a file of the same name replaces the shipped one
    /// (the environment variable's directory over the user directory).
    std::vector<InstrumentFile> loadShipped(std::string& dir);
    std::vector<InstrumentFile> loadAll(std::string& dir)
    {
      std::vector<InstrumentFile> files = loadShipped(dir);
      auto user = userInstrumentDirectories();
      std::reverse(user.begin(), user.end());   // lowest precedence first: later ones replace
      for (const auto& [origin, d] : user)
      {
        std::error_code ec;
        if (!fs::is_directory(fromUtf8(d), ec))
        {
          if (origin.front() == '$') { throw std::runtime_error(origin + " is set but " + d + " is not a directory"); }
          continue;   // the user directory is optional
        }
        for (InstrumentFile& f : loadDirectory(d))
        {
          f.origin = origin;
          const auto same = std::find_if(files.begin(), files.end(), [&](const InstrumentFile& g) { return g.file == f.file; });
          if (same != files.end()) { *same = std::move(f); } else { files.push_back(std::move(f)); }
        }
      }
      return files;
    }

    std::vector<InstrumentFile> loadShipped(std::string& dir)
    {
      for (const auto& d : instrumentDirectories())
      {
        std::error_code ec;
        if (fs::is_regular_file(fromUtf8(d) / "default.yaml", ec)) { dir = d; return loadDirectory(d); }
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

  std::vector<std::pair<std::string, std::string>> userInstrumentDirectories()
  {
    std::vector<std::pair<std::string, std::string>> dirs;
    if (const char* e = std::getenv("DIATRACEEXTRACTOR_INSTRUMENT_DIR"); e != nullptr && *e != '\0')
    {
      dirs.emplace_back("$DIATRACEEXTRACTOR_INSTRUMENT_DIR", toUtf8(fs::path(e)));
    }
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    if (base != nullptr && *base != '\0') { dirs.emplace_back("the user directory", toUtf8(fs::path(base) / "DIATraceExtractor" / "instruments")); }
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') { dirs.emplace_back("the user directory", toUtf8(fs::path(home) / "Library" / "Application Support" / "DIATraceExtractor" / "instruments")); }
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (xdg != nullptr && *xdg != '\0') { dirs.emplace_back("the user directory", toUtf8(fs::path(xdg) / "DIATraceExtractor" / "instruments")); }
    else if (home != nullptr && *home != '\0') { dirs.emplace_back("the user directory", toUtf8(fs::path(home) / ".config" / "DIATraceExtractor" / "instruments")); }
#endif
    return dirs;
  }

  std::vector<std::string> instrumentDirectories()
  {
    std::vector<std::string> dirs;
    const std::string exe = platform::executablePath();   // UTF-8
    if (!exe.empty())
    {
      // <prefix>/bin/DIATraceExtractor or <prefix>/libexec/DIATraceExtractor (the release layout); a build
      // directory has no share/ next to it and falls through to the source tree
      dirs.push_back(toUtf8(fromUtf8(exe).parent_path().parent_path() / "share" / "DIATraceExtractor" / "instruments"));
    }
#ifdef ODIA_SOURCE_DIR
    dirs.push_back(toUtf8(fromUtf8(ODIA_SOURCE_DIR) / "data" / "instruments"));
#endif
    return dirs;
  }

  InstrumentFile readInstrumentFile(const std::filesystem::path& file, bool is_default)
  {
    InstrumentFile f;
    const std::string path = toUtf8(file);
    f.path = path;
    f.file = toUtf8(file.filename());
    std::ifstream in(file, std::ios::binary);
    if (!in) { fail(path, "", "cannot be read"); }
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    YAML::Node root;
    try { root = YAML::Load(content); }
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
        if (!is_default) { allowed += "extends, aliases, keywords, priority, "; }
        for (const auto& k : instrumentValueKeys()) { allowed += k + (k == instrumentValueKeys().back() ? "" : ", "); }
        fail(path, key, "unknown key (allowed: " + allowed + ")");
      }
    }
    if (f.name.empty()) { fail(path, "name", "missing"); }
    if (is_default)
    {
      for (const auto& k : instrumentValueKeys()) { if (f.values.count(k) == 0) { fail(path, k, "missing (default.yaml sets every value)"); } }
    }
    f.sha1 = sha1Hex(content);
    return f;
  }

  InstrumentDefaults resolveInstrument(const std::string& spec, const std::string& run_instrument,
                                       const std::string& instrument_file, std::string& warning)
  {
    warning.clear();
    InstrumentDefaults d;
    std::vector<InstrumentFile> files = loadAll(d.directory);
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
      InstrumentFile own = readInstrumentFile(fs::path(instrument_file), false);   // as given (the tool's narrow paths)
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
    for (const auto& f : loadAll(dir)) { if (stem(f) != "default") { out += (out.empty() ? "" : ", ") + f.name; } }
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
      if (f.values.count(key) != 0)
      {
        // a user's file is recorded as such: the value is the user's, not the shipped default
        if (!f.origin.empty()) { return "user-file:" + f.file; }
        return f.file == "default.yaml" ? std::string("default") : "instrument-default:" + f.name;
      }
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
      if (!chain[i].origin.empty()) { t += " from " + chain[i].origin; }
    }
    return t;
  }

  std::string InstrumentDefaults::filesProvenance() const
  {
    std::string t;
    for (const auto& f : chain) { t += (t.empty() ? "" : "; ") + (f.origin.empty() ? f.file : f.path) + " sha1:" + f.sha1; }
    return t;
  }
}
