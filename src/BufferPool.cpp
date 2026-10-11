// ODIA-V3 trace extractor -- recycled trace buffers.
#include "BufferPool.h"
#include "Profile.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>
#include <sys/mman.h>

namespace ODIA
{
  BufferPool::BufferPool(std::size_t block_floats, std::size_t blocks)
    : block_floats_(std::max<std::size_t>(1, block_floats))
  {
    bytes_ = block_floats_ * sizeof(float) * blocks;
    if (bytes_ == 0) { return; }
    base_ = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (base_ == MAP_FAILED) { base_ = nullptr; throw std::bad_alloc(); }
    ::madvise(base_, bytes_, MADV_HUGEPAGE);   // fewer TLB misses; a hint, failure is harmless
    free_.reserve(blocks);
    // LIFO: block 0 is handed out first, so a small working set stays at the start of the mapping.
    for (std::size_t b = blocks; b-- > 0;)
    {
      free_.push_back(static_cast<float*>(base_) + b * block_floats_);
    }
  }

  BufferPool::~BufferPool()
  {
    if (base_) { ::munmap(base_, bytes_); }
  }

  std::size_t BufferPool::available() const
  {
    std::lock_guard lock(mtx_);
    return free_.size();
  }

  std::size_t BufferPool::peakInUse() const
  {
    std::lock_guard lock(mtx_);
    return peak_;
  }

  bool BufferPool::tryAcquire(std::size_t n, std::vector<float*>& out)
  {
    if (n == 0) { return true; }
    static prof::Counter& lc = prof::counter("lock.trace_pool_acquire");
    prof::timedLock(mtx_, lc);
    std::lock_guard lock(mtx_, std::adopt_lock);
    if (free_.size() < n) { return false; }
    out.insert(out.end(), free_.end() - static_cast<std::ptrdiff_t>(n), free_.end());
    free_.resize(free_.size() - n);
    in_use_ += n;
    peak_ = std::max(peak_, in_use_);
    return true;
  }

  void BufferPool::release(std::vector<float*>& blocks)
  {
    if (blocks.empty()) { return; }
    static prof::Counter& lc = prof::counter("lock.trace_pool_release");
    prof::timedLock(mtx_, lc);
    std::lock_guard lock(mtx_, std::adopt_lock);
    // Reverse order: the block used first is handed out first next time.
    free_.insert(free_.end(), blocks.rbegin(), blocks.rend());
    in_use_ -= blocks.size();
    blocks.clear();
  }

  TraceArena& TraceArena::operator=(TraceArena&& o) noexcept
  {
    if (this != &o)
    {
      release();
      pool_ = o.pool_;
      blocks_ = std::move(o.blocks_);
      cur_ = o.cur_;
      used_ = o.used_;
      heap_floats_ = o.heap_floats_;
      heap_ = std::move(o.heap_);
      o.pool_ = nullptr;
      o.blocks_.clear();
      o.cur_ = o.used_ = o.heap_floats_ = 0;
    }
    return *this;
  }

  void TraceArena::reset(BufferPool* pool, std::vector<float*>&& blocks)
  {
    release();
    pool_ = pool;
    blocks_ = std::move(blocks);
    cur_ = used_ = 0;
  }

  float* TraceArena::alloc(std::size_t n)
  {
    const std::size_t bf = pool_ ? pool_->blockFloats() : 0;
    while (cur_ < blocks_.size() && n <= bf)
    {
      if (used_ + n <= bf)
      {
        float* p = blocks_[cur_] + used_;
        used_ += n;
        std::memset(p, 0, n * sizeof(float));
        return p;
      }
      ++cur_;
      used_ = 0;
    }
    heap_.push_back(std::make_unique<float[]>(n));   // value-initialised: zero
    heap_floats_ += n;
    return heap_.back().get();
  }

  void TraceArena::release()
  {
    if (pool_) { pool_->release(blocks_); }
    blocks_.clear();
    heap_.clear();
    cur_ = used_ = heap_floats_ = 0;
  }

  std::size_t TraceArena::blocksFor(const std::vector<std::size_t>& runs, std::size_t block_floats)
  {
    std::size_t blocks = 0, used = block_floats;   // "full" so the first run opens a block
    for (const std::size_t n : runs)
    {
      if (n == 0 || n > block_floats) { continue; }   // empty or heap
      if (used + n > block_floats) { ++blocks; used = 0; }
      used += n;
    }
    return blocks;
  }
}
