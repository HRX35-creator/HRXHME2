# Experimental AMD GPU snapshot builder

`lib=hipvs` collects the complete dense L2 HNSW snapshot, builds its base graph
with hipVS CAGRA, validates/converts the export, and loads it into VSAG without
CPU Build/Add construction. Existing VSAG serialization, CPU queries, filtering
and incremental writes remain in use. Ordinary `lib=vsag` is unchanged.

This is a development implementation. Real AMD GPU smoke, SQL build/restart,
and mutation checks have passed on gfx1100. The 100k exploratory dataset shows
a construction speed gain but lower recall than the CPU baseline. Quality at
the required workload, memory accounting and concurrent builds remain open.
This is not a competition submission or an acceptance result.

Local validation on 2026-10-28: full seekdb compilation succeeded; all three
standalone test groups passed, including 25,434 graph/consumer checks under
ASan/UBSan. The source-built SQL test passed seven lifecycle checks with the
explicit synthetic plugin: unsupported options, CPU and imported-graph index
creation, restart without the plugin, insert, delete, and another restart.
Both query plans used VECTOR INDEX SCAN and the snapshot import event was
observed. These results establish local integration, not AMD GPU execution.

## Real GPU validation on 2026-10-28

The source-built server from `f63c1f8c66a761ec2e5940e1e662c60292b62639`,
plus the C API linkage fix in this directory, was tested on Radeon Cloud
gfx1100. The container quota was 16 CPUs / 55 GiB; seekdb was configured with
`cpu_count=8` and `memory_limit=8G`. No synthetic plugin was deployed.

All inputs were 128-dimensional float32 L2 vectors, with `m=16`,
`ef_construction=200`, `ef_search=64`, and immediate synchronization. CAGRA
uses the different construction parameters documented below. Timing starts
at CREATE VECTOR INDEX after raw insertion and ends after the first valid
ANN query. It includes conversion, synchronization and loading. Each run uses
a fresh table/index; three-run trials alternate CPU/GPU order in one server.
A direct GPU smoke runs in the Python driver beforehand; it does not share
the server's process or CUDA/HIP context. Kernel tracing is excluded from
these performance runs.

| Data | Trials per library | CPU median seconds | GPU median seconds | CPU Recall@10 | GPU Recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Template first 2,000 rows, 10 queries | 1 | 0.7724 | 2.0613 | 0.900 | 0.980 |
| Template 10,000 rows, 100 queries | 3 | 2.5823 | 2.5221 | 0.640 | 0.704-0.714 |
| Generated Gaussian 100,000 rows, 100 queries, seed 20261028 | 3 | 39.2652 | 6.3644 | 0.509 | 0.395-0.433 |

The 100k timing ratio is 6.17x, but its worse recall prevents an equal-quality
speedup claim. Generated data is exploratory, not an official evaluation set.
Recall uses exact float64 L2 top-10 truth on the same saved query vectors.
All returned IDs were valid and distinct, query plans used VECTOR INDEX SCAN,
and every GPU index produced a graph-import event and survived restart without
the optional builder. A separate check on the real 2k GPU index passed insert
with a greater-than-2^53 ID, vector update, snapshot-row deletion and persistence
of these changes after another restart, all without the builder on its path.

A standalone bridge `rocprofv3 --kernel-trace --output-format csv` run exited
0 and recorded 199 dispatches, including NN-Descent local-join and reverse-edge
kernels. This is direct bridge evidence; SQL construction is separately evidenced
by its import events. An earlier trace of the entire SQL test exited 1 after
losing the database connection; that failure is retained and not counted as
a passed SQL profile. Its root cause remains undetermined.

Reproduce the performance runs with
`python3 unittest/lib/vector/benchmark_hipvs_snapshot.py --root /path/to/candidate`.
The staging root contains `bin/seekdb`, `manifest.json`, the built plugin under
`build_hipvs/`, and optional system runtime libraries under `runtime/`. Use
`--rows 10000 --queries 100 --repeats 3`, or `--rows 100000` for the exploratory
scale check. The script records input origin and hashes, exact-query results,
individual timings, plugin/runtime identity, failures and restart checks.
Its completion flag checks functional execution, not a competition quality
or performance threshold. The 10k template dataset must be present under
`/seekdb_workspace/datasets`; otherwise the script explicitly reports generated
data. The real mutation and profiling scripts and all attempts are retained in
the local evidence archive with SHA-256
`765457718f28fa9cfd8fcf258010a8ac6938b59d6d70813a97e98c9620f8ee75`.

## Offline graph diagnostics

The standalone test project also builds `replay_hipvs_graph`. Run:

```sh
replay_hipvs_graph captured-export.bin base.f32 query.f32 report.json
replay_hipvs_graph --cpu-control base.f32 query.f32 cpu-control.json
```

The first command validates the complete captured hipVS export with the
production converter, retains its edges and vector values, and measures recall
against exact float64 top-10 truth. It reports reachability, zero-indegree nodes,
three entry-point choices, and ef_search 64/128/256/512. It also compares a CPU
HNSW graph with copies that preserve its base edges and remove only upper layers.
The CPU-only command uses 128 dimensions and m=16 and requires no captured graph
or GPU. Both paths validate returned ID uniqueness/range and vector distances.
Do not confuse their in-process load/query timings with the SQL build benchmark.

On the saved 100k Gaussian input, the CPU-only control reproduced Recall@10
0.509 at ef=64. Removing only its upper layers gave 0.497, or 0.505 with a
centroid entry, on two runs. This small loss does not establish that removing
hierarchy is harmless for every graph, but it cannot alone explain the earlier
GPU result of 0.395-0.433. Two captured 2k GPU graphs were fully reachable; higher
ef reached recall 1.0, and changing their entry points had little effect. These
small graphs do not rule out connectivity or candidate-quality issues at 100k.
The full 100k GPU graph was not retained in the earlier archive, so a new
controlled capture is required before changing production construction settings.

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
The C ABI requires `libcuvs_c.so` (or `libhipvs_c.so`), not only the C++ library.
The plugin link rejects unresolved symbols so a missing C ABI fails at build
time instead of failing later at `dlopen`.

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

Before submission: improve the observed large-data recall gap, validate the
required evaluation dataset and resource limits, and repeat at least three
fresh CPU and GPU builds at comparable quality. Report exact recall separately
from timing and include first query and any deferred work. Preserve the full
failed attempts as well as successes. Do not report synthetic fixtures as GPU
evidence.
