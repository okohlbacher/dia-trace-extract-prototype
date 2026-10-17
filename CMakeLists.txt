cmake_minimum_required(VERSION 3.24)
project(DIATraceExtractor VERSION 0.3.0 LANGUAGES CXX)

# ODIA-V3 DIA trace extractor: a standalone TOPP-style tool built against an
# OpenMS (>= 3.7, develop) build or install. Configure with
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_PREFIX_PATH="<OpenMS-build>;<OpenMS-build>/vcpkg_installed/x64-linux-dynamic"
# The project version is the release version (git tag V<version>); the release workflow checks that they match.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenMS CONFIG REQUIRED)
# OpenMSConfig does not re-export Arrow/Parquet; find them from the same prefix.
find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
find_package(OpenMP REQUIRED)
find_package(ZLIB REQUIRED)
# the instrument files (data/instruments/*.yaml); from OpenMS's vcpkg tree (ci/patch_openms_manifest.py adds it)
find_package(yaml-cpp CONFIG REQUIRED)
message(STATUS "OpenMS ${OpenMS_VERSION} (${OpenMS_DIR}); Arrow ${Arrow_VERSION}")

if(TARGET Arrow::arrow_shared)
  set(ODIA_ARROW Arrow::arrow_shared)
else()
  set(ODIA_ARROW Arrow::arrow_static)
endif()
if(TARGET Parquet::parquet_shared)
  set(ODIA_PARQUET Parquet::parquet_shared)
else()
  set(ODIA_PARQUET Parquet::parquet_static)
endif()

# Compiler-specific flags shared by every target of this project (and applied to the mzpeak submodule below).
if(MSVC)
  # MSVC's own OpenMP (vcomp140, OpenMP 2.0), as OpenMS uses: one OpenMP runtime per process (the loops use signed
  # indices and only 2.0 constructs, so the LLVM runtime of /openmp:llvm is not needed)
  set(ODIA_CXX_OPTIONS /W3 /bigobj /utf-8 /EHsc)
  set(ODIA_CXX_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  set(ODIA_CXX_OPTIONS -Wall -Wextra)
  set(ODIA_CXX_DEFINITIONS)
endif()

add_library(odia_traces STATIC
  src/Library.cpp
  src/MzMLFast.cpp
  src/Platform.cpp
  src/Profile.cpp
  src/RtMap.cpp
  src/RunGrid.cpp
  src/ArrowPool.cpp
  src/BufferPool.cpp
  src/Extractor.cpp
  src/Scheduler.cpp
  src/TraceWriter.cpp
  src/ParquetMerge.cpp
  src/Calibration.cpp
  src/Instruments.cpp)
target_include_directories(odia_traces PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# mzPeak reader/writer: the mzpeak-openms library (git submodule external/mzpeak, tag partitioned-read-2026-10-11),
# built here against the same Arrow as OpenMS. It needs Boost >= 1.89 with json and program_options, which the
# OpenMS vcpkg tree provides once they are in its manifest (ci/patch_openms_manifest.py); another BoostConfig.cmake
# directory can be given with MZPEAK_BOOST_DIR.
set(MZPEAK_BOOST_DIR "" CACHE PATH "Directory of a BoostConfig.cmake (Boost >= 1.89 with json, program_options) for external/mzpeak")
if(MZPEAK_BOOST_DIR)
  set(Boost_DIR "${MZPEAK_BOOST_DIR}" CACHE PATH "" FORCE)
endif()
# The submodule links Arrow::arrow_shared and Parquet::parquet_shared by name; static Arrow builds (the Windows vcpkg
# triplets) provide only the *_static targets. Provide the shared names as global interface targets forwarding to
# the static ones (not ALIASes: Arrow's own config, re-run by the submodule, aliases arrow_shared to them, and an
# alias of an alias is an error).
foreach(_lib Arrow::arrow Parquet::parquet)
  if(NOT TARGET ${_lib}_shared AND TARGET ${_lib}_static)
    add_library(${_lib}_shared INTERFACE IMPORTED GLOBAL)
    target_link_libraries(${_lib}_shared INTERFACE ${_lib}_static)
    get_target_property(_inc ${_lib}_static INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      set_target_properties(${_lib}_shared PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_inc}")
    endif()
  endif()
endforeach()
add_subdirectory(external/mzpeak EXCLUDE_FROM_ALL)
if(MSVC)
  # the submodule's GCC/Clang warning flags mean /Wall (thousands of warnings) or unknown options under MSVC
  set_property(TARGET mzpeak PROPERTY COMPILE_OPTIONS /W3 /bigobj /utf-8 /EHsc)
  target_compile_definitions(mzpeak PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(TARGET numpress)
    target_compile_options(numpress PRIVATE /utf-8 /EHsc)
  endif()
endif()
target_link_libraries(odia_traces PUBLIC OpenMS ${ODIA_ARROW} ${ODIA_PARQUET} OpenMP::OpenMP_CXX ZLIB::ZLIB mzpeak yaml-cpp::yaml-cpp)
target_compile_options(odia_traces PRIVATE ${ODIA_CXX_OPTIONS})
target_compile_definitions(odia_traces PUBLIC ${ODIA_CXX_DEFINITIONS} ODIA_VERSION="${PROJECT_VERSION}")
# the source tree's instrument files, found when the tool runs from a build directory (src/Instruments.cpp)
set_property(SOURCE src/Instruments.cpp APPEND PROPERTY COMPILE_DEFINITIONS ODIA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
  target_link_libraries(odia_traces PUBLIC psapi)
endif()

add_executable(DIATraceExtractor src/DIATraceExtractor.cpp)
target_link_libraries(DIATraceExtractor PRIVATE odia_traces OpenMS_CLI)
target_compile_options(DIATraceExtractor PRIVATE ${ODIA_CXX_OPTIONS})

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Comparison helper: OpenSWATH .xic -> trace layout used by scripts/compare_traces.py.
add_executable(OpenSwathXicToTraces src/OpenSwathXicToTraces.cpp)
target_link_libraries(OpenSwathXicToTraces PRIVATE OpenMS ${ODIA_ARROW} ${ODIA_PARQUET})
target_compile_options(OpenSwathXicToTraces PRIVATE ${ODIA_CXX_OPTIONS})

# Helper: mzML -> mzPeak (point layout, split metadata) with the mzpeak-openms streaming writer.
add_executable(MzMLToMzPeak src/MzMLToMzPeak.cpp)
target_link_libraries(MzMLToMzPeak PRIVATE odia_traces)
target_compile_options(MzMLToMzPeak PRIVATE ${ODIA_CXX_OPTIONS})

# ---------------------------------------------------------------------------------------------------------------
# Install: a self-contained tree for the binary release (ci/release.yml, ci/package.sh).
#   bin/DIATraceExtractor            launcher (Linux, macOS: sets the library path and OpenMS's data path)
#   libexec/DIATraceExtractor        the tool            (Windows: bin/DIATraceExtractor.exe, DLLs beside it)
#   lib/                             the shared libraries the tool links (OpenMS, Arrow, Boost, ...): collected by
#                                    CMake's runtime-dependency resolution, system libraries excluded
#   share/OpenMS/                    OpenMS's shared data (ODIA_OPENMS_SHARE; the tool refuses to start without it)
# ---------------------------------------------------------------------------------------------------------------
set(ODIA_OPENMS_SHARE "" CACHE PATH "OpenMS share/OpenMS directory to bundle (default: next to the OpenMS build or source tree)")
if(NOT ODIA_OPENMS_SHARE)
  foreach(candidate "${OpenMS_DIR}/share/OpenMS" "${OpenMS_DIR}/../share/OpenMS" "${OpenMS_DIR}/../../share/OpenMS")
    if(EXISTS "${candidate}/CHEMISTRY")
      set(ODIA_OPENMS_SHARE "${candidate}")
      break()
    endif()
  endforeach()
endif()
set(ODIA_DEP_DIRS "" CACHE STRING "Extra directories searched for the tool's shared-library dependencies at install time (semicolon list)")

if(WIN32)
  install(TARGETS DIATraceExtractor RUNTIME_DEPENDENCY_SET odia_deps RUNTIME DESTINATION bin)
  set(ODIA_DEP_DESTINATION bin)
else()
  install(TARGETS DIATraceExtractor RUNTIME_DEPENDENCY_SET odia_deps RUNTIME DESTINATION libexec)
  install(PROGRAMS packaging/launcher.sh DESTINATION bin RENAME DIATraceExtractor)
  set(ODIA_DEP_DESTINATION lib)
endif()
install(RUNTIME_DEPENDENCY_SET odia_deps
  DESTINATION ${ODIA_DEP_DESTINATION}
  PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-.*" "^hvsifiletrust" "^pdmutilities"
  # the C library and the dynamic loader stay the user's; the C++ runtime and OpenMP are bundled
  POST_EXCLUDE_REGEXES ".*[\\\\/][Ss]ystem32[\\\\/].*\\.dll"
                       "(^|/)(ld-linux[^/]*|libc\\.so[^/]*|libm\\.so[^/]*|libpthread[^/]*|libdl\\.so[^/]*|librt\\.so[^/]*|libresolv[^/]*|libutil\\.so[^/]*|libnsl[^/]*)$"
                       "^/System/" "^/usr/lib/.*\\.dylib"
  DIRECTORIES ${ODIA_DEP_DIRS})
if(MSVC)
  # App-local Visual C++ runtime and MSVC's OpenMP (vcomp140): the dependency collector above skips them because
  # they resolve to System32 on the build machine.
  set(CMAKE_INSTALL_OPENMP_LIBRARIES ON)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
  set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
  include(InstallRequiredSystemLibraries)
endif()
if(ODIA_OPENMS_SHARE)
  install(DIRECTORY "${ODIA_OPENMS_SHARE}/" DESTINATION share/OpenMS)
else()
  message(WARNING "ODIA_OPENMS_SHARE not found: the installed tree will lack share/OpenMS and the tool will not start from it")
endif()
# the instrument defaults (src/Instruments.cpp finds them at <prefix>/share/DIATraceExtractor/instruments)
install(DIRECTORY data/instruments/ DESTINATION share/DIATraceExtractor/instruments FILES_MATCHING PATTERN "*.yaml")
install(FILES README.md LICENSE DESTINATION . OPTIONAL)
