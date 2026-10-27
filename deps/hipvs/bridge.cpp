/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "lib/vector/hipvs_build_api.h"
#include <hip/hip_runtime_api.h>
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void checked(cuvsError_t status, const char *operation)
{
  if (status != CUVS_SUCCESS) {
    const char *detail = cuvsGetLastErrorText();
    throw std::runtime_error(std::string(operation) + ": " + (detail == nullptr ? "hipVS failed" : detail));
  }
}
void checked(hipError_t status, const char *operation)
{
  if (status != hipSuccess) { throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(status)); }
}
struct BuildResources {
  cuvsResources_t resources = 0;
  cuvsCagraIndexParams_t cagra_params = nullptr;
  cuvsHnswIndexParams_t hnsw_params = nullptr;
  cuvsCagraIndex_t cagra = nullptr;
  cuvsHnswIndex_t hnsw = nullptr;
  ~BuildResources()
  {
    if (resources != 0) { cuvsStreamSync(resources); }
    if (hnsw != nullptr) { cuvsHnswIndexDestroy(hnsw); }
    if (cagra != nullptr) { cuvsCagraIndexDestroy(cagra); }
    if (hnsw_params != nullptr) { cuvsHnswIndexParamsDestroy(hnsw_params); }
    if (cagra_params != nullptr) { cuvsCagraIndexParamsDestroy(cagra_params); }
    if (resources != 0) { cuvsResourcesDestroy(resources); }
  }
};
struct TemporaryIndex {
  char path[64] = "/tmp/seekdb-hipvs-hnsw-XXXXXX";
  int fd = -1;
  TemporaryIndex()
  {
    fd = mkostemp(path, O_CLOEXEC);
    if (fd == -1) { throw std::runtime_error("cannot create private hipVS temporary index"); }
  }
  ~TemporaryIndex() { if (fd != -1) { close(fd); unlink(path); } }
};

// Iterative CAGRA construction includes each row itself in the base graph on
// the tested hipVS image. A self edge carries no connectivity and is invalid
// for the native mutable HNSW. Remove only those edges during conversion;
// retain every non-self edge, all vectors/labels and the full upper hierarchy.
// The core importer still validates every edge before publishing the index.
void remove_base_self_links(uint8_t *bytes, size_t length, const SeekdbHipvsBuildRequestV1 &request)
{
  const auto read64 = [](const uint8_t *p) { uint64_t value; std::memcpy(&value, p, sizeof(value)); return value; };
  const auto read32 = [](const uint8_t *p) { uint32_t value; std::memcpy(&value, p, sizeof(value)); return value; };
  const auto write32 = [](uint8_t *p, uint32_t value) { std::memcpy(p, &value, sizeof(value)); };
  const uint64_t degree = 2 * request.max_degree;
  const uint64_t stride = 4 + degree * 4 + uint64_t(request.dimension) * 4 + 8;
  if (length < 96 || read64(bytes + 16) != request.rows || read64(bytes + 24) != stride
      || request.rows > (length - 96) / stride) {
    throw std::runtime_error("cannot normalize an invalid hipVS HNSW base layer");
  }
  for (uint64_t i = 0; i < request.rows; ++i) {
    uint8_t *record = bytes + 96 + i * stride;
    const uint32_t count = read32(record);
    if (count > degree) { throw std::runtime_error("invalid hipVS base-layer link count"); }
    uint32_t kept = 0;
    for (uint32_t j = 0; j < count; ++j) {
      const uint32_t neighbor = read32(record + 4 + j * 4);
      if (neighbor >= request.rows) { throw std::runtime_error("invalid hipVS base-layer link target"); }
      if (neighbor != i) { write32(record + 4 + kept++ * 4, neighbor); }
    }
    for (uint32_t j = kept; j < count; ++j) { write32(record + 4 + j * 4, 0); }
    write32(record, kept);
  }
}
}  // namespace

extern "C" void seekdb_hipvs_free_v1(void *buffer) { std::free(buffer); }

extern "C" int seekdb_hipvs_build_hnsw_v1(const SeekdbHipvsBuildRequestV1 *request,
                                          SeekdbHipvsBuildResultV1 *result)
{
  if (result == nullptr || result->abi_version != 1 || result->struct_size != sizeof(*result)) { return 1; }
  std::memset(result, 0, sizeof(*result));
  result->abi_version = 1; result->struct_size = sizeof(*result); result->device_id = -1;
  try {
    if (request == nullptr || request->abi_version != 1 || request->struct_size != sizeof(*request)
        || request->vectors == nullptr || request->rows < 1024 || request->rows > UINT32_MAX
        || request->dimension == 0 || request->dimension > 65536 || request->max_degree < 2
        || request->max_degree > 256 || 4 * request->max_degree >= request->rows
        || request->ef_construction < request->max_degree || request->ef_construction > INT32_MAX
        || request->cpu_threads == 0 || request->cpu_threads > 200) {
      throw std::runtime_error("unsupported hipVS float32 L2 batch shape or construction parameters");
    }
    const uint64_t elements = request->rows * request->dimension;
    for (uint64_t i = 0; i < elements; ++i) {
      if (!std::isfinite(request->vectors[i])) { throw std::runtime_error("hipVS source contains a non-finite float"); }
    }
    int device = -1;
    checked(hipGetDevice(&device), "hipGetDevice");
    hipDeviceProp_t properties{};
    checked(hipGetDeviceProperties(&properties, device), "hipGetDeviceProperties");
    if (std::strncmp(properties.gcnArchName, "gfx", 3) != 0) { throw std::runtime_error("an AMD gfx device is required"); }
    result->device_id = device;
    std::snprintf(result->device_name, sizeof(result->device_name), "%.*s", int(sizeof(result->device_name) - 1), properties.name);
    std::snprintf(result->gcn_arch, sizeof(result->gcn_arch), "%.*s", int(sizeof(result->gcn_arch) - 1), properties.gcnArchName);
    BuildResources handles;
    checked(cuvsResourcesCreate(&handles.resources), "cuvsResourcesCreate");
    int resource_device = -1;
    checked(cuvsDeviceIdGet(handles.resources, &resource_device), "cuvsDeviceIdGet");
    if (resource_device != device) { throw std::runtime_error("hipVS resources selected an unexpected GPU device"); }
    checked(cuvsCagraIndexParamsCreate(&handles.cagra_params), "cuvsCagraIndexParamsCreate");
    checked(cuvsHnswIndexParamsCreate(&handles.hnsw_params), "cuvsHnswIndexParamsCreate");
    checked(cuvsCagraIndexCreate(&handles.cagra), "cuvsCagraIndexCreate");
    checked(cuvsHnswIndexCreate(&handles.hnsw), "cuvsHnswIndexCreate");
    handles.cagra_params->metric = cuvsDistanceType::L2Expanded;
    handles.cagra_params->graph_degree = 2 * request->max_degree;
    handles.cagra_params->intermediate_graph_degree = std::max<uint32_t>(64, 4 * request->max_degree);
    // NN-Descent on the supplied image uses half-precision scratch vectors.
    // The iterative construction kernels retain float32 dataset/distances.
    handles.cagra_params->build_algo = cuvsCagraGraphBuildAlgo::ITERATIVE_CAGRA_SEARCH;
    handles.cagra_params->compression = nullptr;
    handles.hnsw_params->hierarchy = cuvsHnswHierarchy::CPU;
    handles.hnsw_params->ef_construction = int(request->ef_construction);
    handles.hnsw_params->num_threads = int(request->cpu_threads);
    int64_t shape[2] = {int64_t(request->rows), int64_t(request->dimension)};
    DLManagedTensor tensor{};
    tensor.dl_tensor.data = const_cast<float *>(request->vectors);
    tensor.dl_tensor.device = DLDevice{kDLCPU, 0};
    tensor.dl_tensor.ndim = 2;
    tensor.dl_tensor.dtype = DLDataType{kDLFloat, 32, 1};
    tensor.dl_tensor.shape = shape;
    const auto build_start = Clock::now();
    // The host tensor is copied by hipVS. Build and synchronization stay inside
    // this measured interval. Iterative CAGRA uses search internally to build
    // the graph; serving queries and first-query lazy builds are not used.
    checked(cuvsCagraBuild(handles.resources, handles.cagra_params, &tensor, handles.cagra), "cuvsCagraBuild");
    checked(cuvsStreamSync(handles.resources), "cuvsStreamSync after CAGRA build");
    result->cagra_build_ms = milliseconds(build_start);
    const auto hierarchy_start = Clock::now();
    checked(cuvsHnswFromCagraWithDataset(handles.resources, handles.hnsw_params, handles.cagra, handles.hnsw, &tensor),
      "cuvsHnswFromCagraWithDataset");
    TemporaryIndex file;
    checked(cuvsHnswSerialize(handles.resources, file.path, handles.hnsw), "cuvsHnswSerialize");
    checked(cuvsStreamSync(handles.resources), "cuvsStreamSync after HNSW conversion");
    struct stat info{};
    if (fstat(file.fd, &info) != 0 || info.st_size < 96) { throw std::runtime_error("invalid hipVS serialized index size"); }
    const uint64_t maximum = 96 + request->rows * (4 * request->dimension + 8 * request->max_degree + 12
      + 4 + 64 * (4 * request->max_degree + 4));
    if (uint64_t(info.st_size) > maximum || uint64_t(info.st_size) > std::numeric_limits<size_t>::max()) {
      throw std::runtime_error("hipVS serialized index exceeds the full HNSW shape bound");
    }
    std::unique_ptr<uint8_t, decltype(&std::free)> buffer(
      static_cast<uint8_t *>(std::malloc(size_t(info.st_size))), &std::free);
    if (!buffer) { throw std::bad_alloc(); }
    size_t offset = 0;
    while (offset < size_t(info.st_size)) {
      const ssize_t count = read(file.fd, buffer.get() + offset, std::min<size_t>(1048576, size_t(info.st_size) - offset));
      if (count < 0 && errno == EINTR) { continue; }
      if (count <= 0) { throw std::runtime_error("cannot read completed hipVS HNSW index"); }
      offset += size_t(count);
    }
    remove_base_self_links(buffer.get(), size_t(info.st_size), *request);
    result->hierarchy_serialize_ms = milliseconds(hierarchy_start);
    result->nn_descent_iterations = 0;
    result->hnsw_bytes = uint64_t(info.st_size);
    result->hnsw_data = buffer.release();
    return 0;
  } catch (const std::exception &e) {
    std::snprintf(result->error, sizeof(result->error), "%s", e.what());
  } catch (...) {
    std::snprintf(result->error, sizeof(result->error), "unknown hipVS build exception");
  }
  return 1;
}
