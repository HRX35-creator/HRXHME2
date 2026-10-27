# Optional hipVS HNSW bulk construction

This implementation is in development. The full native seekdb build, native SQL controls, CPU interoperability and AddressSanitizer/UndefinedBehaviorSanitizer checks have passed. An AMD bridge build, GPU execution, CPU/GPU end-to-end speedup and competition acceptance have **not** been verified.

The default CPU path is unchanged. An explicit process option stages a complete ordinary HNSW snapshot, calls hipVS to construct its base graph on AMD hardware, converts it to a full HNSW hierarchy, validates its contents and imports it into a new native VSAG index. Queries, filtering, incremental DML and persistence then use the existing VSAG implementation. Quantized, HGraph and sparse indexes retain their existing paths.

## Construction path

Initial DDL uses `ObHNSWIndexAppendBufferOperator::append_row` and finalizes through `check_snap_hnswsq_index` before serialization. Async snapshot rebuilds also use this finalization point. With the option enabled, `add_snap_index` stages VIDs, vectors and any extra information instead of building an ordinary HNSW through repeated CPU `Add` calls. Only successful construction publishes the native index; failures release the staged state through existing snapshot cleanup.

The separate ROCm bridge calls the hipVS C API:

1. Build CAGRA with float32 L2 vectors, NN-Descent, output degree `2*M`, intermediate degree `max(64,4*M)` and 20 iterations. The C API transfers the host tensor; stream synchronization completes before the build interval ends.
2. Use `cuvsHnswFromCagraWithDataset` with a CPU-built upper hierarchy, the original host vectors, the requested `ef_construction` and the same VSAG building thread count. The expensive base graph comes from hipVS; no GPU search is used.
3. Serialize the full mutable HNSW to a private temporary file, read it and return it through the versioned C ABI. Per-build indexes, handles, streams and temporary files have scoped cleanup. The bridge library stays loaded between builds.

The importer verifies the complete serialized shape, original float32 vectors, a one-to-one ordinal mapping, all graph edges and upper-layer targets. It replaces each stored ordinal with its source VID, including when parallel hierarchy construction reordered internal nodes. It does not infer IDs from internal row positions. A fresh native index is published only after successful deserialization and an exact element-count check.

References: [AMD hipVS HNSW API](https://rocm.docs.amd.com/projects/hipVS/en/latest/reference/cpp_api/neighbors_hnsw.html), [the inspected hipVS source revision](https://github.com/AMD-Ecosystem/hipVS/tree/f96a663d0a4441d6f5344bd3ef552b71ae7b370b), and [the packaged VSAG source revision](https://github.com/antgroup/vsag/tree/129b82cff36a9736d06e1ecba5a608a671f6e948).

## Build and enable

Build seekdb normally. Its default build has no ROCm dependency. On Linux with an installed AMD ROCm stack and a matching hipVS installation that exports `cuvs::c_api`:

```sh
cmake -S deps/hipvs -B build_hipvs -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/hipvs/prefix
cmake --build build_hipvs -j2
```

The inspected hipVS C API must be enabled with `BUILD_CUVS_C_LIBRARY=ON` when building that dependency. A different installation must provide all referenced symbols and the same API. This bridge has not yet been compiled on an actual ROCm installation.

Set both variables **before starting seekdb**:

```sh
export SEEKDB_HIPVS_BUILD=required
export SEEKDB_HIPVS_LIBRARY=/absolute/path/to/build_hipvs/libseekdb_hipvs_bridge.so
```

Supported modes are `off` (the default), `auto` (attempt an eligible bulk build and log any CPU fallback), and `required` (fail an eligible build rather than silently use the CPU). Invalid option values fail explicitly. Settings are cached for the process and require restart to change. Incremental DML remains on its existing CPU path.

The initial bridge supports ordinary float32 L2 HNSW without extra columns, at least 1,024 rows, `2 <= M <= 256`, `4*M < rows`, and `ef_construction >= M`. The native importer currently accepts the verified packaged VSAG revision `129b82c`; an unverified version is rejected or explicitly falls back in `auto` mode. New VSAG versions require the interoperability test before enabling their layout. CPU behavior with `off` does not depend on this restriction.

`HIPVS_BUILD_COMPLETE` is logged only after actual bridge success, shape/VID validation and native import. It includes the AMD device, architecture, row count, parameters, serialized size and wall intervals. `cagra_build_wall_ms` includes transfer and synchronization; it is **not** kernel-only time. `adapter_build_total_ms` includes loading, graph construction, conversion and native import, but excludes the preceding database scan and subsequent snapshot storage. These log intervals are not the competition speedup metric. A fallback is recorded as `HIPVS_BUILD_NOT_USED` and must never be counted as a GPU result.

## Verified CPU interoperability

`unittest/lib/vector/test_hipvs_hnsw_format.cpp` links against the actual packaged VSAG library. It builds and serializes 5,000 vectors with reversed insertion order, imports them through the new validator with signed and wide VIDs, checks 400 exact self queries, round-trips serialization, and verifies later insertion/deletion. Thirteen corrupt-input cases and a missing bridge are rejected. The validator and loader passed ASan/UBSan locally. These checks use a CPU-generated graph; they do not prove that hipVS produced a compatible graph or establish Recall@K on independent queries.

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

The local test servers needed forced termination after a 30-second graceful-shutdown wait. These results do not establish normal shutdown behavior or GPU resource stability. Actual GPU compilation/execution remains required before merge.

## Required performance evidence

Use the same frozen vectors, query set, float32 precision, IDs, dimension, L2 metric, data-generation seed, CPU resources and comparable `M`/`ef_construction`/`ef_search` values. Record the implementation and dependency revisions and device information. Each run must begin with imported raw data and **no vector index**. Time fresh `CREATE VECTOR INDEX` through completed snapshot storage and the first successful query that actually uses the vector index; any lazy construction belongs inside this interval.

Run at least three fresh CPU builds with `off` and three GPU builds with `required`. Preserve every timing, verify complete data/ID coverage, check the actual query plan, and calculate Recall@K against independent exact ground truth. Record any failure or fallback. Compare medians and apply the officially published quality/speedup thresholds when available; no threshold or passing score is asserted here. Initialization, data generation/import and query-performance measurements are separate from index construction.

The executable harness is `unittest/sql/engine/vector/benchmark_hipvs_build.py`. Install NumPy and PyMySQL in an isolated environment (the local harness control used NumPy 2.5.4 and PyMySQL 1.2.3). Prepare the dataset once and use that exact file on the same AMD node for both modes:

```sh
python unittest/sql/engine/vector/benchmark_hipvs_build.py prepare \
  --data vectors.npz --rows 10000 --dim 128 --queries 200 --k 10 --seed 20261027
python unittest/sql/engine/vector/benchmark_hipvs_build.py run \
  --mode off --data vectors.npz --binary /absolute/path/to/seekdb \
  --base-dir /workspace/bench-cpu --output cpu.json --source-revision COMMIT
python unittest/sql/engine/vector/benchmark_hipvs_build.py run \
  --mode required --bridge /absolute/path/to/libseekdb_hipvs_bridge.so \
  --data vectors.npz --binary /absolute/path/to/seekdb \
  --base-dir /workspace/bench-gpu --output gpu.json --source-revision COMMIT
```

The example shape is a reproducibility example, not the unpublished official evaluation configuration. Each invocation owns a new disposable native instance and performs three fresh table imports/builds. Its endpoint includes `CREATE VECTOR INDEX`, verification of the vector query plan and the first indexed query. It records import/startup separately, raw neighbor IDs and independent float64 exact-L2 ground truth over the frozen float32 inputs. GPU mode requires matching completed-build logs and rejects any fallback; a rotated build log causes an explicit failure requiring reconciliation. The output binds dataset, binary, bridge and harness SHA-256 values, parameters, CPU affinity, versions and raw timings. Record the actual AMD hardware/ROCm/hipVS revisions and GPU profiling evidence alongside it; positive log timing alone does not replace hardware execution proof.

The harness itself completed a local CPU control with 2,048 vectors of dimension 16, 32 independent queries and three fresh builds. Recall@10 was 1.0 in every run; query-ready build times were 0.3197, 0.3090 and 0.3087 seconds. These macOS control timings verify the harness and are not the AMD CPU baseline or a claimed acceleration result.
