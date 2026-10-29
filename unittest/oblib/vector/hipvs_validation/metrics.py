"""Strict L2 truth and recall helpers shared by offline tests and live experiments."""
import json
import math
from pathlib import Path

import numpy as np


def exact_neighbors(base, queries, ids, k):
    if base.ndim != 2 or queries.ndim != 2 or base.shape[1] != queries.shape[1]:
        raise ValueError("base/queries must have matching dimensions")
    if not 0 < k <= len(base) or len(ids) != len(base) or len(np.unique(ids)) != len(ids):
        raise ValueError("invalid k or nonunique ids")
    if not np.isfinite(base).all() or not np.isfinite(queries).all():
        raise ValueError("non-finite vectors")
    # float64 exact scan of the *actual float32 stored values*, no ANN approximation.
    result, distances, ties = [], [], []
    base64 = base.astype(np.float64)
    for q in queries:
        diff = base64 - q.astype(np.float64)
        dist = np.einsum("ij,ij->i", diff, diff)
        order = np.lexsort((ids, dist))
        result.append(ids[order[:k]])
        distances.append(dist[order[:k]])
        ties.append(bool(k < len(base) and dist[order[k - 1]] == dist[order[k]]))
    return np.array(result, dtype=np.int64), np.array(distances), ties


def score_results(truth, results, valid_ids):
    """Missing queries count as zero. Malformed, duplicate and out-of-domain IDs fail validation."""
    expected = {int(row["query_id"]): row["ids"] for row in truth}
    if not expected or len(expected) != len(truth):
        raise ValueError("truth query ids must be nonempty and unique")
    k = len(next(iter(expected.values())))
    if not k or any(len(v) != k or len(set(v)) != k for v in expected.values()):
        raise ValueError("truth must have k unique ids for each query")
    valid_ids = set(int(x) for x in valid_ids)
    if any(not set(v).issubset(valid_ids) for v in expected.values()):
        raise ValueError("truth includes unknown ids")
    observed, errors, hits = set(), [], 0
    returned_unique = 0
    for row in results:
        if not isinstance(row, dict):
            errors.append("query result is not an object")
            continue
        qid = row.get("query_id")
        if type(qid) is not int or qid not in expected:
            errors.append(f"invalid query id: {qid!r}")
            continue
        if qid in observed:
            errors.append(f"duplicate query: {qid}")
            continue
        observed.add(qid)
        neighbors = row.get("neighbors")
        if not isinstance(neighbors, list):
            errors.append(f"query {qid}: neighbors is not a list")
            continue
        if len(neighbors) != k:
            errors.append(f"query {qid}: expected {k} neighbors, got {len(neighbors)}")
        selected, seen_ids = set(), set()
        for position, item in enumerate(neighbors):
            if not isinstance(item, dict):
                errors.append(f"query {qid}: neighbor is not an object")
                continue
            nid, distance = item.get("id"), item.get("distance")
            if type(nid) is not int or nid not in valid_ids:
                errors.append(f"query {qid}: unknown/noninteger document id {nid!r}")
                continue
            if nid in seen_ids:
                errors.append(f"query {qid}: repeated document id {nid}")
                continue
            seen_ids.add(nid)
            if isinstance(distance, bool) or not isinstance(distance, (int, float)) or not math.isfinite(distance) or distance < 0:
                errors.append(f"query {qid}: invalid distance for {nid}")
                continue
            if position < k:
                selected.add(nid)
        hits += len(selected.intersection(expected[qid]))
        returned_unique += len(selected)
    missing = sorted(set(expected) - observed)
    errors.extend(f"missing query: {qid}" for qid in missing)
    return {"recall_at_k": hits / (len(expected) * k), "k": k,
            "queries": len(expected), "hits": hits, "denominator": len(expected) * k,
            "returned_unique": returned_unique, "missing_queries": missing,
            "valid_results": not errors, "errors": errors}


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temp.replace(path)


def vector_literal(values):
    # 9 significant digits round-trip IEEE float32, unlike the demo's fixed 6 decimals.
    return "[" + ",".join(format(float(x), ".9g") for x in values) + "]"
