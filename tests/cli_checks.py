#!/usr/bin/env python3
"""Command-line checks of DIATraceExtractor (docs/HANDOFF-instrument-defaults-and-cli.md section 6): help, unknown
options, instrument files, value sources, the 0.2.14 names on the command line and in INI files, -write_ini.
usage: cli_checks.py CHECK EXE TESTDATA_DIR SOURCE_DIR      (run in the test's working directory)"""
import json, os, re, subprocess, sys

check, exe, td, src = sys.argv[1:5]
IN = os.path.join(td, "OpenSwathWorkflow_1_input.mzML")
LIB = os.path.join(td, "OpenSwathWorkflow_1_input.TraML")
# whole run, fixed tolerances: no calibration needed on this small file
FIXED = ["-rt_window", "-1", "-mz_tol_ms2", "20", "-mz_tol_ms1", "20", "-threads", "2"]
# pinned: the options `--help` lists (the tool's 14 + TOPP's 5; change deliberately), not its line count:
# the framing around them differs between platforms.
HELP_OPTIONS = 19


def run(*args, ok=True):
    p = subprocess.run([exe, *args], capture_output=True, text=True)
    out = p.stdout + p.stderr
    if ok and p.returncode != 0:
        sys.exit(f"exit {p.returncode} for {' '.join(args)}:\n{out}")
    return p.returncode, out


def expect(cond, what, out=""):
    if not cond:
        sys.exit(f"FAILED: {what}\n{out}")
    print("ok:", what)


def extract(name, *extra):
    run("-in", IN, "-lib", LIB, *FIXED, "-out", f"{name}.parquet", "-out_stats", f"{name}.json", *extra)
    return json.load(open(f"{name}.json"))


def same_table(a, b):
    import pyarrow.parquet as pq
    return pq.read_table(a).equals(pq.read_table(b))


def write_yaml(name, text):
    with open(name, "w", newline="\n") as f:
        f.write(text)
    return name


if check == "help":
    _, out = run("--help")
    listed = [l.split()[0] for l in out.splitlines() if re.match(r"^  --?[a-z]", l)]
    expect(len(listed) == HELP_OPTIONS, f"--help lists {HELP_OPTIONS} options (lists {len(listed)}: {' '.join(listed)})", out)
    expect("-instrument <name>" in out, "--help lists -instrument")
    _, out = run("--helphelp")
    expect("-instrument_file" in out, "--helphelp lists -instrument_file")
    expect(re.search(r"-mz_tol <value>\s+Deprecated", out) is not None, "--helphelp lists the deprecated -mz_tol")

elif check == "unknown_option":
    rc, out = run("-in", IN, "-mz_tolx", "5", "-threadz", "2", ok=False)
    expect(rc == 6, "exit code 6", out)
    expect("Unknown option '-mz_tolx' (did you mean -mz_tol_ms2?)." in out, "suggests the current name, not the deprecated one", out)
    expect("Unknown option '-threadz' (did you mean -threads?)." in out, "one line per unknown option", out)
    expect("Options (mandatory" not in out, "no usage text", out)

elif check == "instruments":
    s = extract("auto")
    v = s["values"]
    expect(s["instrument"]["name"] == "default", "an unknown instrument (LCQ Deca) gets default.yaml")
    expect(v["mz_accuracy_ms2"] == {"value": "6", "source": "default"}, "default fragment multiple 6", json.dumps(v))
    s = extract("astral", "-instrument", "Orbitrap Astral", "-mz_accuracy_ms1", "5")
    v = s["values"]
    expect(s["instrument"]["name"] == "Astral", "-instrument by alias")
    expect(v["mz_accuracy_ms2"] == {"value": "9", "source": "instrument-default:Astral"}, "Astral 9 from astral.yaml", json.dumps(v))
    expect(v["mz_accuracy_ms1"] == {"value": "5", "source": "given"}, "an option given overrides only itself", json.dumps(v))
    expect(v["rt_window_sigma"]["source"] == "default", "values no instrument file sets come from default.yaml", json.dumps(v))
    expect(v["mz_tol_ms2"] == {"value": "20", "source": "given"}, "a fixed tolerance is recorded as given", json.dumps(v))
    for name, inst in (("timsTOF Pro 2", "timsTOF"), ("Bruker timsTOF HT", "timsTOF"), ("Exploris 480", "Orbitrap"), ("Q-TOF", "TOF")):
        expect(extract("named", "-instrument", name)["instrument"]["name"] == inst, f"-instrument '{name}' -> {inst}")
    # several files match: the order of the class rule of 0.2.16 (timsTOF, then the Orbitraps, then other TOFs)
    for name, inst in (("tims Astral", "timsTOF"), ("tims orbitrap", "timsTOF"), ("Orbitrap TOF", "Orbitrap"), ("Astral TOF", "Astral")):
        expect(extract("mixed", "-instrument", name)["instrument"]["name"] == inst, f"-instrument '{name}' -> {inst} (priority)")
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet", "-instrument", "Fancy", ok=False)
    expect(rc == 6 and "unknown instrument 'Fancy'" in out, "an unknown -instrument name is an error", out)
    own = write_yaml("exploris.yaml", "name: Exploris\nextends: orbitrap\nmz_accuracy_ms2: 7\n")
    s = extract("own", "-instrument_file", own)
    expect(s["values"]["mz_accuracy_ms2"] == {"value": "7", "source": "instrument-default:Exploris"}, "-instrument_file over the shipped files", json.dumps(s["values"]))
    expect("orbitrap.yaml sha1:" in s["instrument"]["files"], "the files and their hashes are recorded", s["instrument"]["files"])
    bad = {
        "unknown key": ("name: X\nmz_acuracy_ms2: 7\n", "unknown key"),
        "duplicate key": ("name: X\nmz_accuracy_ms2: 7\nmz_accuracy_ms2: 8\n", "given twice"),
        "wrong type": ("name: X\nmz_accuracy_ms2: seven\n", "expected a number"),
        "out of range": ("name: X\nmz_accuracy_ms2: 0\n", "out of range"),
        "missing name": ("mz_accuracy_ms2: 7\n", "'name': missing"),
        "unknown parent": ("name: X\nextends: nothing\n", "no instrument file 'nothing'"),
        "not YAML": ("name: [X\n", "not valid YAML"),
    }
    for what, (text, msg) in bad.items():
        f = write_yaml("bad.yaml", text)
        rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet", "-instrument_file", f, ok=False)
        expect(rc == 6 and msg in out and "bad.yaml" in out, f"instrument file error: {what}", out)

elif check == "compatibility":
    # the 0.2.14 names give the same traces as the current ones
    run("-in", IN, "-lib", LIB, "-rt_window", "-1", "-mz_tol", "20", "-mz_tol_ms1", "20", "-mz_sigma_ms1", "4", "-out_shards", "4", "-threads", "2", "-out", "old.parquet")
    run("-in", IN, "-lib", LIB, "-rt_window", "-1", "-mz_tol_ms2", "20", "-mz_tol_ms1", "20", "-write_parts", "4", "-threads", "2", "-out", "new.parquet")
    expect(same_table("old.parquet", "new.parquet"), "old and new option names: identical traces")
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-mz_tol", "5", "-out", "x.parquet", ok=False)
    expect(rc == 6 and "different values" in out, "an old and a new name with different values is an error", out)
    # an old name in an INI counts as given, also at its old default: it overrides an instrument file
    ini = open(os.path.join(src, "docs", "cli-review", "defaults-0.2.14.ini")).read()
    expect('name="mz_sigma_ms1" value="4.0"' in ini, "fixture: the 0.2.14 INI has mz_sigma_ms1 4.0")
    own = write_yaml("ms1.yaml", "name: Wide\nextends: orbitrap\nmz_accuracy_ms1: 8\n")
    s = extract("oldini", "-ini", os.path.join(src, "docs", "cli-review", "defaults-0.2.14.ini"), "-instrument_file", own)
    expect(s["values"]["mz_accuracy_ms1"] == {"value": "4", "source": "given"}, "old INI value (at its old default) over the instrument file", json.dumps(s["values"]))
    # an INI written by 0.2.14 runs
    rc, out = run("-ini", os.path.join(src, "docs", "cli-review", "defaults-0.2.14.ini"), "-in", IN, "-lib", LIB, *FIXED, "-out", "ini.parquet")
    expect(rc == 0, "an INI written by 0.2.14 runs")

elif check == "write_ini":
    run("-write_ini", "new.ini")
    text = open("new.ini").read()
    expect(not re.search(r'name="(mz_tol|mz_sigma_ms2|aggregation|out_shards|calibration:anchor_fdr)"', text), "-write_ini leaves the 0.2.14 names out")
    expect('name="instrument"' in text and 'name="instrument_file"' in text, "-write_ini has the new options")
    # round trip: the written INI runs and gives the defaults' traces
    run("-ini", "new.ini", "-in", IN, "-lib", LIB, *FIXED, "-out", "roundtrip.parquet")
    run("-in", IN, "-lib", LIB, *FIXED, "-out", "plain.parquet")
    expect(same_table("roundtrip.parquet", "plain.parquet"), "-write_ini round trip: identical traces")
    # an old INI rewritten: values on the new names
    old = open(os.path.join(src, "docs", "cli-review", "defaults-0.2.14.ini")).read()
    for k, v in (("mz_tol", "6.0"), ("rt_mode", "identity"), ("aggregation", "sum"), ("ms1_isotope_spacing", "unit")):
        old, n = re.subn(rf'(name="{k}" value=")[^"]*"', rf'\g<1>{v}"', old)
        expect(n == 1, f"fixture has {k}")
    open("old.ini", "w").write(old)
    run("-write_ini", "migrated.ini", "-ini", "old.ini")
    m = open("migrated.ini").read()
    # the current name with another value in the same INI is kept (and reported)
    both = re.sub(r'(\s*)(<ITEM name="mz_tol" [^\n]*/>)', r'\1\2\1<ITEM name="mz_tol_ms2" value="20.0" type="double" description="" required="false" advanced="false" />', old, count=1)
    expect('name="mz_tol_ms2" value="20.0"' in both, "fixture: an INI with mz_tol 6 and mz_tol_ms2 20")
    open("both.ini", "w").write(both)
    rc, out = run("-write_ini", "both_migrated.ini", "-ini", "both.ini")
    expect('name="mz_tol_ms2" value="20.0"' in open("both_migrated.ini").read(), "-write_ini keeps the current name's value on a conflict")
    expect("mz_tol (its current name mz_tol_ms2 has another value, kept)" in out, "-write_ini reports the conflict", out)
    for k, v in (("mz_tol_ms2", "6.0"), ("rt_mode", "seconds"), ("scheme", "B")):
        expect(f'name="{k}" value="{v}"' in m, f"-write_ini -ini <0.2.14 INI>: {k} = {v}")
    expect('name="mz_tol"' not in m and 'name="aggregation"' not in m, "-write_ini -ini <0.2.14 INI>: no old names")

else:
    sys.exit(f"unknown check {check}")
