// ODIA-V3 trace extractor -- minimal mzML <spectrum> parser for the run grid.
#include "MzMLFast.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <numeric>
#include <zlib.h>

namespace ODIA
{
  namespace
  {
    /// Value of attribute `name` inside one tag (between '<' and '>'); empty if absent.
    std::string_view attribute(std::string_view tag, std::string_view name)
    {
      for (std::size_t p = 0;;)
      {
        p = tag.find(name, p);
        if (p == std::string_view::npos) { return {}; }
        const bool start_ok = p > 0 && (tag[p - 1] == ' ' || tag[p - 1] == '\t' || tag[p - 1] == '\n' || tag[p - 1] == '\r');
        std::size_t q = p + name.size();
        if (start_ok && q + 1 < tag.size() && tag[q] == '=' && (tag[q + 1] == '"' || tag[q + 1] == '\''))
        {
          const char quote = tag[q + 1];
          const std::size_t e = tag.find(quote, q + 2);
          if (e == std::string_view::npos) { return {}; }
          return tag.substr(q + 2, e - q - 2);
        }
        p = q;
      }
    }

    bool toDouble(std::string_view s, double& v)
    {
      if (s.empty()) { return false; }
      const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
      return r.ec == std::errc();
    }

    /// RFC 4648 base64 (whitespace ignored). Returns false on invalid input.
    bool base64(std::string_view in, std::vector<unsigned char>& out)
    {
      static const auto table = [] {
        std::array<signed char, 256> t{};
        t.fill(-1);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) { t[static_cast<unsigned char>(a[i])] = static_cast<signed char>(i); }
        return t;
      }();
      out.resize(in.size() / 4 * 3 + 3);
      std::size_t o = 0;
      std::uint32_t acc = 0;
      int bits = 0;
      for (const char ch : in)
      {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '=') { break; }
        const int v = table[c];
        if (v < 0)
        {
          if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { continue; }
          return false;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = static_cast<unsigned char>((acc >> bits) & 0xFF); }
      }
      out.resize(o);
      return true;
    }

    struct Array
    {
      int kind = 0;          // 1 = m/z, 2 = intensity, 0 = other
      int bits = 0;          // 32 / 64
      bool zlib = false, unsupported = false;
      std::string_view binary;
    };
  }

  bool parseSpectrumFast(std::string_view xml, GridSpectrum& out, std::vector<unsigned char>& scratch,
                         std::vector<unsigned char>& scratch2)
  {
    out.ms_level = 0;
    out.rt = 0;
    out.target = out.lower = out.upper = 0;
    out.was_unsorted = false;
    out.mz.clear();
    out.intensity.clear();

    // <spectrum ... defaultArrayLength="n">
    const std::size_t head_end = xml.find('>');
    if (xml.substr(0, 9) != "<spectrum" || head_end == std::string_view::npos) { return false; }
    std::size_t n = 0;
    {
      const auto v = attribute(xml.substr(0, head_end), "defaultArrayLength");
      if (v.empty() || std::from_chars(v.data(), v.data() + v.size(), n).ec != std::errc()) { return false; }
    }

    bool have_time = false, have_level = false, have_target = false;
    double lower_off = 0, upper_off = 0, selected = 0;
    bool have_selected = false;
    Array arrays[2];
    int n_arrays = 0;
    Array current;
    bool in_array = false;

    // Walk the tags. Values never contain '<' (XML-escaped), so the next tag is the next '<'.
    for (std::size_t p = head_end + 1; p < xml.size();)
    {
      const char* q = static_cast<const char*>(std::memchr(xml.data() + p, '<', xml.size() - p));
      if (q == nullptr) { break; }
      p = static_cast<std::size_t>(q - xml.data());
      const std::size_t e = xml.find('>', p);
      if (e == std::string_view::npos) { return false; }
      const std::string_view tag = xml.substr(p, e - p);
      if (tag.substr(0, 8) == "<cvParam")
      {
        const auto acc = attribute(tag, "accession");
        if (acc == "MS:1000511") { double v = 0; if (!toDouble(attribute(tag, "value"), v)) { return false; } out.ms_level = static_cast<int>(v); have_level = true; }
        else if (acc == "MS:1000016")
        {
          if (!toDouble(attribute(tag, "value"), out.rt)) { return false; }
          const auto unit = attribute(tag, "unitAccession");
          if (unit == "UO:0000031" || unit == "MS:1000038") { out.rt *= 60.0; }
          else if (unit != "UO:0000010") { return false; }   // unknown or missing unit: let OpenMS decide
          have_time = true;
        }
        else if (acc == "MS:1000827") { if (!toDouble(attribute(tag, "value"), out.target)) { return false; } have_target = true; }
        else if (acc == "MS:1000828") { if (!toDouble(attribute(tag, "value"), lower_off)) { return false; } }
        else if (acc == "MS:1000829") { if (!toDouble(attribute(tag, "value"), upper_off)) { return false; } }
        else if (acc == "MS:1000744") { if (!toDouble(attribute(tag, "value"), selected)) { return false; } have_selected = true; }
        else if (in_array)
        {
          if (acc == "MS:1000514") { current.kind = 1; }
          else if (acc == "MS:1000515") { current.kind = 2; }
          else if (acc == "MS:1000521") { current.bits = 32; }
          else if (acc == "MS:1000523") { current.bits = 64; }
          else if (acc == "MS:1000574") { current.zlib = true; }
          else if (acc == "MS:1000576") { /* no compression */ }
          else if (acc == "MS:1002312" || acc == "MS:1002313" || acc == "MS:1002314") { current.unsupported = true; }   // numpress
        }
      }
      else if (tag.substr(0, 16) == "<binaryDataArray" && tag.substr(0, 20) != "<binaryDataArrayList")
      {
        current = Array{};
        in_array = true;
      }
      else if (tag.substr(0, 7) == "<binary" && (tag.size() == 7 || tag[7] == ' ' || tag[7] == '/'))
      {
        if (!in_array) { return false; }
        const std::size_t b0 = e + 1;
        const std::size_t b1 = xml.find("</binary>", b0);
        if (tag.back() == '/') { current.binary = {}; }
        else
        {
          if (b1 == std::string_view::npos) { return false; }
          current.binary = xml.substr(b0, b1 - b0);
        }
        if (current.kind != 0)
        {
          if (current.unsupported || current.bits == 0 || n_arrays >= 2) { return false; }
          arrays[n_arrays++] = current;
        }
        in_array = false;
        p = (tag.back() == '/') ? e + 1 : b1 + 9;
        continue;
      }
      else if (tag.substr(0, 11) == "</spectrum") { break; }
      p = e + 1;
    }
    if (!have_time || !have_level) { return false; }
    if (out.ms_level >= 2)
    {
      if (!have_target && have_selected) { out.target = selected; have_target = true; }
      if (have_target) { out.lower = out.target - lower_off; out.upper = out.target + upper_off; }
    }
    if (n == 0) { return true; }
    if (n_arrays != 2 || arrays[0].kind == arrays[1].kind) { return false; }

    out.mz.resize(n);
    out.intensity.resize(n);
    for (const Array& a : arrays)
    {
      if (!base64(a.binary, scratch)) { return false; }
      const std::size_t width = a.bits / 8, need = n * width;
      const unsigned char* bytes = scratch.data();
      if (a.zlib)
      {
        scratch2.resize(need);
        uLongf len = static_cast<uLongf>(need);
        if (::uncompress(scratch2.data(), &len, scratch.data(), static_cast<uLong>(scratch.size())) != Z_OK || len != need) { return false; }
        bytes = scratch2.data();
      }
      else if (scratch.size() != need) { return false; }
      // mzML binary arrays are little-endian (as is the host: x86-64 / aarch64).
      for (std::size_t i = 0; i < n; ++i)
      {
        double v;
        if (width == 8) { std::memcpy(&v, bytes + 8 * i, 8); }
        else { float f; std::memcpy(&f, bytes + 4 * i, 4); v = f; }
        if (a.kind == 1) { out.mz[i] = toFixed(v); }
        else { out.intensity[i] = static_cast<float>(v); }
      }
    }
    // Ascending m/z (as OpenMS's sortByPosition would make it).
    if (!std::is_sorted(out.mz.begin(), out.mz.end()))
    {
      std::vector<std::size_t> o(n);
      std::iota(o.begin(), o.end(), 0);
      std::stable_sort(o.begin(), o.end(), [&](std::size_t a, std::size_t b) { return out.mz[a] < out.mz[b]; });
      std::vector<MzFixed> m(n);
      std::vector<float> in(n);
      for (std::size_t i = 0; i < n; ++i) { m[i] = out.mz[o[i]]; in[i] = out.intensity[o[i]]; }
      out.mz.swap(m);
      out.intensity.swap(in);
      out.was_unsorted = true;
    }
    return true;
  }
}
