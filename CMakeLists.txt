cmake_minimum_required(VERSION 3.24)
project(ODIATraces VERSION 0.1.0 LANGUAGES CXX)

# ODIA-V3 DIA trace extractor: a standalone TOPP-style tool built against an
# OpenMS (>= 3.7, develop) build or install. Configure with
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_PREFIX_PATH="<OpenMS-build>;<OpenMS-build>/vcpkg_installed/x64-linux-dynamic"

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

add_library(odia_traces STATIC
  src/Library.cpp
  src/MzMLFast.cpp
  src/Profile.cpp
  src/RtMap.cpp
  src/RunGrid.cpp
  src/ArrowPool.cpp
  src/BufferPool.cpp
  src/Extractor.cpp
  src/Scheduler.cpp
  src/TraceWriter.cpp
  src/Calibration.cpp)
target_include_directories(odia_traces PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# mzPeak reader/writer: the mzpeak-openms library (git submodule external/mzpeak, tag partitioned-read-2026-10-11),
# built here against the same Arrow as OpenMS. It needs Boost >= 1.89 with json and program_options, which the
# OpenMS vcpkg tree lacks: point MZPEAK_BOOST_DIR at a BoostConfig.cmake directory that has them.
set(MZPEAK_BOOST_DIR "" CACHE PATH "Directory of a BoostConfig.cmake (Boost >= 1.89 with json, program_options) for external/mzpeak")
if(MZPEAK_BOOST_DIR)
  set(Boost_DIR "${MZPEAK_BOOST_DIR}" CACHE PATH "" FORCE)
endif()
add_subdirectory(external/mzpeak EXCLUDE_FROM_ALL)
target_link_libraries(odia_traces PUBLIC OpenMS ${ODIA_ARROW} ${ODIA_PARQUET} OpenMP::OpenMP_CXX ZLIB::ZLIB mzpeak)
target_compile_options(odia_traces PRIVATE -Wall -Wextra)

add_executable(DIATraceExtractor src/DIATraceExtractor.cpp)
target_link_libraries(DIATraceExtractor PRIVATE odia_traces OpenMS_CLI)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Comparison helper: OpenSWATH .xic -> trace layout used by scripts/compare_traces.py.
add_executable(OpenSwathXicToTraces src/OpenSwathXicToTraces.cpp)
target_link_libraries(OpenSwathXicToTraces PRIVATE OpenMS ${ODIA_ARROW} ${ODIA_PARQUET})

# Helper: mzML -> mzPeak (point layout, split metadata) with the mzpeak-openms streaming writer.
add_executable(MzMLToMzPeak src/MzMLToMzPeak.cpp)
target_link_libraries(MzMLToMzPeak PRIVATE odia_traces)
