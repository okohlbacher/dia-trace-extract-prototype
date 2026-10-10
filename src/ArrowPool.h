// ODIA-V3 trace extractor -- recycling memory pool for Arrow / Parquet buffers.
#pragma once

#include <arrow/memory_pool.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ODIA
{
  /// Arrow memory pool that recycles its buffers instead of returning them to the C allocator.
  ///
  /// Buffers of at least 4 KiB are anonymous mappings in size classes (four per power of two, so at most
  /// 25% slack); a freed buffer goes on its class's LIFO free list and is handed out again (warm in
  /// cache and already faulted in). Growing a buffer within its class keeps it in place. Smaller
  /// buffers use aligned malloc. Up to `cache_bytes` of free buffers are kept; beyond that they are
  /// unmapped. Used for every Arrow/Parquet allocation of the extraction (library decoding, trace
  /// encoding, Parquet writing): without it, glibc kept ~4 GiB of freed Arrow memory across the
  /// workers' arenas (5.5 GiB arena, 1.4 GiB in use, full 7 M-precursor run).
  class RecyclingPool : public arrow::MemoryPool
  {
  public:
    explicit RecyclingPool(std::size_t cache_bytes = std::size_t{1} << 30);
    ~RecyclingPool() override;

    arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override;
    arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t** ptr) override;
    void Free(uint8_t* buffer, int64_t size, int64_t alignment) override;
    void ReleaseUnused() override;

    int64_t bytes_allocated() const override { return allocated_.load(); }
    int64_t max_memory() const override { return peak_.load(); }
    int64_t total_bytes_allocated() const override { return total_.load(); }
    int64_t num_allocations() const override { return count_.load(); }
    std::string backend_name() const override { return "odia-recycling"; }

    int64_t cachedBytes() const { return cached_.load(); }
    int64_t mappedBytes() const { return mapped_.load(); }   ///< large buffers mapped (in use + cached)
    int64_t reuses() const { return reuses_.load(); }

  private:
    static constexpr int kLargeLog = 12;   // recycled from 4 KiB (one page) upward
    static constexpr std::size_t kLarge = std::size_t{1} << kLargeLog;
    static constexpr int kClasses = 4 * 52;
    static int classOf(std::size_t size);
    static std::size_t classSize(int c);
    uint8_t* takeLarge_(int c);
    void putLarge_(uint8_t* p, int c);
    void account_(int64_t delta);

    struct FreeList
    {
      std::mutex mtx;
      std::vector<uint8_t*> blocks;
    };
    std::array<FreeList, kClasses> free_;
    const int64_t cache_cap_;
    std::atomic<int64_t> allocated_{0}, peak_{0}, total_{0}, count_{0}, cached_{0}, mapped_{0}, reuses_{0};
  };

  /// The process-wide pool for the extractor's Arrow/Parquet work.
  RecyclingPool* arrowPool();
}
