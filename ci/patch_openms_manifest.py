#!/usr/bin/env python3
"""Adjust an OpenMS checkout's vcpkg.json for the release build, idempotently:
  - add the ports the mzPeak reader needs (Boost.JSON, Boost.ProgramOptions);
  - build Arrow without S3 (aws-sdk-cpp: unused here, an hour of build, long-path failures on Windows);
  - with --no-coin: drop the COIN-OR ports (they pull lapack and openblas, whose arm64-windows host build is
    broken); OpenMS then takes its LP solver from the lp-glpk feature (-DLP_SOLVER=GLPK).
usage: patch_openms_manifest.py OPENMS_DIR [--no-coin]"""
import json, sys
openms = sys.argv[1].rstrip("/")
no_coin = "--no-coin" in sys.argv[2:]
p = openms + "/vcpkg.json"
m = json.load(open(p))
deps = m["dependencies"]
names = [d if isinstance(d, str) else d["name"] for d in deps]
changes = []
for d in ("boost-json", "boost-program-options"):
    if d not in names:
        deps.append(d); changes.append("+" + d)
for d in deps:
    if isinstance(d, dict) and d.get("name") == "arrow" and "s3" in d.get("features", []):
        d["features"] = [f for f in d["features"] if f != "s3"]; changes.append("arrow without s3")
if no_coin:
    coin = [d for d in deps if (d if isinstance(d, str) else d["name"]).startswith(("coin-or-", "coinutils"))]
    if coin:
        m["dependencies"] = [d for d in deps if d not in coin]; changes.append("-coin-or (%d ports)" % len(coin))
json.dump(m, open(p, "w"), indent=2)
print("changed:" if changes else "already patched", ", ".join(changes))
