#!/usr/bin/env python3
"""OpenMS's MSVC link-library architecture check (cmake/Modules/CheckLibArchitecture.cmake) knows x64 and x86 only;
on Windows arm64 every vcpkg library reports 'AA64 machine (ARM64)' and the check aborts the configure. Teach it
ARM64 (idempotent).  usage: patch_openms_arm64.py OPENMS_DIR"""
import sys
p = sys.argv[1].rstrip("/") + "/cmake/Modules/CheckLibArchitecture.cmake"
s = open(p).read()
old = 'set(M_64bit "Machine      : 8664 \\\\(x64\\\\)|8664 machine \\\\(x64\\\\)")'
new = 'set(M_64bit "Machine      : 8664 \\\\(x64\\\\)|8664 machine \\\\(x64\\\\)|Machine      : AA64 \\\\(ARM64\\\\)|AA64 machine \\\\(ARM64\\\\)")'
if new in s: print("already patched")
elif old in s: open(p, "w").write(s.replace(old, new)); print("patched: ARM64 accepted as 64-bit")
else: sys.exit("CheckLibArchitecture.cmake: expected line not found")
