"""Prepare reproducible local data and exact truth before the GPU clock starts."""
import argparse
import hashlib
import time
from pathlib import Path

import numpy as np
from metrics import exact_neighbors, write_json


def generate(out, n=10000, dim=128, nq=100, k=10, seed=20261029):
    if min(n, dim, nq, k) <= 0 or k > n:
        raise ValueError("positive sizes and k <= n required")
    out = Path(out)
    out.mkdir(parents=True, exist_ok=False)
    start = time.perf_counter()
    rng = np.random.Generator(np.random.PCG64(seed))
    centers = rng.standard_normal((min(64, n), dim))
    base = (centers[rng.integers(len(centers), size=n)] + rng.normal(0, 0.45, (n, dim))).astype(np.float32)
    queries = (centers[rng.integers(len(centers), size=nq)] + rng.normal(0, 0.45, (nq, dim))).astype(np.float32)
    # Non-contiguous >32-bit external document IDs expose accidental row-offset assumptions.
    ids = rng.permutation(np.arange(n, dtype=np.int64) * 17 + 8589934593)
    truth, distances, ties = exact_neighbors(base, queries, ids, k)
    np.savez(out / "vectors.npz", base=base, queries=queries, ids=ids)
    rows = [{"query_id": i, "ids": truth[i].tolist(), "squared_l2": distances[i].tolist()} for i in range(nq)]
    write_json(out / "truth.json", rows)
    manifest = {"kind": "local_development_dataset_not_official", "seed": seed, "rng": "PCG64",
                "n": n, "dimensions": dim, "queries": nq, "k": k, "metric": "l2", "dtype": "float32",
                "truth_arithmetic": "float64 exhaustive squared L2; tie broken by document ID",
                "tie_at_k_queries": [i for i,v in enumerate(ties) if v],
                "generation_seconds": time.perf_counter() - start, "numpy_version": np.__version__,
                "files": {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in ("vectors.npz", "truth.json")}}
    write_json(out / "manifest.json", manifest)
    return manifest


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--n", type=int, default=10000)
    p.add_argument("--dim", type=int, default=128)
    p.add_argument("--queries", type=int, default=100)
    p.add_argument("--k", type=int, default=10)
    p.add_argument("--seed", type=int, default=20261029)
    a = p.parse_args()
    m = generate(a.out, a.n, a.dim, a.queries, a.k, a.seed)
    print(f"Prepared {m['n']} x {m['dimensions']}, {m['queries']} queries; {m['generation_seconds']:.2f}s")
