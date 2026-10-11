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
  src/Calibration.cpp
  src/mzpeak/MzPeakFile.cpp)
target_include_directories(odia_traces PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The vendored mzPeak reader uses nlohmann/json; OpenMS ships it (header-only) but does not export it.
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
          HINTS ${OpenMS_DIR}/../src/openms/extern/nlohmann_json/include ${OpenMS_DIR}/vcpkg_installed/x64-linux-dynamic/include)
if(NOT NLOHMANN_JSON_INCLUDE_DIR)
  message(FATAL_ERROR "nlohmann/json.hpp not found; set NLOHMANN_JSON_INCLUDE_DIR")
endif()
target_include_directories(odia_traces SYSTEM PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
target_link_libraries(odia_traces PUBLIC OpenMS ${ODIA_ARROW} ${ODIA_PARQUET} OpenMP::OpenMP_CXX ZLIB::ZLIB)
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

# Helper: mzML -> mzPeak with the vendored MzPeakFile writer.
add_executable(MzMLToMzPeak src/mzpeak/MzMLToMzPeak.cpp)
target_link_libraries(MzMLToMzPeak PRIVATE odia_traces)
