# Experimental AMD GPU snapshot builder

`lib=hipvs` collects the complete dense L2 HNSW snapshot, builds its base graph
with hipVS CAGRA, validates/converts the export, and loads it into VSAG without
CPU Build/Add construction. Existing VSAG serialization, CPU queries, filtering
and incremental writes remain in use. Ordinary `lib=vsag` is unchanged.

This is a development implementation. Real AMD GPU correctness, scale, recall,
memory accounting and end-to-end speedup have not yet been established. It is
not a competition submission or a performance result.

Local validation on 2026-10-28: full seekdb compilation succeeded; all three
standalone test groups passed, including 25,434 graph/consumer checks under
ASan/UBSan. The source-built SQL test passed seven lifecycle checks with the
explicit synthetic plugin: unsupported options, CPU and imported-graph index
creation, restart without the plugin, insert, delete, and another restart.
Both query plans used VECTOR INDEX SCAN and the snapshot import event was
observed. These results establish local integration, not AMD GPU execution.

## Compatibility and limits

- Linux LP64, little endian, float32 dense L2, HNSW without extra-info payloads.
- Consumer pinned to VSAG `012a0f2` (the competition SDK dependency).
- Export contract inspected at AMD-Ecosystem/hipVS
  `f96a663d0a4441d6f5344bd3ef552b71ae7b370b`,
  `cpp/src/neighbors/detail/cagra/cagra_serialize.cuh`. The export is experimental;
  mismatched headers, lengths, ordinals, neighbors or vector data are rejected.
- Nonempty GPU builds require `rows > 2*m`. Empty snapshots have no graph build.
- CAGRA uses NN-Descent, 20 iterations, output degree `2*m`, intermediate degree
  `min(4*m, rows-1)`. HNSW `ef_construction` is retained for later CPU additions;
  it is not an interchangeable CAGRA construction parameter.
- The imported graph is base-layer-only. Its recall must be measured on the
  actual workload; format compatibility does not imply acceptable recall.
- Plugin/API/GPU/export failures fail the build; there is no CPU build fallback.
- Full vectors are buffered in memory. Larger-than-memory construction, GPU
  resource quotas and concurrent GPU build admission remain open work.

## Build and load the optional plugin

Build against an actual AMD hipVS/ROCm SDK. The server does not link the SDK.

```sh
cmake -S tools/hipvs_bridge -B build_hipvs -DHIPVS_ROOT=/path/to/hipvs
cmake --build build_hipvs
# Add build_hipvs and the matching SDK/runtime libraries to LD_LIBRARY_PATH
# before starting the source-built seekdb server.
```

The loader resolves `libseekdb_hipvs_build.so` ABI v1, keeps it loaded, and uses
a dedicated 32 MiB-stack worker. Missing plugins are cached until restart.
Transfers, GPU synchronization, private temporary export I/O, validation,
conversion and VSAG loading all belong in the measured build interval.

```sql
-- Import raw vectors first, then construct the index.
CREATE VECTOR INDEX idx_v ON vectors(v)
WITH (distance=l2,type=hnsw,lib=hipvs,m=16,ef_construction=200,ef_search=64);
```

## Local compatibility and failure tests

```sh
cmake -S unittest/lib/vector -B build_graph_tests -DVSAG_ROOT=/path/to/vsag/sdk
cmake --build build_graph_tests
ctest --test-dir build_graph_tests --output-on-failure
```

Set runtime paths for the VSAG SDK when required. The tests use synthetic graphs
to check exact 64-bit IDs, exact top-10/distance on a small connected fixture,
filters, persistence, incremental additions, malformed/truncated exports,
atomic rejection, missing plugin, GPU errors, exceptions and worker stack size.
`fake/libseekdb_hipvs_build.so` is TEST ONLY; never deploy it or include it in a
GPU measurement. The SQL lifecycle test requires `--synthetic-fixture` explicitly.

The public GPU query prototype in seekdb PR #1314 helped identify the snapshot
gap and worker stack constraint. This implementation builds the snapshot before
persistence; it does not reuse that PR's lazy query-side CAGRA index cache.

Before submission: build and test the real plugin, run SQL create/restart and
mutation checks on AMD, record kernel/build evidence, and run at least three
fresh CPU and GPU index builds with identical input and explicit construction
parameters. Report exact recall separately from timing and include first query
and any deferred work. Do not report synthetic fixtures as GPU evidence.
