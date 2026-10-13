#!/usr/bin/env python3
"""Add the vcpkg ports the mzPeak reader needs (Boost.JSON, Boost.ProgramOptions) to an OpenMS checkout's
vcpkg.json, idempotently.  usage: patch_openms_manifest.py OPENMS_DIR"""
import json, sys
p = sys.argv[1].rstrip("/") + "/vcpkg.json"
m = json.load(open(p))
deps = m["dependencies"]
names = [d if isinstance(d, str) else d["name"] for d in deps]
added = [d for d in ("boost-json", "boost-program-options") if d not in names]
deps.extend(added)
json.dump(m, open(p, "w"), indent=2)
print("added" if added else "already present", added or "")
