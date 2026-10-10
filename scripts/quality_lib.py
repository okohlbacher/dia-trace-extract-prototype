"""Shared code for the trace-quality study (scripts/quality_*.py).

Rule engine on the centroid sample of quality_reextract.py, and engine-agnostic shape metrics.
"""
import numpy as np, pandas as pd, pyarrow.parquet as pq

C13 = 1.0033548378


# ------------------------------------------------------------------ centroid sample + rules
class Sample:
    def __init__(self, d):
        self.q = pd.read_parquet(d + "/queries.parquet")
        c = pq.read_table(d + "/centroids.parquet").to_pandas()
        c = c.merge(self.q[["qid", "target_mz", "ms_level"]], on="qid")
        c["ppm"] = (c.mz - c.target_mz) / c.target_mz * 1e6      # error vs the library (uncorrected) m/z
        self.c = c

    def extract(self, level, centre_ppm, half_ppm, agg="max", strict=False, accept=None):
        """centre_ppm: scalar or function(target_mz) -> ppm shift of the window centre; half_ppm: scalar or
        function(centroid rows) -> per-centroid half width (intensity-dependent windows). accept: optional
        boolean mask over self.c rows (further restriction). Returns {qid: dense array over [c0, c1)}."""
        c = self.c[self.c.ms_level == level]
        ctr = centre_ppm(c.target_mz.to_numpy()) if callable(centre_ppm) else centre_ppm
        hw = half_ppm(c) if callable(half_ppm) else half_ppm
        dev = c.ppm.to_numpy() - ctr
        ok = (np.abs(dev) < hw) if strict else (np.abs(dev) <= hw)
        if accept is not None: ok &= accept[c.index].to_numpy() if isinstance(accept, pd.Series) else accept
        s = c[ok]
        g = s.groupby(["qid", "pos"]).intensity.agg(agg)
        return self.dense(g, level)

    def dense(self, g, level):
        out = {}
        qq = self.q[self.q.ms_level == level]
        for qid, c0, c1 in qq[["qid", "c0", "c1"]].itertuples(index=False):
            out[qid] = np.zeros(c1 - c0)
        if len(g):
            qi = g.index.get_level_values(0).to_numpy(); pi = g.index.get_level_values(1).to_numpy()
            c0 = self.q.c0.to_numpy()[qi]
            for k, v in zip(zip(qi, pi - c0), g.to_numpy()):
                out[k[0]][k[1]] = v
        return out


# ------------------------------------------------------------------ shape metrics
def _pearson_rows(M):
    """mean pairwise Pearson of the rows of M (rows with zero variance dropped)."""
    M = M[M.std(axis=1) > 0]
    if len(M) < 2: return np.nan
    C = np.corrcoef(M); iu = np.triu_indices(len(M), 1)
    return float(np.nanmean(C[iu]))


def shape_metrics(F, lib, ms1=None, half=4, top=6):
    """F: fragments x T (common time grid of one precursor, one engine); lib: library intensities (F);
    ms1: M+0 trace on the same grid or None. Peak region = +-half points around the apex of the summed
    top-`top` library fragments. Returns dict of metrics."""
    F = np.nan_to_num(np.asarray(F, float)); lib = np.asarray(lib, float)
    order = np.argsort(-lib)[:top]
    tot = F[order].sum(0)
    if tot.max() <= 0: return None
    ap = int(np.argmax(tot)); lo, hi = max(0, ap - half), min(F.shape[1], ap + half + 1)
    P = F[order, lo:hi]; L = lib[order]
    out = {"apex": ap, "signal": float(P.sum())}
    out["coelution"] = _pearson_rows(P)
    v = F[order, ap]
    out["cos_apex"] = float(v @ L / (np.linalg.norm(v) * np.linalg.norm(L))) if v.any() else 0.0
    area = P.sum(1)
    out["cos_area"] = float(area @ L / (np.linalg.norm(area) * np.linalg.norm(L))) if area.any() else 0.0
    # apex spread (points) over fragments with signal
    has = P.max(1) > 0
    out["apex_spread"] = float(np.std(np.argmax(P[has], 1))) if has.sum() >= 2 else np.nan
    # jaggedness: second-difference energy relative to signal energy (per fragment, intensity-weighted)
    W = F[order, max(0, lo - 1):min(F.shape[1], hi + 1)]
    if W.shape[1] >= 3:
        d2 = np.diff(W, 2, axis=1); e = (W ** 2).sum(1)
        okf = e > 0
        out["jagged"] = float(((d2 ** 2).sum(1)[okf]).sum() / e[okf].sum()) if okf.any() else np.nan
    else: out["jagged"] = np.nan
    # isolated spikes: non-zero points whose neighbours are both zero; dropouts: zeros between non-zeros
    nz = P > 0
    spikes = nz[:, 1:-1] & ~nz[:, :-2] & ~nz[:, 2:]
    out["spike_frac"] = float(spikes.sum() / max(1, nz.sum()))
    gaps = 0; span = 0
    for r in nz:
        idx = np.nonzero(r)[0]
        if len(idx) >= 2:
            seg = r[idx[0]:idx[-1] + 1]; gaps += int((~seg).sum()); span += len(seg)
    out["dropout_frac"] = gaps / span if span else np.nan
    if ms1 is not None:
        m = np.nan_to_num(np.asarray(ms1, float))[lo:hi]; s = P.sum(0)
        out["ms1_coelution"] = float(np.corrcoef(m, s)[0, 1]) if m.std() > 0 and s.std() > 0 else np.nan
        mw = np.nan_to_num(np.asarray(ms1, float))[max(0, lo - 1):min(len(ms1), hi + 1)]
        out["ms1_dropout"] = float(_dropouts(mw > 0))
    return out


def _dropouts(r):
    idx = np.nonzero(r)[0]
    if len(idx) < 2: return np.nan
    seg = r[idx[0]:idx[-1] + 1]
    return (~seg).sum() / len(seg)
