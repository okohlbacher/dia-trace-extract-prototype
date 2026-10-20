**The evidence does not support making the running window the default or enabling the proposed stopping rule.** Metadata-only integration is a separate claim. The review below uses the supplied code and tables; repository access failed, so I could not inspect integration or reproduce results.

Ranked by consequence:

1. **Critical — the proposed anchor reduction contradicts the coverage evidence. Certain.**  
   At 300 anchors, the worst configuration’s 5th-percentile coverage is **97.0%**, against a **99.4%** target; even 1,000 gives 97.9%. Median convergence to the full-anchor curve does not establish safe extraction. The reported 35–60% endpoint errors at 300 also contradict treating “300 anchors” as sufficient. Require coverage validation of the **actual stopped procedure**, including adverse draws and endpoint regions, before reducing anchors.

2. **Critical — the stopping rule measures agreement, not accuracy. Certain.**  
   Successive cumulative batches share most observations, so their estimates can change little while both are wrong. Unseen tail observations cannot influence the stop. Repeated opportunities to stop select apparently stable prefixes; the direction of resulting bias is not universally downward, but rare-tail omission makes undercoverage a concrete risk. This is not a formal hypothesis test, so merely applying a multiple-testing correction would not fix it.  
   Specify fixed comparison RTs, uncertainty bounds, all quantities affecting extraction, and fallback behavior. Validate the entire adaptive algorithm over many panel orders on held-out runs. If claiming a statistical guarantee, use a procedure valid under optional stopping.

3. **High — the implementation does not select the nearest anchors in RT. Certain code defect.**  
   `lower_bound` followed by taking roughly `k/2` anchors on each side selects a **rank-balanced window**, not the `k` smallest RT distances. With uneven density or gaps, it can include distant anchors while excluding closer ones. Correct the implementation or change the documented estimator and validate that estimator.

4. **High — endpoint estimates are smoothed inward; constant extrapolation has no demonstrated safety. Certain mechanism; bias direction depends on the run.**  
   At the 5th-percentile grid point with a 20% span, the estimator uses approximately the first 20% of anchors. It represents an extended, asymmetric region rather than the local endpoint. Rapidly varying widths/scales are blurred; constant continuation beyond 5–95% adds unsupported extrapolation. More anchors do not remove this smoothing bias when the span stays 20%.  
   Report neighborhood RT extents and endpoint support. Test gradients, density gaps and sparse tails; use conservative endpoint fallback windows until validated.

5. **High — “99th percentile converges at 300 anchors” overstates the evidence. Certain.**  
   At 300 total anchors, each neighborhood has about **60** observations: its interpolated 99th percentile lies between the largest two deviations. Under ideal independent sampling, the probability that all 60 observations lie below the population 99th percentile is \(0.99^{60}\approx55\%\). Even 150 local observations gives about 22%. Approximately 299 are needed merely for the sample maximum to exceed that percentile with 95% probability. This is an illustrative best-case calculation, not a guarantee for these fitted residuals. [NIST distinguishes percentile estimates from population coverage with stated confidence.](https://itl.nist.gov/div898/handbook/prc/section2/prc262.htm)  
   Overlapping neighborhoods and comparison against a shared full sample make curve agreement still less informative. Keep `tail99` descriptive unless uncertainty is quantified.

6. **High — anchor residuals cannot establish target-precursor coverage. Certain.**  
   Confident anchors are selected for detectability, co-elution and agreement with the current search windows. If they also fit the LOWESS map, their residuals are in-sample and potentially optimistic. Stage-2 RT/mass restrictions further censor difficult anchors. The heavier identification tail is consistent with selection bias, but these tables do not isolate its causes.  
   Use precursor-grouped cross-fitting for residual diagnostics and independent runs for window calibration. Ten configurations include repeated runs/libraries, so they are not ten independent generalization tests. DIA-NN IDs also define a selected reference population, not all true precursors.

7. **High — intensity-dependent tolerance has an unvalidated selection loop. Certain risk; magnitude unknown.**  
   Narrow stage-2 tolerances can remove weak or high-error matches, making the estimated width look smaller and supporting further narrowing. Stable offset/width does not detect this failure. Likewise, intensity-dependent scatter alone does not prove that accepting by observed centroid intensity improves extraction: intensity also reflects interference. “Equivalent to down-weighting” is false for downstream trace formation.  
   Estimate using a deliberately broad, fixed diagnostic search; inspect truncation and false matches. Define intensity normalization, sparse-decile fallback, interpolation, tolerance floors/caps and offset handling. Validate signal recovery, interference and quantitative bias by intensity—not just agreement with DIA-NN.

8. **High — fixed seed does not ensure thread determinism. Certain.**  
   Specify canonical candidate ordering, tie-breaking, batch membership, deterministic reductions and batch barriers. Stop only after a complete predetermined batch. RT tiling additionally needs overlap and deduplication so boundary peaks retain their apex, FWHM and co-elution context. Test anchor identities and models as well as final output across thread counts.

9. **Medium — public estimator inputs permit failures or misleading output. Certain.**  
   `span=0, kmin=0` produces empty neighborhoods and invalid median access. Nonfinite inputs are unchecked; nonfinite RT breaks sorting assumptions, and serialization can emit invalid JSON. `at()` silently returns zero for missing/mismatched curves—a dangerous future window fallback. Validate domains and expose unavailable estimates explicitly. Also, `trimmedMean` includes all ties at quartile cutoffs, so it is not a fixed central-50% trimmed mean; quantized FWHM makes that distinction relevant.

10. **Medium — several conclusions and validation criteria are too strong. Certain.**  
    The displayed arithmetic supports RT residuals dominating the current window. It does **not** establish Gaussian tails, general irrelevance of RT mass drift, or representative stage-1 FWHM. Small aggregate narrowing can hide local benefit. FWHM also does not guarantee whole-peak boundary coverage. A 0.1 ppm stability threshold has very different meaning at 0.4 versus 2.75 ppm widths.

**Change the order:** fix estimator semantics and input contracts; reproduce archived analyses; establish independent coverage and mass-error diagnostics; then validate B. Split C into deterministic RT tiling, stage-1 reduction and sequential stopping so regressions are attributable. A needs valid mass observations and provenance even though it only writes metadata. D changes anchor selection and requires recalibration of subsequent models.

For release gates, require per-run and stratified coverage with uncertainty, endpoint behavior, failure fallbacks, and extraction/scoring performance. Mean width no greater than bins and pooled 99.4% coverage are insufficient. The current evidence supports **retaining conservative extraction defaults while collecting diagnostics**.
