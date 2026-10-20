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
# the tool must not see the tester's own instrument files: an empty user directory unless a check sets one
os.makedirs("no-user-config", exist_ok=True)
os.environ.pop("DIATRACEEXTRACTOR_INSTRUMENT_DIR", None)
for v in ("XDG_CONFIG_HOME", "APPDATA") + (("HOME",) if sys.platform == "darwin" else ()):
    os.environ[v] = os.path.abspath("no-user-config")


def run(*args, ok=True, env=None):
    p = subprocess.run([exe, *args], capture_output=True, text=True, env=None if env is None else {**os.environ, **env})
    out = p.stdout + p.stderr
    if ok and p.returncode != 0:
        sys.exit(f"exit {p.returncode} for {' '.join(args)}:\n{out}")
    return p.returncode, out


def expect(cond, what, out=""):
    if not cond:
        sys.exit(f"FAILED: {what}\n{out}")
    print("ok:", what)


def extract(name, *extra, env=None):
    run("-in", IN, "-lib", LIB, *FIXED, "-out", f"{name}.parquet", "-out_stats", f"{name}.json", *extra, env=env)
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
    expect("-mz_tol <" not in out and "Deprecated names" not in out, "--helphelp has no 0.2.14 names (removed in 0.4)")
    expect(out.count("Calibration from the run itself:") == 1, "--helphelp has one calibration section")

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
    expect(v["mz_tol_intensity_max"] == {"value": "1", "source": "instrument-default:Orbitrap"}, "Astral inherits mz_tol_intensity_max 1 from orbitrap.yaml", json.dumps(v))
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

elif check == "user_instruments":
    # $DIATRACEEXTRACTOR_INSTRUMENT_DIR over the user directory over the shipped files, per file name
    os.makedirs("env", exist_ok=True)
    os.makedirs(os.path.join("cfg", "DIATraceExtractor", "instruments"), exist_ok=True)
    write_yaml(os.path.join("env", "astral.yaml"), "name: Astral\nextends: orbitrap\naliases: [Orbitrap Astral]\nkeywords: [astral]\npriority: 1\nmz_accuracy_ms2: 8\n")
    write_yaml(os.path.join("cfg", "DIATraceExtractor", "instruments", "astral.yaml"), "name: Astral\nextends: orbitrap\nkeywords: [astral]\npriority: 1\nmz_accuracy_ms2: 7\n")
    write_yaml(os.path.join("cfg", "DIATraceExtractor", "instruments", "exploris.yaml"), "name: Exploris\nextends: orbitrap\nkeywords: [exploris]\npriority: 1\nmz_accuracy_ms2: 7\n")
    cfg = os.path.abspath("cfg")
    user = {"XDG_CONFIG_HOME": cfg, "HOME": cfg, "APPDATA": os.path.join(cfg)}
    if sys.platform == "darwin":
        os.makedirs(os.path.join(cfg, "Library", "Application Support"), exist_ok=True)
        if not os.path.exists(os.path.join(cfg, "Library", "Application Support", "DIATraceExtractor")):
            os.symlink(os.path.join(cfg, "DIATraceExtractor"), os.path.join(cfg, "Library", "Application Support", "DIATraceExtractor"))
    s = extract("user", "-instrument", "Astral", env=user)
    expect(s["values"]["mz_accuracy_ms2"] == {"value": "7", "source": "user-file:astral.yaml"}, "the user directory replaces the shipped astral.yaml", json.dumps(s["values"]))
    s = extract("envdir", "-instrument", "Astral", env={**user, "DIATRACEEXTRACTOR_INSTRUMENT_DIR": os.path.abspath("env")})
    expect(s["values"]["mz_accuracy_ms2"]["value"] == "8", "$DIATRACEEXTRACTOR_INSTRUMENT_DIR over the user directory", json.dumps(s["values"]))
    expect("astral.yaml" in s["instrument"]["files"] and "env" in s["instrument"]["files"], "a user's file is recorded with its path", s["instrument"]["files"])
    s = extract("added", "-instrument", "Exploris 480", env=user)
    expect(s["instrument"]["name"] == "Exploris" and s["values"]["mz_accuracy_ms2"]["value"] == "7", "a user's file adds an instrument", json.dumps(s["instrument"]))
    s = extract("noenv", "-instrument", "Astral")
    expect(s["values"]["mz_accuracy_ms2"] == {"value": "9", "source": "instrument-default:Astral"}, "without them the shipped file", json.dumps(s["values"]))
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet", ok=False, env={"DIATRACEEXTRACTOR_INSTRUMENT_DIR": os.path.abspath("missing")})
    expect(rc == 6 and "is not a directory" in out, "a missing $DIATRACEEXTRACTOR_INSTRUMENT_DIR is an error", out)

elif check == "combinations":
    # errors before any work
    for what, args, msg in (
        ("-rt_trafo_invert without -rt_trafo", ["-rt_trafo_invert"], "-rt_trafo_invert needs -rt_trafo"),
        ("two outputs on one path", ["-out", "same.parquet", "-out_axes", "same.parquet"], "name the same file"),
        ("an output over an input", ["-out_tsv", LIB], "would overwrite the input -lib"),
    ):
        rc, out = run("-in", IN, "-lib", LIB, *FIXED, *(["-out", "x.parquet"] if "-out" not in args else []), *args, ok=False)
        expect(rc == 6 and msg in out, f"error: {what}", out)
    rc, out = run("-in", IN, "-lib", LIB, "-mz_calibration", "off", "-out", "x.parquet", ok=False)
    expect(rc == 6 and "-mz_calibration off needs fixed tolerances" in out, "error: -mz_calibration off without tolerances", out)
    # options without effect: one WARNING line
    for what, args, names in (
        ("-no_ms1", ["-no_ms1", "-ms1_isotopes", "5", "-ms1_axis", "cycles"], "-ms1_isotopes, -ms1_axis, -mz_tol_ms1 (-no_ms1)"),
        ("-scheme B", ["-scheme", "B", "-mz_accuracy_ms2", "7"], "-mz_accuracy_ms2 (-scheme B"),
        ("a fixed tolerance", ["-mz_accuracy_ms2", "7"], "-mz_accuracy_ms2 (-mz_tol_ms2 fixes the fragment tolerance)"),
        ("the whole run", ["-rt_mode", "minutes"], "-rt_mode (a negative -rt_window extracts the whole run)"),
    ):
        rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet", *args)
        expect(f"WARNING: without effect here: " in out and names in out, f"warning: {what}", out)
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet")
    expect("without effect" not in out, "no such warning for a plain command", out)
    # only -out_mzml: the library is not opened
    rc, out = run("-in", IN, "-lib", LIB, "-out_mzml", "run.mzML")
    expect("-lib (only -out_mzml is written)" in out and "Library:" not in out, "-out_mzml alone does not open -lib", out)
    # -ids: counted
    open("ids.txt", "w").write("Precursor.Id\nNOT_THERE\nPEPTIDEA\n")
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet", "-ids", "ids.txt")
    expect("1 of the 2 ids of -ids are not in the library" in out, "-ids: unknown ids are counted", out)
    open("none.txt", "w").write("NOT_THERE\n")
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet", "-ids", "none.txt", ok=False)
    expect(rc == 4 and "None of the 1 ids of -ids is in the library" in out, "-ids: none in the library is an error", out)

elif check == "compatibility":
    # the names of 0.2.14 were removed in 0.4: an error with a hint naming the replacement
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-mz_tol", "5", "-out", "x.parquet", ok=False)
    expect(rc == 6 and "-mz_tol was renamed to -mz_tol_ms2" in out and "did you mean -mz_tol_ms2" in out, "an old name: error with a hint", out)
    rc, out = run("-in", IN, "-lib", LIB, *FIXED, "-rt_mode", "identity", "-out", "x.parquet", ok=False)
    expect(rc != 0 and "use -rt_mode seconds" in out, "an old -rt_mode value: error with a hint", out)
    rc, out = run("-ini", os.path.join(src, "docs", "cli-review", "defaults-0.2.14.ini"), "-in", IN, "-lib", LIB, *FIXED, "-out", "ini.parquet", ok=False)
    expect(rc != 0 and "The -ini file is from 0.2.14 or earlier" in out, "an INI of 0.2.14: error with a hint", out)
    rc, out = run("-ms1_isotopes", "-1", "-in", IN, "-lib", LIB, *FIXED, "-out", "x.parquet", ok=False)
    expect(rc != 0, "-ms1_isotopes -1 (0.2.14's -no_ms1) is rejected", out)

elif check == "write_ini":
    run("-write_ini", "new.ini")
    text = open("new.ini").read()
    expect(not re.search(r'name="(mz_tol|mz_sigma_ms2|aggregation|out_shards|calibration:anchor_fdr)"', text), "-write_ini has no 0.2.14 names")
    expect('name="instrument"' in text and 'name="instrument_file"' in text, "-write_ini has the new options")
    # round trip: the written INI runs and gives the defaults' traces
    run("-ini", "new.ini", "-in", IN, "-lib", LIB, *FIXED, "-out", "roundtrip.parquet")
    run("-in", IN, "-lib", LIB, *FIXED, "-out", "plain.parquet")
    expect(same_table("roundtrip.parquet", "plain.parquet"), "-write_ini round trip: identical traces")

else:
    sys.exit(f"unknown check {check}")
