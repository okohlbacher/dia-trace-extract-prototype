# Trace gallery captions (run 01, entrapment tuned library)

Made by `scripts/trace_gallery_features.py` + `scripts/trace_gallery.py` (seed 20261010). Tables are in `results/analysis/gallery_run01_*`.

**Panels.** Each panel is one precursor. Top: its top library fragment. Bottom: MS1 M+0.

**Engines.** OpenSWATH is the thick light orange line and ours B (OpenSWATH rules) the thin dark orange dashed line. DIA-NN 1.7.12 is the
thick light blue line and ours A (DIA-NN rules) the thin dark blue dotted line. When a pair agrees, the thin line lies on the thick one.

**Axes.** RT is in minutes, at the true acquisition times: window spectra for MS2; MS1 scans for DIA-NN and OSW; the global cycle axis for
ours at MS1. Each trace is normalised to its maximum inside DIA-NN's exported span (grey band: 60 scans around DIA-NN's apex). Values
above 1.1 are clipped.

**Annotations.** r and log2 are DIA-NN vs OSW on the shown fragment. rA/D and l1 are ours A vs DIA-NN at MS1. Panel labels are R (random)
and S (stratified).

**Modes.** One mode per precursor, from the per-trace metrics of the shown traces. The first matching rule wins (rules are in the
`trace_gallery.py` docstring). Thresholds are anchored on the B-vs-OSW noise floor. A K-means cross-tab is in `gallery_run01_mode_vs_kmeans.tsv`.

| mode | share (72,993 precursors) | in random 50 |
|---|---|---|
| All engines agree | 17.4% | 9 |
| Low signal / noisy | 5.9% | 1 |
| MS2 shape differs, OSW higher | 3.6% | 2 |
| MS2 shape differs, DIA-NN higher | 11.4% | 6 |
| MS2 scale-only (sum vs max) | 1.1% | 0 |
| Truncated common span (DIA-NN XIC off-centre) | 6.3% | 2 |
| MS1-only divergence (ours A vs DIA-NN) | 29.8% | 13 |
| Minor differences | 24.5% | 17 |

- `trace_gallery_by_mode.png/.pdf`: the 50 random precursors, grouped by mode.
- `trace_gallery_stratified.png/.pdf`: the stratified sample, up to 8 per mode, not overlapping the random 50. It adds the scale-only
  mode, which the random sample lacks.
- `trace_gallery_mode_<mode>.png/.pdf`: one figure per mode, with random (R) and stratified (S) members.
