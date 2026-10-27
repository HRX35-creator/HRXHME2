/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "hipvs_hnsw_builder.h"
#include "hipvs_build_api.h"
#include "vsag/constants.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace oceanbase {
namespace common {
namespace obvsag {

HipvsBuildMode hipvs_build_mode()
{
  static const HipvsBuildMode mode = [] {
    const char *setting = std::getenv("SEEKDB_HIPVS_BUILD");
    if (setting == nullptr || std::strcmp(setting, "off") == 0) { return HipvsBuildMode::DISABLED; }
    if (std::strcmp(setting, "auto") == 0) { return HipvsBuildMode::PREFER; }
    if (std::strcmp(setting, "required") == 0) { return HipvsBuildMode::REQUIRED; }
    return HipvsBuildMode::INVALID;
  }();
  return mode;
}

bool build_hipvs_hnsw(const HipvsHnswSource &source, uint32_t cpu_threads,
                      vsag::BinarySet &output, HipvsBuildReport &report)
{
  report = HipvsBuildReport{};
  output = vsag::BinarySet{};
  const auto reject = [&report](const char *message) { report.error = message; return false; };
#if defined(__unix__) || defined(__APPLE__)
  using Build = int (*)(const SeekdbHipvsBuildRequestV1 *, SeekdbHipvsBuildResultV1 *);
  using Release = void (*)(void *);
  struct Runtime {
    std::shared_ptr<void> library;
    Build build = nullptr;
    Release release = nullptr;
    std::string error;
  };
  // Keep the ROCm runtime loaded across builds, rather than repeatedly tearing
  // down its process-wide contexts. Settings take effect on process restart.
  static const auto cached = [] {
    auto runtime = std::make_shared<Runtime>();
    const char *path = std::getenv("SEEKDB_HIPVS_LIBRARY");
    if (path == nullptr || path[0] != '/') {
      runtime->error = "SEEKDB_HIPVS_LIBRARY must name an absolute bridge library path";
      return runtime;
    }
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      const char *detail = dlerror();
      runtime->error = detail == nullptr ? "unable to load hipVS build bridge" : detail;
      return runtime;
    }
    runtime->library = std::shared_ptr<void>(handle, [](void *p) { dlclose(p); });
    runtime->build = reinterpret_cast<Build>(dlsym(handle, "seekdb_hipvs_build_hnsw_v1"));
    runtime->release = reinterpret_cast<Release>(dlsym(handle, "seekdb_hipvs_free_v1"));
    if (runtime->build == nullptr || runtime->release == nullptr) {
      runtime->error = "hipVS bridge does not expose ABI v1 build/release functions";
    }
    return runtime;
  }();
  if (!cached->error.empty()) { report.error = cached->error; return false; }
  const auto library = cached->library;
  const auto build = cached->build;
  const auto release = cached->release;
  SeekdbHipvsBuildRequestV1 request{1, sizeof(request), source.rows, source.dimension,
    source.max_degree, source.ef_construction, cpu_threads, source.vectors};
  SeekdbHipvsBuildResultV1 result{};
  result.abi_version = 1; result.struct_size = sizeof(result);
  const int status = build(&request, &result);
  const auto free_buffer = [library, release](int8_t *p) { (void)library; release(p); };
  std::unique_ptr<int8_t[], decltype(free_buffer)> bytes(reinterpret_cast<int8_t *>(result.hnsw_data), free_buffer);
  if (result.abi_version != 1 || result.struct_size != sizeof(result)) { return reject("hipVS bridge returned a different ABI layout"); }
  if (status != 0) {
    report.error.assign(result.error, strnlen(result.error, sizeof(result.error)));
    if (report.error.empty()) { report.error = "hipVS build failed without an error detail"; }
    return false;
  }
  if (result.hnsw_data == nullptr || result.hnsw_bytes > std::numeric_limits<size_t>::max()
      || result.device_id < 0 || std::strncmp(result.gcn_arch, "gfx", 3) != 0
      || !std::isfinite(result.cagra_build_ms) || result.cagra_build_ms <= 0
      || !std::isfinite(result.hierarchy_serialize_ms) || result.hierarchy_serialize_ms < 0) {
    return reject("hipVS bridge did not return a completed AMD build and valid timings");
  }
  if (!prepare_hipvs_hnsw_blob(result.hnsw_data, size_t(result.hnsw_bytes), source, report.error)) { return false; }
  report.device.assign(result.device_name, strnlen(result.device_name, sizeof(result.device_name)));
  report.architecture.assign(result.gcn_arch, strnlen(result.gcn_arch, sizeof(result.gcn_arch)));
  report.bytes = result.hnsw_bytes;
  report.cagra_build_ms = result.cagra_build_ms;
  report.hierarchy_serialize_ms = result.hierarchy_serialize_ms;
  report.nn_descent_iterations = result.nn_descent_iterations;
  // Keep the originating DSO alive until its allocator releases the buffer.
  std::shared_ptr<int8_t[]> owned(std::move(bytes));
  output.Set(vsag::HNSW_DATA, {std::move(owned), result.hnsw_bytes});
  return true;
#else
  (void)source; (void)cpu_threads;
  return reject("hipVS build bridge is not available on this platform");
#endif
}

}  // namespace obvsag
}  // namespace common
}  // namespace oceanbase
