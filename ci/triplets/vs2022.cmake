# Pin vcpkg's Windows builds to the Visual Studio 2022 toolset (v143). The hosted runners also carry Visual
# Studio 2026 (toolset 14.5x), which vcpkg picks by default; libiconv's static build fails to link under it
# (vcpkg port 1.18#4, 2026-10), and OpenMS's own CI never compiles it there (binary cache). The tool and OpenMS
# are compiled with the same toolset (ilammy/msvc-dev-cmd, vsversion 2022) so every object comes from v143.
set(VCPKG_PLATFORM_TOOLSET v143)
foreach(edition Enterprise Professional Community BuildTools)
  if(EXISTS "C:/Program Files/Microsoft Visual Studio/2022/${edition}")
    set(VCPKG_VISUAL_STUDIO_PATH "C:/Program Files/Microsoft Visual Studio/2022/${edition}")
    break()
  endif()
endforeach()
