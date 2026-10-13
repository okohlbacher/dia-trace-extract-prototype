# OpenMS's x64-windows-static-md-release triplet (static dependencies, dynamic CRT, release only) (kept as an overlay so
# the host and target triplets come from one place).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_BUILD_TYPE release)
