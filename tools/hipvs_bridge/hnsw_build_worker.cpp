/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include "hnsw_hierarchy.h"
#include <hip/hip_runtime.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

// Separate executable: hipVS's hnswlib and VSAG expose overlapping C++ symbols.
// Linking both in one process corrupts full-hierarchy conversion. Only the
// validated native graph crosses the process boundary, never a C++ object.
struct Resources {
  cuvsResources_t res = 0;
  cuvsCagraIndexParams_t cp = nullptr;
  cuvsCagraIndex_t cagra = nullptr;
  ~Resources() {
    if (cagra) cuvsCagraIndexDestroy(cagra);
    if (cp) cuvsCagraIndexParamsDestroy(cp);
    if (res) cuvsResourcesDestroy(res);
  }
};

static void check(cuvsError_t rc, const char *stage) {
  if (rc != CUVS_SUCCESS) throw std::runtime_error(std::string(stage) + ": " + cuvsGetLastErrorText());
}

int main()
{
  try {
    struct stat input_stat{};
    if (fstat(STDIN_FILENO, &input_stat) != 0 || !S_ISREG(input_stat.st_mode))
      throw std::runtime_error("hipVS build input must be a private regular IPC file");
    uint64_t header[6];
    if (fread(header, sizeof(header), 1, stdin) != 1 || header[0] != 0x31444c4955425048ULL)
      throw std::runtime_error("invalid hipVS build protocol header");
    const uint64_t n = header[1], dim = header[2], degree = header[3], ef = header[4], threads = header[5];
    if (n < 256 || n > std::numeric_limits<uint32_t>::max() || dim < 1 || dim > 65536 ||
        degree < 4 || degree > 128 || n <= 4 * degree || ef < 1 || ef > 1000000 || threads < 1 || threads > 64 ||
        n > std::numeric_limits<size_t>::max() / dim / sizeof(float))
      throw std::runtime_error("unsupported hipVS build shape");
    if (static_cast<uint64_t>(input_stat.st_size) != sizeof(header) + n * dim * sizeof(float))
      throw std::runtime_error("hipVS input file size does not match its shape");
    std::vector<float> vectors(n * dim);
    if (fread(vectors.data(), sizeof(float), vectors.size(), stdin) != vectors.size() || fgetc(stdin) != EOF)
      throw std::runtime_error("incomplete or oversized hipVS input");
    for (float v : vectors) if (!std::isfinite(v)) throw std::runtime_error("nonfinite input vector");
    int devices = 0;
    if (hipGetDeviceCount(&devices) != hipSuccess || devices < 1) throw std::runtime_error("no AMD GPU available");
    Resources r;
    check(cuvsResourcesCreate(&r.res), "resources");
    check(cuvsCagraIndexParamsCreate(&r.cp), "CAGRA parameters");
    r.cp->graph_degree = degree * 2;
    // Avoid IVF-PQ startup on small snapshots. Use its refined candidate graph
    // at larger cardinalities, where fixed NN-descent can lose recall.
    const bool use_nn_descent = n < 262144;
    r.cp->build_algo = use_nn_descent ? NN_DESCENT : IVF_PQ;
    r.cp->nn_descent_niter = 20;
    r.cp->intermediate_graph_degree = degree * (use_nn_descent ? 4 : 8);
    if (!use_nn_descent) {
      // The C API destroys graph_build_params; transfer a heap object to it.
      if (r.cp->graph_build_params == nullptr) r.cp->graph_build_params = new cuvsIvfPqParams{};
      r.cp->graph_build_params->refinement_rate = 4.0f;
    }
    check(cuvsCagraIndexCreate(&r.cagra), "CAGRA index");
    int64_t shape[2] = {static_cast<int64_t>(n), static_cast<int64_t>(dim)};
    DLManagedTensor data{};
    data.dl_tensor.data = vectors.data(); data.dl_tensor.device = {kDLCPU, 0};
    data.dl_tensor.ndim = 2; data.dl_tensor.dtype = {kDLFloat, 32, 1}; data.dl_tensor.shape = shape;
    const auto begin = std::chrono::steady_clock::now();
    check(cuvsCagraBuild(r.res, r.cp, &data, r.cagra), "GPU CAGRA build");
    if (hipDeviceSynchronize() != hipSuccess) throw std::runtime_error("GPU build synchronization failed");
    const auto built = std::chrono::steady_clock::now();
    DLManagedTensor graph_view{};
    check(cuvsCagraIndexGetGraph(r.cagra, &graph_view), "GPU graph view");
    std::vector<uint32_t> graph(n*degree*2);
    if(graph_view.dl_tensor.ndim!=2 || graph_view.dl_tensor.shape[0]!=static_cast<int64_t>(n) ||
       graph_view.dl_tensor.shape[1]!=static_cast<int64_t>(degree*2) || graph_view.dl_tensor.dtype.bits!=32)
      throw std::runtime_error("invalid GPU graph view shape");
    auto copy_status=hipMemcpy(graph.data(),graph_view.dl_tensor.data,graph.size()*sizeof(uint32_t),hipMemcpyDeviceToHost);
    if(graph_view.deleter) graph_view.deleter(&graph_view);
    if(copy_status!=hipSuccess)throw std::runtime_error("GPU graph transfer failed");
    auto hierarchy=seekdb_hipvs::serialize_hierarchy(stdout,graph,vectors,n,dim,degree,ef);
    const auto done = std::chrono::steady_clock::now();
    fprintf(stderr, "[HIPVS_HNSW_BUILD] backend=hipvs rows=%llu dim=%llu m=%llu gpu_ms=%.3f conversion_and_serialize_ms=%.3f fallback=0 algorithm_request=%s components=%llu upper_nodes=%llu\n",
      static_cast<unsigned long long>(n), static_cast<unsigned long long>(dim), static_cast<unsigned long long>(degree),
      std::chrono::duration<double, std::milli>(built - begin).count(),
      std::chrono::duration<double, std::milli>(done - built).count(), use_nn_descent ? "nn_descent_20" : "ivf_pq",
      static_cast<unsigned long long>(hierarchy.components),static_cast<unsigned long long>(hierarchy.upper_nodes));
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "[HIPVS_HNSW_BUILD_FAILED] %s; no CPU fallback\n", e.what());
    return 1;
  }
}
