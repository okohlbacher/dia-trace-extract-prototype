#!/usr/bin/env python3
"""OpenMS on Windows arm64 needs two source fixes the develop tree lacks (idempotent; usage: patch_openms_arm64.py OPENMS_DIR):
  1. cmake/Modules/CheckLibArchitecture.cmake knows x64 and x86 only; every vcpkg library reports
     'AA64 machine (ARM64)' and the configure aborts.
  2. src/openms/thirdparty/percolator/Globals.cpp includes <windows.h> inside the OpenMS::Internal::Percolator
     namespace; on ARM64 winbase.h's inline Interlocked wrappers then conflict with the intrinsics it declares
     (C2664/C2665). The include block moves above the namespace."""
import sys, re
openms = sys.argv[1].rstrip("/")

p = openms + "/cmake/Modules/CheckLibArchitecture.cmake"
s = open(p).read()
old = 'set(M_64bit "Machine      : 8664 \\\\(x64\\\\)|8664 machine \\\\(x64\\\\)")'
new = 'set(M_64bit "Machine      : 8664 \\\\(x64\\\\)|8664 machine \\\\(x64\\\\)|Machine      : AA64 \\\\(ARM64\\\\)|AA64 machine \\\\(ARM64\\\\)")'
if new in s: print("CheckLibArchitecture: already patched")
elif old in s: open(p, "w").write(s.replace(old, new)); print("CheckLibArchitecture: ARM64 accepted as 64-bit")
else: sys.exit("CheckLibArchitecture.cmake: expected line not found")

p = openms + "/src/openms/thirdparty/percolator/Globals.cpp"
s = open(p).read()
block = "#if defined (__WIN32__) || defined (__MINGW__) || defined (MINGW) || defined (_WIN32)\n#include <windows.h>\n#include <tchar.h>\n#endif\n"
ns = "namespace OpenMS { namespace Internal { namespace Percolator {\n"
if s.find(block) != -1 and s.find(block) < s.find(ns): print("Globals.cpp: already patched")
elif block in s and ns in s:
    s = s.replace(block, "", 1).replace(ns, block + "\n" + ns, 1)
    open(p, "w").write(s); print("Globals.cpp: <windows.h> moved out of the namespace")
else: sys.exit("Globals.cpp: expected include block or namespace not found")
