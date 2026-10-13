# Integration tests on OpenMS's own OpenSWATH test data (testdata/openms).
set(TD ${PROJECT_SOURCE_DIR}/testdata/openms)
# The comparison scripts need pyarrow: the project's tools/pyenv when it exists, else the python3 on PATH.
if(EXISTS ${PROJECT_SOURCE_DIR}/tools/pyenv/bin/python)
  set(PY ${PROJECT_SOURCE_DIR}/tools/pyenv/bin/python)
else()
  find_program(ODIA_PYTHON NAMES python3 python REQUIRED)
  set(PY ${ODIA_PYTHON})
endif()
set(OPENMS_BIN ${OpenMS_DIR}/bin)

# Same extraction as OpenSwathWorkflow with -rt_extraction_window -1, 100 ppm full width (= +-50 ppm),
# 2 MS1 isotopes at OpenSWATH's (charge-independent) spacing.
add_test(NAME extract_traml
  COMMAND DIATraceExtractor -in ${TD}/OpenSwathWorkflow_1_input.mzML -lib ${TD}/OpenSwathWorkflow_1_input.TraML
          -rt_window -1 -mz_tol 50 -mz_tol_ms1 50 -top_fragments 0 -scheme B
          -out traml.parquet -out_tsv traml.tsv -threads 4)
add_test(NAME openswath_reference
  COMMAND ${OPENMS_BIN}/OpenSwathWorkflow -in ${TD}/OpenSwathWorkflow_1_input.mzML -tr ${TD}/OpenSwathWorkflow_1_input.TraML
          -out_chrom osw.xic -rt_extraction_window -1 -mz_extraction_window 100 -mz_extraction_window_unit ppm
          -mz_extraction_window_ms1 100 -mz_extraction_window_ms1_unit ppm -ms1_isotopes 2
          -Calibration:auto_irt:enabled false -out_features osw.featureXML -threads 2)
add_test(NAME openswath_explode COMMAND ${OPENMS_BIN}/TextExporter -in osw.xic -out osw_explode.tsv -xic:mode explode)
set_tests_properties(openswath_explode PROPERTIES DEPENDS openswath_reference)
add_test(NAME parity_with_openswath
  COMMAND ${PY} ${PROJECT_SOURCE_DIR}/scripts/compare_openswath_xic.py traml.tsv osw_explode.tsv --tol 1e-3)
set_tests_properties(parity_with_openswath PROPERTIES DEPENDS "extract_traml;openswath_explode")

# Output must not depend on the thread count.
add_test(NAME determinism_t1
  COMMAND DIATraceExtractor -in ${TD}/OpenSwathWorkflow_1_input.mzML -lib ${TD}/OpenSwathWorkflow_1_input.TraML
          -rt_window -1 -mz_tol 50 -mz_tol_ms1 50 -top_fragments 0 -mass_calibration off -unit_precursors 1 -out det1.parquet -threads 1)
add_test(NAME determinism_t8
  COMMAND DIATraceExtractor -in ${TD}/OpenSwathWorkflow_1_input.mzML -lib ${TD}/OpenSwathWorkflow_1_input.TraML
          -rt_window -1 -mz_tol 50 -mz_tol_ms1 50 -top_fragments 0 -mass_calibration off -unit_precursors 1 -out det8.parquet -threads 8)
add_test(NAME determinism_compare COMMAND ${PY} ${PROJECT_SOURCE_DIR}/scripts/compare_parquet.py det1.parquet det8.parquet)
set_tests_properties(determinism_compare PROPERTIES DEPENDS "determinism_t1;determinism_t8")
