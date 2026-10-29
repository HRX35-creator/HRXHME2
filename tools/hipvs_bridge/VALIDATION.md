# AMD HNSW snapshot construction: submission evidence

## Scope and result

This candidate was developed from seekdb `2026-competition`,
`5bce9e06d1281347ad61344e4b01fab9158c58a2`. The source-built server and real
hipVS plugin completed SQL construction, query, persistence and mutation checks
on Radeon Cloud on 2026-10-28, before the neighbor refinement added on 2026-10-29.
The historical GPU measurements below do not validate that new production path.
Evaluation against the competition's final quality
and performance thresholds is pending.

The GPU constructs the snapshot graph with CAGRA NN-Descent. In the current
candidate, a CPU pass preserves the original BFS tree and selects neighbors from
outgoing plus reverse edges. Its cost belongs inside end-to-end build timing. A validated
conversion restores external 64-bit IDs and VSAG's mutable serialization
parameters; VSAG deserializes the completed graph. CPU queries, filtering,
incremental writes and persistence continue through VSAG. Optional builder
failures return errors. `lib=vsag` remains the ordinary CPU construction path.

| Workload | CPU median seconds | GPU median seconds | Timing ratio | CPU Recall@10 | GPU Recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 10,000 template vectors, 100 queries, 3 trials each | 2.582334 | 2.522127 | 1.024x | 0.640 | 0.704-0.714 |
| 100,000 generated Gaussian vectors, 100 queries, 3 trials each | 39.265204 | 6.364414 | 6.17x | 0.509 | 0.395-0.433 |

The 100k GPU recall is lower. Its timing ratio is consequently **not an
equal-quality speedup result or a competition pass claim**. The 10k gain is small.
The 100k data uses seed 20261028 and is an exploratory workload, not the official
evaluation data. The table describes the unrefined implementation and has not
been relabeled as a measurement of the current candidate.

Raw successful runs, query IDs/truth/recall, plans, build events, environment
details and the independent kernel trace are in
[`validation/2026-10-28/`](validation/2026-10-28/). Their SHA-256 manifest records
the source files copied from the downloaded evidence archive. All failed local
attempts remain in that archive, SHA-256
`765457718f28fa9cfd8fcf258010a8ac6938b59d6d70813a97e98c9620f8ee75`.

## Tested environment and identity

- Radeon Cloud `seekdb-hipvs` image digest:
  `52772c38647ad71c4c65954f555b90c871e11370d2cebee5d86745105355816a`.
- GPU: gfx1100, reported as AMD Radeon Graphics. CPU: AMD EPYC 9334.
- Container quota: 16 CPU cores, 55 GiB RAM. seekdb: `cpu_count=8`, `memory_limit=8G`.
- Pinned VSAG consumer: `012a0f2`. The inspected experimental hipVS serialization
  contract is recorded in `hipvs_graph_import.h`; actual runtime dependencies and
  GPU inventory are retained in each raw report.
- Server source: `f63c1f8c66a761ec2e5940e1e662c60292b62639`. Its packaged binary
  SHA-256 is `8a1266f144dbf9bc00cb60f9fad36025d15d057ec1bd25b4636cb9fada2a3cff`.
  The build directory had no Git metadata; changed production inputs were
  compared byte-for-byte with the committed source after CRLF normalization.
- The C API link fix was applied before successful GPU trials and committed in
  `e0e8084364817e06db6d2e15cf466258237d2e1a`. Tested plugin SHA-256:
  `bd2e7498aefa5f413e653ca9f2d30ad762a1bc7416ef79b804e112e2b8bb115a`.
- The 2026-10-29 refinement changes production graph import; it does not change
  the plugin source. It requires a new real GPU SQL validation run.

## Reproduction

Use Linux x86-64 with the repository's supported toolchain, the real AMD hipVS
SDK at `/opt/hipvs`, ROCm at `/opt/rocm`, Python 3 with NumPy, and a `mysql` CLI.
The template contains `/seekdb_workspace/datasets/base.f32` and `query.f32`.
The driver reports explicitly whether it used these files or generated Gaussian
vectors. Record the Python/NumPy and SDK versions when reproducing on a different
image. Build with the repository's pinned dependencies:

```sh
./build.sh release --init --make
cmake -S tools/hipvs_bridge -B build_hipvs -DHIPVS_ROOT=/opt/hipvs -DROCM_ROOT=/opt/rocm
cmake --build build_hipvs --parallel 2

stage_dir=$(mktemp -d /tmp/seekdb-hipvs-validation.XXXXXX)
mkdir -p "$stage_dir/bin" "$stage_dir/build_hipvs" "$stage_dir/runtime"
cp build_release/src/observer/seekdb "$stage_dir/bin/seekdb"
cp build_hipvs/libseekdb_hipvs_build.so "$stage_dir/build_hipvs/"
# When moving an Ubuntu 24.04 build to the supplied runtime, preserve its libaio SONAME.
if [ -f /lib/x86_64-linux-gnu/libaio.so.1t64 ]; then
  cp /lib/x86_64-linux-gnu/libaio.so.1t64 "$stage_dir/runtime/"
fi
python3 - "$stage_dir" "$(git rev-parse HEAD)" <<'PY'
import hashlib, json, platform, sys
from pathlib import Path
import numpy
root = Path(sys.argv[1])
files = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in root.rglob('*') if p.is_file()}
(root/'manifest.json').write_text(json.dumps({
    'commit': sys.argv[2], 'files': files,
    'python': platform.python_version(), 'numpy': numpy.__version__}, indent=2))
PY
export LD_LIBRARY_PATH="$stage_dir/runtime:/opt/hipvs/lib:/opt/hipvs/lib64:/opt/rocm/lib:${LD_LIBRARY_PATH:-}"
ldd "$stage_dir/bin/seekdb"
ldd "$stage_dir/build_hipvs/libseekdb_hipvs_build.so"

python3 unittest/lib/vector/benchmark_hipvs_snapshot.py --root "$stage_dir" --rows 2000 --queries 10 --repeats 1
python3 unittest/lib/vector/benchmark_hipvs_snapshot.py --root "$stage_dir" --rows 10000 --queries 100 --repeats 3
python3 unittest/lib/vector/benchmark_hipvs_snapshot.py --root "$stage_dir" --rows 100000 --queries 100 --repeats 3
```

The driver requires an unused local port (default 2907). It starts its own server
and preserves a new `evidence/gpu-*/` directory per attempt. It rejects missing
GPU support, invalid IDs, duplicate results, missing import events, unexpected
plans and failed restart checks. `complete=true` reports completion of these
functional checks; it does not encode a competition quality threshold.

All inputs are 128-dimensional float32 L2; `m=16`, `ef_construction=200`,
`ef_search=64`, immediate synchronization. CAGRA uses NN-Descent 20 iterations,
intermediate degree 64 and final degree 32. CPU HNSW construction parameters and
CAGRA parameters are not interchangeable. Exact query truth uses float64 L2.

Timing begins after raw rows are inserted and before CREATE VECTOR INDEX, then
ends after the first valid ANN result. Scanning, transfers, synchronization,
conversion and VSAG loading are included. Each trial builds a fresh table/index;
CPU/GPU order alternates. The Python process performs an explicit small direct
GPU smoke before SQL; it does not share the database process's GPU context.
Data generation/import and server startup are outside the measured interval.
Profiler runs are separate from timings.

## Functional and GPU evidence

- Full server build, 3 standalone test groups and 25,434 graph/consumer checks
  under ASan/UBSan passed locally. Synthetic-plugin tests establish integration
  behavior; the plugin used in real GPU measurements is separately identified.
- Real GPU trials completed 14 CPU/GPU index constructions in total. All returned
  IDs were valid and distinct, plans used VECTOR INDEX SCAN, and all 7 GPU indexes
  survived restart with the optional builder directory removed from the loader path.
- The real 2k GPU graph passed six mutation checks: existing row, insert with an
  exact greater-than-2^53 ID, vector update, deletion of a snapshot row, and
  persistence of the updated/new row and deleted row after restart. The builder
  directory was absent from the path during these checks.
- A separate bridge-only `rocprofv3 --kernel-trace --output-format csv` run exited
  0 and recorded 199 dispatches, including NN-Descent local joins and reverse-edge
  work. SQL graph-import events provide the separate integration evidence.
- The first plugin attempt failed at load with `undefined symbol: cuvsCagraBuild`;
  linking `libcuvs_c.so` and rejecting undefined symbols fixed it. A full-process
  profiler attempt lost its SQL connection and exited 1; its cause is unresolved.
  It is not counted as a passed SQL profiling run.

## Remaining limits

Dense float32 L2 on Linux LP64 only; nonempty builds require `rows > 2*m` and
`2 <= m <= 128`. The graph format is experimental and validated against a pinned
contract. Full vectors are buffered in memory. Large-data recall, GPU memory
quotas and concurrent-build admission remain open. GPU errors fail explicitly.
Fresh construction needs the optional real plugin and runtime; persisted indexes
retain the ordinary VSAG format and query without the builder.

## Refined candidate: local evidence on 2026-10-29

Full-graph capture subsequently succeeded (archive SHA-256
`cb3720c32513b7dc6ab8c1ab0b958df8a7d14b9a16d02d9850d6e163cfe1f800`,
109 manifest files verified). With 400 fresh Gaussian queries, intermediate
degrees 64/128/256 gave original ef64 recall 0.4195/0.39325/0.38025 and refined
recall 0.4845/0.46475/0.4485. CPU HNSW gave 0.474. These three graphs share one
vector dataset and differ in construction parameters; they are not independent
datasets or repeated same-parameter builds. A separate template 2k graph changed
from 0.963 to 0.964 on 100 existing queries. All 192 replay configurations passed.
Original reachable coverage and all non-edge bytes were preserved.

The paired-query bootstrap interval for the default graph versus CPU includes
zero; these observations do not establish stable superiority to CPU or formal
acceptance. A sort-only control returned identical results to the original graph
in all 24 configurations. The production component output matched the previously
tested refined graph byte-for-byte. Its three local CPU times were
1.92971/1.92835/1.89237 seconds, including validation and allocation but excluding
file I/O and GPU construction. Cross-machine times must not be added to infer
an end-to-end speedup.

The refined server built successfully and passed five standalone test groups,
18 refinement shapes under ASan/UBSan, and eight synthetic-plugin SQL checks:
unsupported options, CPU/imported index creation, restart without builder,
insert, vector update, deletion filtering and restart after mutations. Build
logs contain existing Python AST deprecation warnings, with no C++ warnings
observed. The source-built unstripped binary SHA-256 is
`605148ce9324b90508daf837a5bfc2db94f84a38b97c658536277a1b9cea9dc1`.

These local tests do not execute AMD GPU code. Repeat the real GPU SQL benchmark
on one machine with this binary, including refinement in the measured interval,
before reporting a new quality or performance result. CPU scratch allocations,
GPU quotas and concurrent-build memory accounting remain limitations.
