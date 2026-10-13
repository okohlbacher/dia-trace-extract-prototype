# vcpkg overlay triplet for the Windows arm64 release build: the arm64 twin of OpenMS's
# x64-windows-static-md-release (static dependencies, dynamic CRT, release only), Visual Studio 2022 toolset.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_BUILD_TYPE release)
include(${CMAKE_CURRENT_LIST_DIR}/vs2022.cmake)
