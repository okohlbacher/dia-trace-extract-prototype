// ODIA-V3 trace extractor -- Arrow status/result to exception.
#pragma once

#include <arrow/result.h>
#include <arrow/status.h>

#include <stdexcept>
#include <utility>

namespace ODIA
{
  inline void check(const arrow::Status& s)
  {
    if (!s.ok()) { throw std::runtime_error("Arrow: " + s.ToString()); }
  }
  template <typename T> T check(arrow::Result<T> r)
  {
    if (!r.ok()) { throw std::runtime_error("Arrow: " + r.status().ToString()); }
    return std::move(r).ValueUnsafe();
  }
}
