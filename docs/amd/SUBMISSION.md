# AMD submission materials

Official requirement: https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb

Question 600072, subquestion 800509, maximum 20 points. The official snapshot is
provenance/official-requirements-snapshot.txt. It requires a seekdb PR, reproducible
method and CPU/GPU HNSW build comparison report. It does not publish a fixed
speedup/quality threshold in that snapshot.

| Deliverable | Location | Status |
|---|---|---|
| seekdb integration | src/oblib/lib/vector and src/observer/vector_index | Revision 108 on public upstream base 76f871eb5a84b664d487b03b3412ce50194135b5 |
| GPU CAGRA → full HNSW bridge | tools/hipvs_bridge | Source and build script staged |
| Reproduction method | tools/benchmark/amd_hnsw/README.md | Draft; actual formal configuration pending |
| CPU/GPU ≥3 fresh rounds | results.json and evidence/formal | Pending root's actual run |
| Exact-GT Recall@K and ID checks | SQL reports from the complete shared query set | Pending formal report |
| GPU build and no-fallback evidence | phase logs and completion proofs per round | Pending formal report |
| Upstream seekdb PR | Not created | Pending validation and commit/push |

Preliminary bridge and local regression receipts are included with their actual
scope. They do not replace the per-round integrated SQL evidence. No final score
or passing claim is made before the measured comparison is supplied.

The implementation is intentionally limited to ordinary float32 L2 HNSW with no
extra-info payload. Unsupported extra-info types are rejected, and stateful
filtered queries preserve the original CPU iterative-search behavior. CPU
fallback is explicit and never counted as successful GPU acceleration.
