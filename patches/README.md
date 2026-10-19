# Patches to third-party tools used by the benchmark

- `diann-1.7.12-xic-export-and-gcc-build.patch` — against DIA-NN 1.7.12 source
  (https://github.com/vdemichev/DiaNN, commit cff0408, © Vadim Demichev, CC BY 4.0). Two fixes to the
  `--xic` export (buffer sized before the fragment list was known → heap overflow; MS1 intensity block
  overwritten by the MS2 RTs) and four braces for GCC ≥ 8. A third export defect (stored addresses ignore
  the per-group element count) is handled in `scripts/diann17_xic_to_parquet.py`. Identification is unchanged.
  Build: `tools/DiaNN-1.7.12/linux-build-gcc-7` flags, output `diann-1.7.12-xicfix`.
- `dialibgen-0.11.0-openms-develop-compat.patch` — against DIALibGen 0.11.0
  (https://github.com/okohlbacher/DIALibGen, 7ac9654): OpenMS develop (3.7) API changes (`OpenMS::String`
  removed, `TOPPBase` moved to `OpenMS_CLI` with a new constructor signature).
- `openms-toppbase-deprecated-options.patch` — against OpenMS develop (`ci/openms.lock`), applied by the release
  workflow and needed to build `DIATraceExtractor` from 0.3.0. `TOPPBase::setDeprecated_()` marks a registered
  option as deprecated (still accepted on the command line and in INI files, left out of `-write_ini` and the tool
  descriptions, never suggested for a typo); `setDeprecatedMigration_()` moves an old INI's values onto the current
  names when `-write_ini` rewrites it; an unknown option prints one line per option with the closest current name
  (`Unknown option '-mz_tolx' (did you mean -mz_tol_ms2?).`) instead of the whole usage text. `--helphelp` names the deprecated
  options once at the end instead of listing them (from 0.3.1). ABI-neutral (new
  non-virtual functions, state kept in TOPPBase.cpp), so tools built against the unpatched library keep running.
  Local build: `git -C OpenMS apply ../patches/openms-toppbase-deprecated-options.patch`, then rebuild `OpenMS_CLI`.
  Meant for upstream.
