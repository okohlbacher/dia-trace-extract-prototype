#include "Platform.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <psapi.h>
  #include <malloc.h>
#else
  #include <sys/mman.h>
  #include <sys/resource.h>
  #include <sys/time.h>
  #include <time.h>
  #include <unistd.h>
  #if defined(__linux__)
    #include <malloc.h>
    #include <fstream>
  #endif
  #if defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach-o/dyld.h>
  #endif
#endif

namespace ODIA::platform
{
#if defined(_WIN32)
  void* mapAnonymous(std::size_t bytes, bool reserve_only, bool prefault, bool)
  {
    // MEM_RESERVE | MEM_COMMIT: committed pages are backed on first touch, as with mmap; a pure reservation
    // would need a commit before every write, which the pools do not do.
    (void)reserve_only; (void)prefault;
    void* p = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p;
  }
  void unmap(void* p, std::size_t) { if (p) { ::VirtualFree(p, 0, MEM_RELEASE); } }
  void* alignedAlloc(std::size_t alignment, std::size_t bytes) { return ::_aligned_malloc(bytes, alignment); }
  void alignedFree(void* p) { ::_aligned_free(p); }

  namespace
  {
    double filetimeSeconds(const FILETIME& t)
    {
      ULARGE_INTEGER u; u.LowPart = t.dwLowDateTime; u.HighPart = t.dwHighDateTime;
      return static_cast<double>(u.QuadPart) * 1e-7;
    }
  }
  double processCpuSeconds()
  {
    FILETIME c, e, k, u;
    if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u)) { return 0.0; }
    return filetimeSeconds(k) + filetimeSeconds(u);
  }
  ThreadUsage threadUsage()
  {
    ThreadUsage r;
    FILETIME c, e, k, u;
    if (::GetThreadTimes(::GetCurrentThread(), &c, &e, &k, &u)) { r.cpu_ns = static_cast<std::uint64_t>((filetimeSeconds(k) + filetimeSeconds(u)) * 1e9); }
    PROCESS_MEMORY_COUNTERS m{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &m, sizeof m)) { r.minor_faults = m.PageFaultCount; }
    return r;
  }
  std::pair<double, double> rssGiB()
  {
    PROCESS_MEMORY_COUNTERS m{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &m, sizeof m)) { return {0.0, 0.0}; }
    return {static_cast<double>(m.WorkingSetSize) / 1073741824.0, static_cast<double>(m.PeakWorkingSetSize) / 1073741824.0};
  }
  double peakRssGiB() { return rssGiB().second; }
  std::uint64_t minorFaults()
  {
    PROCESS_MEMORY_COUNTERS m{};
    return ::GetProcessMemoryInfo(::GetCurrentProcess(), &m, sizeof m) ? m.PageFaultCount : 0;
  }
  void trimHeap() {}
  void reexecWithEnv(const char* var, const char* value, int, const char**)
  {
    // No exec on Windows; the MSVC/LLVM OpenMP runtimes read the variable at first use, which is after main().
    ::_putenv_s(var, value);
  }
#else
  void* mapAnonymous(std::size_t bytes, bool reserve_only, bool prefault, bool hugepages)
  {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  #ifdef MAP_NORESERVE
    if (reserve_only) { flags |= MAP_NORESERVE; }
  #else
    (void)reserve_only;
  #endif
  #ifdef MAP_POPULATE
    if (prefault) { flags |= MAP_POPULATE; }
  #else
    (void)prefault;
  #endif
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) { return nullptr; }
  #ifdef MADV_HUGEPAGE
    if (hugepages) { ::madvise(p, bytes, MADV_HUGEPAGE); }   // a hint; failure is harmless
  #else
    (void)hugepages;
  #endif
    return p;
  }
  void unmap(void* p, std::size_t bytes) { if (p) { ::munmap(p, bytes); } }
  void* alignedAlloc(std::size_t alignment, std::size_t bytes)
  {
    void* p = nullptr;
    if (::posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, bytes) != 0) { return nullptr; }
    return p;
  }
  void alignedFree(void* p) { std::free(p); }

  double processCpuSeconds()
  {
    timespec ts{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
  }
  namespace
  {
    std::uint64_t ns(const timeval& t) { return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(t.tv_usec) * 1000ull; }
  }
  ThreadUsage threadUsage()
  {
    rusage ru{};
  #ifdef RUSAGE_THREAD
    ::getrusage(RUSAGE_THREAD, &ru);
  #else
    ::getrusage(RUSAGE_SELF, &ru);   // macOS: no per-thread rusage; the diagnostics become process-wide
  #endif
    return {ns(ru.ru_utime) + ns(ru.ru_stime), static_cast<std::uint64_t>(ru.ru_minflt),
            static_cast<std::uint64_t>(ru.ru_nvcsw), static_cast<std::uint64_t>(ru.ru_nivcsw)};
  }
  double peakRssGiB()
  {
    rusage u{};
    ::getrusage(RUSAGE_SELF, &u);
  #ifdef __APPLE__
    return static_cast<double>(u.ru_maxrss) / (1024.0 * 1024.0 * 1024.0);   // bytes
  #else
    return static_cast<double>(u.ru_maxrss) / (1024.0 * 1024.0);            // KiB
  #endif
  }
  std::pair<double, double> rssGiB()
  {
  #if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    double rss = 0;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS)
    {
      rss = static_cast<double>(info.resident_size) / 1073741824.0;
    }
    return {rss, peakRssGiB()};
  #elif defined(__linux__)
    std::ifstream st("/proc/self/status");
    std::string line;
    double rss = 0, hwm = 0;
    while (std::getline(st, line))
    {
      if (line.rfind("VmRSS:", 0) == 0) { rss = std::stod(line.substr(6)) / (1024.0 * 1024.0); }
      if (line.rfind("VmHWM:", 0) == 0) { hwm = std::stod(line.substr(6)) / (1024.0 * 1024.0); }
    }
    return {rss, hwm};
  #else
    return {0.0, peakRssGiB()};
  #endif
  }
  std::uint64_t minorFaults()
  {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::uint64_t>(ru.ru_minflt);
  }
  void trimHeap()
  {
  #if defined(__linux__)
    ::malloc_trim(0);
  #endif
  }
  void reexecWithEnv(const char* var, const char* value, int argc, const char** argv)
  {
    ::setenv(var, value, 1);
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) { args.push_back(const_cast<char*>(argv[i])); }
    args.push_back(nullptr);
  #if defined(__APPLE__)
    char exe[4096];
    std::uint32_t exe_len = sizeof exe;
    if (_NSGetExecutablePath(exe, &exe_len) == 0) { ::execv(exe, args.data()); }
  #else
    ::execv("/proc/self/exe", args.data());
  #endif
    // execv returns only on failure: continue with the variable set for this process
  }
#endif
}
