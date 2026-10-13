# OpenMS's x64-windows-static-md-release triplet (static dependencies, dynamic CRT, release only) with the
# Visual Studio 2022 toolset pinned (vs2022.cmake).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_BUILD_TYPE release)
include(${CMAKE_CURRENT_LIST_DIR}/vs2022.cmake)
