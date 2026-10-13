# vcpkg overlay ports for the release build

- `libiconv`: microsoft/vcpkg's port at commit f6672d8e (1.19, 2026-06-05), ahead of the baseline OpenMS pins
  (f9ffbaa, 1.18#4). The 1.18 port's static build fails to link its iconv program under the Visual Studio 2026
  toolset, the only toolset on the hosted Windows runners since September 2026.
