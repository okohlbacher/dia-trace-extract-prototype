# vcpkg's arm64-windows host triplet (build-time tools on the arm64 runner), Visual Studio 2022 toolset pinned.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
include(${CMAKE_CURRENT_LIST_DIR}/vs2022.cmake)
