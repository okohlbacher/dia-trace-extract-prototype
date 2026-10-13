// ODIA-V3 trace extractor -- recycling memory pool for Arrow / Parquet buffers.
#include "ArrowPool.h"
#include "Profile.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include "Platform.h"

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

  /// Per-thread front end: a few free blocks per class and pending statistics, owned by one thread.
  struct RecyclingPool::Local
  {
    // Cached bytes per thread (ODIA_POOL_THREAD_CACHE_MB, default 2).
    static int64_t cap()
    {
      static const int64_t c = [] {
        const char* e = std::getenv("ODIA_POOL_THREAD_CACHE_MB");
        return (e ? std::atoll(e) : 2) << 20;
      }();
      return c;
    }
    static constexpr std::size_t kPerClass = 8;                 // blocks per class
    static constexpr std::size_t kMaxBlock = std::size_t{1} << 20;
    static constexpr int64_t kFlush = int64_t{1} << 20;         // fold statistics every ~1 MiB net change
    RecyclingPool* owner = nullptr;
    std::array<std::vector<uint8_t*>, kClasses> blocks;
    int64_t cached = 0, delta = 0, total = 0, count = 0, reuses = 0;
    bool dying = false;
    ~Local()
    {
      dying = true;   // from here on the shared path serves this thread
      if (!owner) { return; }
      owner->flush_(*this);
      for (int c = 0; c < kClasses; ++c)
      {
        for (uint8_t* p : blocks[static_cast<std::size_t>(c)]) { owner->putLarge_(p, c); }
      }
    }
  };

  RecyclingPool::Local* RecyclingPool::local_()
  {
    thread_local Local l;
    if (l.dying) { return nullptr; }
    if (l.owner == nullptr) { l.owner = this; }
    return l.owner == this ? &l : nullptr;   // one pool per thread gets the front end (in practice: arrowPool())
  }

  void RecyclingPool::flush_(Local& l)
  {
    if (l.delta != 0)
    {
      const int64_t now = allocated_.fetch_add(l.delta) + l.delta;
      int64_t peak = peak_.load();
      while (now > peak && !peak_.compare_exchange_weak(peak, now)) {}
    }
    if (l.total) { total_ += l.total; }
    if (l.count) { count_ += l.count; }
    if (l.reuses) { reuses_ += l.reuses; }
    l.delta = l.total = l.count = l.reuses = 0;
  }

  void RecyclingPool::account_(int64_t delta)
  {
    if (Local* l = local_())
    {
      l->delta += delta;
      if (delta > 0) { l->total += delta; ++l->count; }
      if (l->delta >= Local::kFlush || l->delta <= -Local::kFlush) { flush_(*l); }
      return;
    }
    const int64_t now = allocated_.fetch_add(delta) + delta;
    int64_t peak = peak_.load();
    while (now > peak && !peak_.compare_exchange_weak(peak, now)) {}
    if (delta > 0) { total_ += delta; ++count_; }
  }

  uint8_t* RecyclingPool::takeLarge_(int c)
  {
    if (Local* l = local_(); l && !l->blocks[static_cast<std::size_t>(c)].empty())
    {
      auto& v = l->blocks[static_cast<std::size_t>(c)];
      uint8_t* p = v.back();
      v.pop_back();
      l->cached -= static_cast<int64_t>(classSize(c));
      ++l->reuses;
      return p;
    }
    {
      auto& f = free_[static_cast<std::size_t>(c)];
      static prof::Counter& lc = prof::counter("lock.arrow_pool_take");
      prof::timedLock(f.mtx, lc);
      std::lock_guard lock(f.mtx, std::adopt_lock);
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
    void* p = platform::mapAnonymous(n);
    if (p == nullptr) { return nullptr; }
    mapped_ += static_cast<int64_t>(n);
    return static_cast<uint8_t*>(p);
  }

  void RecyclingPool::putLarge_(uint8_t* p, int c)
  {
    const auto n = static_cast<int64_t>(classSize(c));
    if (Local* l = local_(); l && static_cast<std::size_t>(n) <= Local::kMaxBlock && l->cached + n <= Local::cap()
        && l->blocks[static_cast<std::size_t>(c)].size() < Local::kPerClass)
    {
      if (l->blocks[static_cast<std::size_t>(c)].capacity() == 0) { l->blocks[static_cast<std::size_t>(c)].reserve(Local::kPerClass); }
      l->blocks[static_cast<std::size_t>(c)].push_back(p);
      l->cached += n;
      return;
    }
    // Reserve the cache space first (fetch_add), give it back if the cap is exceeded: two threads cannot both
    // pass a check-then-add and overshoot the cap by a block each.
    if (cached_.fetch_add(n) + n <= cache_cap_)
    {
      auto& f = free_[static_cast<std::size_t>(c)];
      static prof::Counter& lc = prof::counter("lock.arrow_pool_put");
      prof::timedLock(f.mtx, lc);
      std::lock_guard lock(f.mtx, std::adopt_lock);
      f.blocks.push_back(p);
      return;
    }
    cached_ -= n;
    platform::unmap(p, static_cast<std::size_t>(n));
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
      *out = static_cast<uint8_t*>(platform::alignedAlloc(static_cast<std::size_t>(std::max<int64_t>(alignment, sizeof(void*))), n));
    }
    if (*out == nullptr) { return arrow::Status::OutOfMemory("malloc of size ", size, " failed"); }
    account_(size);
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
    else { platform::alignedFree(buffer); }
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
        platform::unmap(p, classSize(c));
        mapped_ -= static_cast<int64_t>(classSize(c));
        cached_ -= static_cast<int64_t>(classSize(c));
      }
      f.blocks.clear();
    }
  }

  RecyclingPool* arrowPool()
  {
    // Never destroyed: OpenMP workers end after main() returns, and their thread_local Local caches hand their
    // blocks back through the pool's mutex. With a function-static pool that mutex is already destroyed by then;
    // glibc tolerates the lock, libc++ throws std::system_error ("mutex lock failed: Invalid argument") from
    // ~Local and the process aborts after all output is written. The process end reclaims the memory.
    static RecyclingPool* pool = new RecyclingPool;
    return pool;
  }
}
