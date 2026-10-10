// ODIA-V3 trace extractor -- recycled trace buffers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ODIA
{
  /// Global pool of fixed-size float blocks for extracted traces.
  ///
  /// One anonymous mapping, pre-faulted once (MAP_POPULATE) and advised for huge pages, carved into
  /// blocks; a LIFO free list hands out the most recently released (cache- and TLB-hot) block first.
  /// Extraction reserves whole blocks per work unit and returns them as soon as the unit's traces are
  /// written, so trace memory is a fixed budget with no page faults or allocator churn after start-up.
  class BufferPool
  {
  public:
    BufferPool(std::size_t block_floats, std::size_t blocks);
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    std::size_t blockFloats() const { return block_floats_; }
    std::size_t capacity() const { return free_.capacity(); }
    std::size_t available() const;
    std::size_t peakInUse() const;
    std::size_t bytes() const { return bytes_; }

    /// All or nothing: append `n` blocks to `out`, or none if fewer are free.
    bool tryAcquire(std::size_t n, std::vector<float*>& out);
    void release(std::vector<float*>& blocks);   ///< clears `blocks`

  private:
    std::size_t block_floats_ = 0, bytes_ = 0;
    void* base_ = nullptr;
    mutable std::mutex mtx_;
    std::vector<float*> free_;
    std::size_t in_use_ = 0, peak_ = 0;
  };

  /// Trace storage of one work unit: bump allocation in the unit's reserved pool blocks; a value run
  /// never straddles blocks. Anything that does not fit (no reservation, or a trace larger than a
  /// block) falls back to the heap, so extraction always proceeds.
  class TraceArena
  {
  public:
    TraceArena() = default;
    ~TraceArena() { release(); }
    TraceArena(TraceArena&& o) noexcept { *this = std::move(o); }
    TraceArena& operator=(TraceArena&& o) noexcept;
    TraceArena(const TraceArena&) = delete;
    TraceArena& operator=(const TraceArena&) = delete;

    /// Start over with reserved `blocks` of `pool` (may be empty).
    void reset(BufferPool* pool, std::vector<float*>&& blocks);
    /// `n` zeroed floats.
    float* alloc(std::size_t n);
    /// Return pool blocks, free heap storage.
    void release();
    std::size_t heapFloats() const { return heap_floats_; }

    /// Blocks needed for value runs of the given sizes, packed as alloc() packs them.
    static std::size_t blocksFor(const std::vector<std::size_t>& runs, std::size_t block_floats);

  private:
    BufferPool* pool_ = nullptr;
    std::vector<float*> blocks_;
    std::size_t cur_ = 0, used_ = 0, heap_floats_ = 0;
    std::vector<std::unique_ptr<float[]>> heap_;
  };
}
