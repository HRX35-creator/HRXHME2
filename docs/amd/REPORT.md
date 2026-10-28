# CPU/GPU HNSW construction report

Status: PENDING FORMAL RESULTS. This file is a report skeleton. All performance
and complete SQL recall fields remain empty until raw results are available.

The measured runtime uses revision 108. Its exact product source is preserved
in commit 111c02a7b510199e1d1a69c46a9c2c6ef9b0312c. A subsequent public-source
compatibility change only adopts upstream's rust/config.h include and safe
bitmap snprintf, and relocates the AMD hooks include. It does not change GPU
logic or represent a newly measured runtime. See
provenance/master-compatibility108.json for the source hashes and check scope.

## Implementation and baseline

The same revision 108 seekdb binary compares lib=vsag with lib=cuvs on separate
fresh tables. CUVS uses GPU CAGRA construction followed by full HNSW conversion,
explicit stream synchronization and a self-contained snapshot. Search uses
CPU HNSW. The original CPU graph is not built eagerly on the selected GPU path.

The input and target index state are checked before timing. The timed interval
starts with raw data already in the database and the target index absent. It
ends after complete DDL, all graph/transfer/conversion/snapshot work and the
first successful indexed query plus its verified completion proof. Lazy work
in that query is included. Data generation, SQL import and server startup are
separately recorded.

## Environment

The existing cloud preflight observed AMD EPYC 9334, gfx1100 AMD Radeon Graphics
and ROCm/HIP 7.2.4. Evidence: evidence/cloud/environment.json. Exact CPU affinity,
cgroup limits, actual GPU identity, image digest, hipVS versions, candidate and
bridge hashes for the formal run are pending and belong in results.json.

memory_budget is a logical seekdb cache/buffer budget. It is not an OS RSS cap.
The formal resource record must include actual CPU affinity and cgroup/instance
limits; cpu_count alone does not establish an OS CPU quota.

## Dataset and common configuration

| Field | Actual formal value |
|---|---|
| Source, seed, base/query hashes | PENDING |
| Base count / dimension | PENDING |
| Complete query count / K / target count | PENDING |
| Distance / vector precision | L2 / float32, subject to recorded formal config |
| M / ef_construction / session ef_search | PENDING |
| GPU hierarchy / graph degree / intermediate degree / iterations | PENDING |
| CPU affinity / thread settings / memory limit | PENDING |
| seekdb / bridge / hipVS / ROCm identity | PENDING |

## End-to-end construction results

| Round | Order | CPU complete build seconds | GPU complete build seconds | CPU / GPU Recall@K | Proof |
|---|---|---:|---:|---|---|
| 1 | PENDING | — | — | PENDING | PENDING |
| 2 | PENDING | — | — | PENDING | PENDING |
| 3 | PENDING | — | — | PENDING | PENDING |
| Median | — | — | — | — | — |

Build speedup = median(CPU complete build time) / median(GPU complete build time).
No speedup has been entered. DDL return and first-query timestamps will be
reported as timing components, not substituted for the verified full boundary.

## Correctness and stability

Each backend must process every base vector and every query. GT uses the same
actual float32 data, exact L2, a complete query denominator and deterministic ID
tie handling. Every result has exactly K distinct valid IDs. The raw query
results, SQL plans and read-back hashes are retained.

Preliminary evidence only: local revision 108 tests pass with explicit missing-
bridge CPU fallback; source-level dispatch controls reject the two former
metadata/iteration bugs. The cloud bridge FD smoke passes at n=2048, d=32,
Recall@10=0.985 including snapshot round trips. That smoke does not establish
integrated SQL speedup or a final competition quality verdict.

## GPU participation and fallback

Every formal GPU round must link the successful SQL DDL and indexed first query
to a new full-N phase sequence: build_start, GPU CAGRA complete, synchronized
HNSW conversion complete and caller-FD snapshot serialization. Source/bridge
hashes and original log offsets must match the run. CPU fallback, missing stages,
partial N, reused graphs and failed rounds invalidate the GPU timing claim.

Formal conclusion: PENDING. No competition pass/score is claimed.
