// Operating-system layer: anonymous memory mappings, aligned allocation, process/thread accounting and the
// one-time re-exec. Linux is the primary platform; macOS and Windows get the nearest equivalent, and the
// diagnostics degrade to what the platform offers (never the extraction).
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace ODIA::platform
{
  /// Anonymous private mapping of `bytes`, zero-filled, page-aligned; nullptr on failure.
  /// reserve_only: virtual reservation, pages are backed on first touch (MAP_NORESERVE on Linux; the default
  /// behaviour elsewhere). prefault: fault every page in now (MAP_POPULATE on Linux; a no-op elsewhere).
  /// hugepages: advise transparent huge pages where that exists (a hint).
  void* mapAnonymous(std::size_t bytes, bool reserve_only = false, bool prefault = false, bool hugepages = false);
  void unmap(void* p, std::size_t bytes);

  /// Aligned heap allocation; free with alignedFree (on Windows the two must pair).
  void* alignedAlloc(std::size_t alignment, std::size_t bytes);
  void alignedFree(void* p);

  /// CPU time of the process (user + system), seconds.
  double processCpuSeconds();

  /// Per-thread accounting where the platform has it (Linux: RUSAGE_THREAD), else per process:
  /// CPU ns, minor page faults, voluntary and involuntary context switches.
  struct ThreadUsage { std::uint64_t cpu_ns = 0, minor_faults = 0, voluntary_switches = 0, involuntary_switches = 0; };
  ThreadUsage threadUsage();

  /// Resident set size now and its peak, GiB.
  std::pair<double, double> rssGiB();
  double peakRssGiB();
  /// Minor page faults of the process so far.
  std::uint64_t minorFaults();

  /// Hand memory the allocator holds back to the OS (glibc's malloc_trim; a no-op elsewhere).
  void trimHeap();
  /// Physical cores this process may run on (its CPU affinity on Linux and Windows; hardware threads of one core
  /// count once); at least 1.
  int physicalCores();

  /// Positional reads of one file from any thread: one shared handle, no per-thread stream state (pread; ReadFile
  /// with an offset on Windows). readAt fails when the range is short. Not copyable.
  class FileReader
  {
  public:
    explicit FileReader(const char* path);
    ~FileReader();
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    bool ok() const { return handle_ != -1; }
    bool readAt(void* dst, std::size_t bytes, std::uint64_t offset) const;
  private:
    std::intptr_t handle_ = -1;
  };

  /// Re-execute the running program once with `var=value` in its environment (needed where a runtime reads the
  /// variable only at load, e.g. libgomp's OMP_WAIT_POLICY). Returns only on failure or where re-exec is not
  /// supported, in which case the variable is set for the current process as far as that helps.
  void reexecWithEnv(const char* var, const char* value, int argc, const char** argv);
}
