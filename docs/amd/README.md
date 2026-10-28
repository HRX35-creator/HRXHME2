# seekdb × AMD hipVS HNSW build

This delivery adds an optional ordinary dense-L2 HNSW build path: raw vectors
are buffered by seekdb, GPU CAGRA constructs the graph, a full HNSW hierarchy is
produced, and a self-contained snapshot is ready for seekdb queries. Existing
CPU VSAG indexes retain their behavior. The bridge performs CPU HNSW search;
build performance and query performance are reported separately.

Public implementation base: 76f871eb5a84b664d487b03b3412ce50194135b5.
provenance/public-base-audit108.json verifies that its three existing product
files match the original validation baseline and all four resulting product
files match tested revision 108 exactly. The original build provenance and
source patch remain in provenance/. This branch is prepared for a separate
public seekdb pull request; the formal comparison is pending.

The measured CPU/GPU end-to-end comparison is pending. REPORT.md and results.json
contain no invented values. Bridge ABI/quality smoke and local contract tests
are recorded independently under evidence/. A bridge smoke is not a completed
competition performance result.

Read these files in order:

1. SUBMISSION.md — official material requirements and current delivery status.
2. tools/hipvs_bridge/README.md — bridge build and ABI acceptance.
3. tools/benchmark/amd_hnsw/README.md — data, GT, SQL and three-round measurement.
4. REPORT.md — hardware, complete build interval, per-round results and recall.
   DIAGNOSTICS.md retains the earlier quality-failing M16 smoke separately.
5. PARAMETER_CONTRACT108.md — the process profile, SQL alignment and query-ef limits.
6. THIRD_PARTY_NOTICES.md — source references and license copies.

Run the portable local controls from the repository root:

```bash
python3 unittest/amd_hipvs/test_prepare_ground_truth.py
python3 unittest/amd_hipvs/test_completion_proof.py
python3 unittest/amd_hipvs/test_dispatch.py
python3 unittest/amd_hipvs/snapshot_io_contract.py
```

The first two use existing NumPy; the last two require a local C++/C compiler.
They do not start a database, operate cloud resources or claim GPU execution.
The packaged portable verification passed 7 GT tests, 4 completion-contract
tests, both production-dispatch cases and 32 snapshot I/O checks. All 13
verification commands passed; their original transcripts and receipt are in
evidence/local/portable108/. Set CXX or CC to select a non-default compiler.

provenance/delivery108-files.json inventories the delivery files and their
SHA256 values. manifest108.json identifies the canonical LF bytes of the four
integrated sources; those match the revision 108 build inputs exactly.

The private transfer scripts, signed artifact links, account material and
platform screenshots are excluded from this tree. Deployment consumes only
local paths and public source references.
