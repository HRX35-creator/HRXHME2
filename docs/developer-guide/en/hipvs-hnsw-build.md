# Optional hipVS HNSW bulk construction

Full native seekdb builds on macOS and Linux, actual AMD SQL construction/query/DML, native graph import with ASan/UBSan, crash recovery and repeated-build resource checks have passed. On one AMD node, three fresh CPU and GPU SQL builds over 10,000x128 vectors measured a 2.93x median construction speedup, with minimum independent Recall@10 of 0.993/0.9985. This is a measured example, not competition acceptance: the official target configuration and thresholds were not published in the inspected task. Raw measurements and limits are in [the results record](hipvs-hnsw-build-results.json).

The default CPU path is unchanged. An explicit process option stages a complete ordinary HNSW snapshot, calls hipVS to construct its base graph on AMD hardware, converts it to a full HNSW hierarchy, validates its contents and imports it into a new native VSAG index. Queries, filtering, incremental DML and persistence then use the existing VSAG implementation. Quantized, HGraph and sparse indexes retain their existing paths.

## Construction path

Initial DDL uses `ObHNSWIndexAppendBufferOperator::append_row` and finalizes through `check_snap_hnswsq_index` before serialization. Async snapshot rebuilds also use this finalization point. With the option enabled, `add_snap_index` stages VIDs, vectors and any extra information instead of building an ordinary HNSW through repeated CPU `Add` calls. Only successful construction publishes the native index; failures release the staged state through existing snapshot cleanup.

The separate ROCm bridge calls the hipVS C API:

1. Build CAGRA with float32 L2 vectors, `ITERATIVE_CAGRA_SEARCH`, output degree `2*M` and intermediate degree `max(64,4*M)`. This algorithm uses GPU search internally to construct the graph; database queries still use native CPU VSAG. The C API transfers the host tensor; stream synchronization completes before the build interval ends.
2. Use `cuvsHnswFromCagraWithDataset` with a CPU-built upper hierarchy, the original host vectors, the requested `ef_construction` and the same VSAG building thread count. The expensive base graph comes from hipVS.
3. Serialize the full mutable HNSW to a private temporary file, read it, remove base-layer self edges emitted by the tested iterative builder, and return it through the versioned C ABI. This retains every non-self edge, original vector/label and upper layer. Conversion is inside the measured interval. Per-build indexes, handles, streams and temporary files have scoped cleanup. The bridge library stays loaded between builds.

The initial NN-Descent prototype was replaced after profiling showed half-precision scratch-vector inputs on the supplied image. The selected iterative path produced float32 dataset/distance kernel descriptors and no half-precision or NN-Descent kernel signatures in its recorded trace. Original float32 vectors are also checked byte for byte during import. The C ABI reports zero NN-Descent iterations for this algorithm. These observations apply to the tested image; another dependency build needs its own validation.

The importer verifies the complete serialized shape, original float32 vectors, a one-to-one ordinal mapping, all graph edges and upper-layer targets. It replaces each stored ordinal with its source VID, including when parallel hierarchy construction reordered internal nodes. It does not infer IDs from internal row positions. A fresh native index is published only after successful deserialization and an exact element-count check.

References: [AMD hipVS HNSW API](https://rocm.docs.amd.com/projects/hipVS/en/latest/reference/cpp_api/neighbors_hnsw.html), [the inspected hipVS source revision](https://github.com/AMD-Ecosystem/hipVS/tree/f96a663d0a4441d6f5344bd3ef552b71ae7b370b), and [the packaged VSAG source revision](https://github.com/antgroup/vsag/tree/129b82cff36a9736d06e1ecba5a608a671f6e948).

## Build and enable

Build seekdb normally. Its default build has no ROCm dependency. On Linux with AMD ROCm and a matching hipVS installation providing C API headers and `libcuvs_c.so`:

```sh
cmake -S deps/hipvs -B build_hipvs -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/opt/rocm/bin/hipcc -DCMAKE_PREFIX_PATH=/opt/rocm \
  -DHIPVS_ROOT=/absolute/path/to/hipvs/prefix -DHIPVS_GPU_ARCH=gfx1100
cmake --build build_hipvs -j2
```

Use the architecture actually supported by the installed hipVS build. The inspected C API must be enabled with `BUILD_CUVS_C_LIBRARY=ON` when building that dependency; another installation must provide all referenced symbols and the same API. The bridge uses the HIP compiler because the C headers include the CUDA-to-HIP compatibility shim. It links only the C API and HIP host runtime; it does not require the RAFT C++ development package referenced by some incomplete prebuilt CMake exports.

This command was compiled successfully with CMake 3.28.3 on the official AMD cloud image `seekdb-hipvs-nb-demo_v1.4.0@sha256:a1aa9d9378e054400c8ee7929572504dd4b1ecb4487e814ebfd87babd9f1456d`, Ubuntu 24.04.4, ROCm 7.2.4 and `gfx1100`. The container has a 16-CPU cgroup quota and 55 GiB memory limit even though `nproc` reports the 128 host CPUs. The image's C API library reports package version 01.00.00; its exact source revision has not been established, so it is not asserted to equal the inspected source revision above.

Set both variables **before starting seekdb**:

```sh
export SEEKDB_HIPVS_BUILD=required
export SEEKDB_HIPVS_LIBRARY=/absolute/path/to/build_hipvs/libseekdb_hipvs_bridge.so
```

Supported modes are `off` (the default), `auto` (attempt an eligible bulk build and log any CPU fallback), and `required` (fail an eligible build rather than silently use the CPU). Invalid option values fail explicitly. Settings are cached for the process and require restart to change. Incremental DML remains on its existing CPU path.

The initial bridge supports ordinary float32 L2 HNSW without extra columns, at least 1,024 rows, `2 <= M <= 256`, `4*M < rows`, and `ef_construction >= M`. The native importer accepts the tested packaged VSAG revisions `129b82c` (macOS) and `012a0f2` (Linux); an unverified version is rejected or explicitly falls back in `auto` mode. Both versions passed the CPU fixture and imports of all three real GPU-built artifacts, including independent queries, persistence and mutation. New VSAG versions require the interoperability test before enabling their layout. CPU behavior with `off` does not depend on this restriction.

`HIPVS_BUILD_COMPLETE` is logged only after actual bridge success, shape/VID validation and native import. It includes the AMD device, architecture, row count, parameters, serialized size and wall intervals. `cagra_build_wall_ms` includes transfer and synchronization; it is **not** kernel-only time. `adapter_build_total_ms` includes loading, graph construction, conversion and native import, but excludes the preceding database scan and subsequent snapshot storage. These log intervals are not the competition speedup metric. A fallback is recorded as `HIPVS_BUILD_NOT_USED` and must never be counted as a GPU result.

## Verified CPU interoperability

`unittest/lib/vector/test_hipvs_hnsw_format.cpp` links against the actual packaged VSAG library. It builds and serializes 5,000 vectors with reversed insertion order, imports them through the new validator with signed and wide VIDs, checks 400 exact self queries, round-trips serialization, and verifies later insertion/deletion. It also compares 200 independent queries against exact float64 L2 ground truth, reporting CPU/imported Recall@10. Its 0.95 floor is a local regression guard, not the unpublished competition threshold. Thirteen corrupt-input cases and a missing bridge are rejected. The validator and loader passed ASan/UBSan locally. The default fixture is CPU-generated; actual GPU artifact coverage is recorded below.

Compile this standalone test with the repository's VSAG headers/libraries and the two new `hipvs_hnsw_*.cpp` sources. On Linux the dynamic-library form is:

```sh
c++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -I src/oblib -isystem "$VSAG_PREFIX/include" \
  unittest/lib/vector/test_hipvs_hnsw_format.cpp \
  src/oblib/lib/vector/hipvs_hnsw_format.cpp \
  src/oblib/lib/vector/hipvs_hnsw_builder.cpp \
  -L "$VSAG_PREFIX/lib" -lvsag -ldl -o test_hipvs_hnsw_format
./test_hipvs_hnsw_format
```

The executed local test used macOS ARM64 and the packaged static VSAG dependencies. The main vector and observer translation units passed the existing Clang 19 compilation flags in syntax checks, followed by a full native build.

`unittest/sql/engine/vector/test_hipvs_build_integration.py` exercises empty snapshots, bulk HNSW, quantized HNSW, signed/wide document IDs, indexed queries and later insert/update/delete operations. On the native binary, `off` and `auto` each passed 19 checks including the existing ANN rewrite and AI column worker suites. `required` with a missing bridge and an invalid option each passed three controls: ordinary bulk HNSW fails explicitly while empty and quantized indexes remain usable. These are 44 test executions across four fresh process configurations. Actual `auto` fallback logs were checked; they are not GPU execution evidence.

The negative test exposed a pre-existing vector DDL error-propagation defect: the build-and-write pipeline recorded an error only in its auxiliary DDL field and returned success to the scheduler. The scheduler could then publish an empty snapshot. The pipeline now preserves the failing task result so `CREATE VECTOR INDEX` reports the construction failure. The same unchanged test failed before this fix and passed afterwards.

The final production code at `2927a906` was rebuilt on macOS and the same 44 SQL controls passed again. The AMD Linux build also passed all three SQL controls with a real required-mode GPU bridge: ordinary bulk HNSW logged a completed 2,048-row GPU build without fallback; empty and quantized indexes, signed/wide IDs and later insert/update/delete remained usable. Both CPU and GPU native servers needed forced termination after a 30-second graceful-shutdown wait; normal shutdown is not established. Separate repeated-build and intentional-crash checks are described below.

## Executed AMD bridge interoperability

`unittest/lib/vector/run_hipvs_bridge.py` generates reproducible float32 vectors, invokes the actual C ABI library, preserves each raw HNSW blob and input vector file, and records device/architecture, parameters, library/data/blob hashes and bridge-only timings. It does not claim SQL construction performance. Run it on the AMD node:

```sh
python unittest/lib/vector/run_hipvs_bridge.py \
  --library /absolute/path/to/build_hipvs/libseekdb_hipvs_bridge.so \
  --output gpu-fixture --rows 5000 --dimension 16 --m 8 \
  --ef-construction 80 --cpu-threads 8 --seed 20261027 --repeats 3
```

Three actual AMD builds completed 5,000 vectors with `M=8`, `ef_construction=80`, eight CPU threads and iterative CAGRA. The CMake-built bridge SHA-256 was `a12326709042856ac16c538ba41202a13726f8985f3bf694993ce2e56aa4d755`; input vectors were hashed `abcbd0646a4b62104dc1cab13d20f64a54173a394bd7278a692c29bc141382d1`. Whole bridge calls took 188.131, 39.964 and 40.168 ms; raw HNSW sizes were 745,872, 745,836 and 745,800 bytes. These are bridge-only samples, not a CPU/GPU speedup comparison.

Each actual AMD blob was then imported into the packaged native VSAG libraries on macOS and Linux with ASan/UBSan. All 400 self queries, signed/wide source VID mapping, serialization round trip, later insertion/deletion and 13 malformed-input rejections passed for each build on both systems. Independent Recall@10 was 1.0 for each GPU-built graph; the separate native CPU fixtures scored 0.999 on macOS and 0.9985 on Linux over the same vectors and 200 queries. This tests graph interoperability/quality through native VSAG, not the full seekdb SQL path. To reproduce that import using the compiled standalone test:

```sh
./test_hipvs_hnsw_format gpu-fixture/vectors.f32 gpu-fixture/index-1.bin
```

The optional external-fixture mode expects exactly 5,000 vectors of dimension 16 with `M=8` and `ef_construction=80`. Without arguments it retains the reordered CPU fixture. A separate `rocprofv3 --kernel-trace --hip-trace --memory-copy-trace --stats` execution of the same bridge produced kernel/HIP/memory-copy CSV traces with 139 kernel dispatches, including iterative float32 CAGRA construction and graph pruning/reverse-edge work. Its instrumented timing is separate from performance measurements.

Thirty consecutive bridge builds completed in one process with `--resources`. After each release, GPU free memory was exactly 51,126,468,608 bytes, open file descriptors remained seven and private temporary index files remained zero. Process RSS increased from 419,156 to 420,552 KiB; the last 20 samples ranged from 420,404 to 420,552 KiB. This observed bounded short-run behavior is not an unlimited leak-free guarantee.

`unittest/sql/engine/vector/check_hipvs_crash_recovery.py` builds an actual 2,048-row GPU SQL index, commits inserts/updates/deletion, intentionally kills the server, then restarts the same data directory with hipVS disabled. It verifies complete retained IDs, the vector index query plan, exact self queries and further DML; `/proc/PID/maps` confirms the bridge is not loaded during recovered queries. It passed twice on the AMD node, including the reusable command-line form. Test cleanup also uses SIGKILL and makes no graceful-shutdown claim.

```sh
python unittest/sql/engine/vector/check_hipvs_crash_recovery.py \
  --binary /absolute/path/to/seekdb --bridge /absolute/path/to/libseekdb_hipvs_bridge.so \
  --vectors gpu-fixture/vectors.f32 --base-dir /absolute/path/to/new-recovery-instance \
  --output recovery.json --source-revision IMPLEMENTATION_REVISION
```

## Required performance evidence

Use the same frozen vectors, query set, float32 precision, IDs, dimension, L2 metric, data-generation seed, CPU resources and comparable `M`/`ef_construction`/`ef_search` values. Record the implementation and dependency revisions and device information. Each run must begin with imported raw data and **no vector index**. Time fresh `CREATE VECTOR INDEX` through completed snapshot storage and the first successful query that actually uses the vector index; any lazy construction belongs inside this interval.

Run at least three fresh CPU builds with `off` and three GPU builds with `required`. Preserve every timing, verify complete data/ID coverage, check the actual query plan, and calculate Recall@K against independent exact ground truth. Record any failure or fallback. Compare medians and apply the officially published quality/speedup thresholds when available; no threshold or passing score is asserted here. Initialization, data generation/import and query-performance measurements are separate from index construction.

The executable harness is `unittest/sql/engine/vector/benchmark_hipvs_build.py`. Install NumPy and PyMySQL in an isolated environment (the local harness control used NumPy 2.5.4 and PyMySQL 1.2.3). Prepare the dataset once and use that exact file on the same AMD node for both modes:

```sh
python unittest/sql/engine/vector/benchmark_hipvs_build.py prepare \
  --data vectors.npz --rows 10000 --dim 128 --queries 200 --k 10 --seed 20261027
python unittest/sql/engine/vector/benchmark_hipvs_build.py run \
  --mode off --ef-search 512 --data vectors.npz --binary /absolute/path/to/seekdb \
  --base-dir /workspace/bench-cpu --output cpu.json --source-revision COMMIT
python unittest/sql/engine/vector/benchmark_hipvs_build.py run \
  --mode required --ef-search 512 --bridge /absolute/path/to/libseekdb_hipvs_bridge.so \
  --data vectors.npz --binary /absolute/path/to/seekdb \
  --base-dir /workspace/bench-gpu --output gpu.json --source-revision COMMIT
```

The example shape is a reproducibility example, not the unpublished official evaluation configuration. Each invocation owns a new disposable native instance and performs three fresh table imports/builds. Its endpoint includes `CREATE VECTOR INDEX`, verification of the vector query plan and the first indexed query. It records import/startup separately, raw neighbor IDs and independent float64 exact-L2 ground truth over the frozen float32 inputs. GPU mode requires matching completed-build logs and rejects any fallback; a rotated build log causes an explicit failure requiring reconciliation. The output binds dataset, binary, bridge and harness SHA-256 values, parameters, CPU affinity, versions and raw timings. Record the actual AMD hardware/ROCm/hipVS revisions and GPU profiling evidence alongside it; positive log timing alone does not replace hardware execution proof.

The harness itself completed a local CPU control with 2,048 vectors of dimension 16, 32 independent queries and three fresh builds. Recall@10 was 1.0 in every run; query-ready build times were 0.3197, 0.3090 and 0.3087 seconds. These macOS control timings verify the harness and are not the AMD CPU baseline or a claimed acceleration result.

## Executed SQL construction comparison

The measured node reported `AMD Radeon Graphics` / `gfx1100`, Ubuntu 24.04.4 and ROCm 7.2.4, with a 16-CPU cgroup quota and 55 GiB memory limit. Both modes used the same binary, frozen dataset, native defaults and CPU affinity 0-15; `OMP_NUM_THREADS=8` and `OPENBLAS_NUM_THREADS=8` were identical. The GPU completion logs reported four VSAG construction threads. The Linux VSAG revision was `012a0f2`; exact hipVS library source revision remains unknown.

The cloud build used public baseline `5bce9e06` plus only the AMD diff from private base `be35bcc` to implementation `2927a906`. Earlier private AI solutions were not uploaded. The full cumulative private tree was also compiled on macOS. Linux binary SHA-256 was `1c148f505bcec2329a591fa9969f8de7888f8e2da2730118350a0bb7e141048e`; bridge SHA-256 was `a12326709042856ac16c538ba41202a13726f8985f3bf694993ce2e56aa4d755`. The shared dataset SHA-256 was `796c992ac20e991223160cf41c984eb9b9cd5810d9bfb1e121580b84e1f92089` (NumPy 2.4.6, seed 20261027, 10,000 float32 vectors of dimension128, 200 independent queries, float64 exact L2 truth, K=10, signed/wide IDs).

Each run imported raw rows into a fresh table with no vector index. The interval includes CREATE INDEX, scan/transfer/synchronization/conversion/snapshot publication, vector-index plan verification and the first successful indexed query. `M=16` and `ef_construction=128` were equal in both modes. All GPU builds logged matching 10,000-row completion and no fallback. No build or slow sample was discarded.

| Configuration | CPU seconds, all three runs | GPU seconds, all three runs | Median speedup | Minimum CPU/GPU Recall@10 |
| --- | --- | --- | --- | --- |
| ef_search=512, overlay-backed data directories | 1.950611 / 1.826902 / 1.819806 | 0.748924 / 0.520260 / 0.624441 | 2.92566x | 0.993 / 0.9985 |
| Earlier ef_search=128, tmpfs-backed data directories | 1.848139 / 1.824960 / 1.822835 | 0.746687 / 0.520323 / 0.619147 | 2.94754x | 0.911 / 0.9055 |

The earlier lower-recall comparison is retained. These are separate paired configurations; the changed filesystem prevents attributing cross-row timing differences solely to query breadth. A subsequent CPU startup on the 4-GiB tmpfs failed for insufficient disk space before any timed build. Both modes of the higher-quality comparison then used fresh directories on the larger overlay filesystem. Original failure, raw query IDs and complete measurements remain preserved locally; the JSON results record binds their SHA-256 values and includes the raw timings, per-run Recall, configuration and GPU log evidence.

The inspected task does not specify a fixed speedup or quality threshold, so this report makes no official passing-score claim. Results cover these synthetic datasets on this one node; they do not establish large-scale or real-embedding performance. Native server cleanup required forced termination in both modes, as noted above.
