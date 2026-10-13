// Number parsing from string views, locale-independent. std::from_chars where the standard library has it for
// floating point; Apple's libc++ on older deployment targets deletes that overload, so macOS falls back to strtod
// on a bounded copy (from_chars semantics kept: no leading whitespace, the whole view must be consumed).
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>

namespace ODIA
{
  inline bool parseDouble(std::string_view s, double& v)
  {
    if (s.empty()) { return false; }
#if defined(__APPLE__)
    char buf[128];
    if (s.size() >= sizeof buf || s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r') { return false; }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char* end = nullptr;
    v = std::strtod(buf, &end);
    return end == buf + s.size();
#else
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
#endif
  }

  /// Leading number of the view (trailing text allowed), as from_chars would parse it; false if none.
  inline bool parseDoublePrefix(std::string_view s, double& v)
  {
    if (s.empty()) { return false; }
#if defined(__APPLE__)
    char buf[128];
    const std::size_t n = s.size() < sizeof buf - 1 ? s.size() : sizeof buf - 1;
    if (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r') { return false; }
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    char* end = nullptr;
    v = std::strtod(buf, &end);
    return end != buf;
#else
    return std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc();
#endif
  }

  template <class Int>
  inline bool parseInt(std::string_view s, Int& v)
  {
    if (s.empty()) { return false; }
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc();
  }
}
