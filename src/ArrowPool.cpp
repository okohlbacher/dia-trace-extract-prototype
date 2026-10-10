// ODIA-V3 trace extractor -- recycling memory pool for Arrow / Parquet buffers.
#include "ArrowPool.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

namespace ODIA
{
  namespace
  {
    alignas(64) uint8_t zero_size_area[64];
  }

  RecyclingPool::RecyclingPool(std::size_t cache_bytes) : cache_cap_(static_cast<int64_t>(cache_bytes)) {}

  RecyclingPool::~RecyclingPool() { ReleaseUnused(); }

  int RecyclingPool::classOf(std::size_t size)
  {
    // Classes: 2^k * (4 + s) / 4 for s = 0..3, from kLarge upward; the smallest class >= size.
    const std::size_t s = std::max(size, kLarge);
    const int k = static_cast<int>(std::bit_width(s - 1)) - 1;   // 2^k < s <= 2^(k+1)
    const std::size_t base = std::size_t{1} << k;
    int sub = static_cast<int>((s - base + (base / 4) - 1) / (base / 4));   // 1..4 quarter steps above 2^k
    return std::min((k - kLargeLog) * 4 + sub, kClasses - 1);
  }

  std::size_t RecyclingPool::classSize(int c)
  {
    if (c == 0) { return kLarge; }
    const int k = (c - 1) / 4 + kLargeLog, sub = (c - 1) % 4 + 1;
    return (std::size_t{1} << k) + sub * (std::size_t{1} << k) / 4;
  }

  void RecyclingPool::account_(int64_t delta)
  {
    const int64_t now = allocated_.fetch_add(delta) + delta;
    int64_t peak = peak_.load();
    while (now > peak && !peak_.compare_exchange_weak(peak, now)) {}
  }

  uint8_t* RecyclingPool::takeLarge_(int c)
  {
    {
      auto& f = free_[static_cast<std::size_t>(c)];
      std::lock_guard lock(f.mtx);
      if (!f.blocks.empty())
      {
        uint8_t* p = f.blocks.back();
        f.blocks.pop_back();
        cached_ -= static_cast<int64_t>(classSize(c));
        ++reuses_;
        return p;
      }
    }
    const std::size_t n = classSize(c);
    void* p = ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { return nullptr; }
    mapped_ += static_cast<int64_t>(n);
    return static_cast<uint8_t*>(p);
  }

  void RecyclingPool::putLarge_(uint8_t* p, int c)
  {
    const auto n = static_cast<int64_t>(classSize(c));
    if (cached_.load() + n <= cache_cap_)
    {
      auto& f = free_[static_cast<std::size_t>(c)];
      std::lock_guard lock(f.mtx);
      f.blocks.push_back(p);
      cached_ += n;
      return;
    }
    ::munmap(p, static_cast<std::size_t>(n));
    mapped_ -= n;
  }

  arrow::Status RecyclingPool::Allocate(int64_t size, int64_t alignment, uint8_t** out)
  {
    if (size < 0) { return arrow::Status::Invalid("negative allocation size"); }
    if (size == 0) { *out = zero_size_area; return arrow::Status::OK(); }
    const auto n = static_cast<std::size_t>(size);
    if (n >= kLarge && alignment <= 4096)
    {
      *out = takeLarge_(classOf(n));
    }
    else
    {
      void* p = nullptr;
      if (::posix_memalign(&p, static_cast<std::size_t>(std::max<int64_t>(alignment, sizeof(void*))), n) != 0) { p = nullptr; }
      *out = static_cast<uint8_t*>(p);
    }
    if (*out == nullptr) { return arrow::Status::OutOfMemory("malloc of size ", size, " failed"); }
    account_(size);
    total_ += size;
    ++count_;
    return arrow::Status::OK();
  }

  arrow::Status RecyclingPool::Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t** ptr)
  {
    if (new_size < 0) { return arrow::Status::Invalid("negative reallocation size"); }
    const auto o = static_cast<std::size_t>(old_size), n = static_cast<std::size_t>(new_size);
    const bool old_large = o >= kLarge && alignment <= 4096 && *ptr != zero_size_area;
    const bool new_large = n >= kLarge && alignment <= 4096;
    if (old_large && new_large && classOf(o) == classOf(n))
    {
      account_(new_size - old_size);   // same block: grows or shrinks in place
      if (new_size > old_size) { total_ += new_size - old_size; }
      return arrow::Status::OK();
    }
    uint8_t* fresh = nullptr;
    ARROW_RETURN_NOT_OK(Allocate(new_size, alignment, &fresh));
    if (*ptr != zero_size_area && old_size > 0) { std::memcpy(fresh, *ptr, static_cast<std::size_t>(std::min(old_size, new_size))); }
    Free(*ptr, old_size, alignment);
    *ptr = fresh;
    return arrow::Status::OK();
  }

  void RecyclingPool::Free(uint8_t* buffer, int64_t size, int64_t alignment)
  {
    if (buffer == zero_size_area || buffer == nullptr) { return; }
    const auto n = static_cast<std::size_t>(size);
    if (n >= kLarge && alignment <= 4096) { putLarge_(buffer, classOf(n)); }
    else { std::free(buffer); }
    account_(-size);
  }

  void RecyclingPool::ReleaseUnused()
  {
    for (int c = 0; c < kClasses; ++c)
    {
      auto& f = free_[static_cast<std::size_t>(c)];
      std::lock_guard lock(f.mtx);
      for (uint8_t* p : f.blocks)
      {
        ::munmap(p, classSize(c));
        mapped_ -= static_cast<int64_t>(classSize(c));
        cached_ -= static_cast<int64_t>(classSize(c));
      }
      f.blocks.clear();
    }
  }

  RecyclingPool* arrowPool()
  {
    static RecyclingPool pool;
    return &pool;
  }
}
